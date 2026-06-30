#include "DeckLinkDevice.h"
#include <comdef.h>
#include <iostream>
#include <sstream>
#include "DeckLinkAPI_i.c" // Helper for GUIDs
#include <chrono>
#include <thread>

// Shared logging helper (tag + message to stdout)
static void DLLog(const char* tag, const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_info;
    localtime_s(&tm_info, &t);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_info);
    std::cout << "\n" << timebuf << " " << tag << " [DeckLink] " << msg << std::endl;
}

// Helper for COM release
template <typename T>
void SafeRelease(T** ppT)
{
    if (*ppT)
    {
        (*ppT)->Release();
        *ppT = nullptr;
    }
}

DeckLinkDevice::DeckLinkDevice()
    : m_refCount(1),
      m_deckLink(nullptr),
      m_deckLinkOutput(nullptr),
      m_currentFrame(nullptr),
      m_frameCompletedSignal(false),
      m_lastCompletionResult(bmdOutputFrameCompleted),
      m_totalFramesScheduled(0),
      m_timeScale(30000),     // Time units per second
      m_frameDuration(1001),  // Duration per FRAME (for 59.94i)
      m_renderCallback(nullptr),
      m_isSimulated(false),
      m_simulationRunning(false)
{
}

void DeckLinkDevice::SetRenderCallback(RenderCallback callback)
{
    m_renderCallback = callback;
}

DeckLinkDevice::~DeckLinkDevice()
{
    StopOutput();
    
    // Clean up frame queue
    while (!m_frameQueue.empty()) {
        IDeckLinkMutableVideoFrame* frame = m_frameQueue.front();
        m_frameQueue.pop_front();
        if (frame) frame->Release();
    }
    
    if (m_currentFrame) {
        m_currentFrame->Release();
        m_currentFrame = nullptr;
    }
    
    SafeRelease(&m_deckLinkOutput);
    SafeRelease(&m_deckLink);
}

bool DeckLinkDevice::Initialize(const std::string& format)
{
    IDeckLinkIterator* deckLinkIterator = nullptr;
    HRESULT result = CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator, (void**)&deckLinkIterator);

    if (FAILED(result))
    {
        std::cerr << "DeckLink drivers may not be installed. Switching to SIMULATOR MODE." << std::endl;
        m_isSimulated = true;
        return true; // Return true as we successfully initialized "Simulated" device
    }

    // Get the first device
    if (deckLinkIterator->Next(&m_deckLink) != S_OK)
    {
        std::cerr << "No DeckLink device found. Switching to SIMULATOR MODE." << std::endl;
        deckLinkIterator->Release();
        m_isSimulated = true;
        return true;
    }

    deckLinkIterator->Release();

    // Get Output Interface
    if (m_deckLink->QueryInterface(IID_IDeckLinkOutput, (void**)&m_deckLinkOutput) != S_OK)
    {
        return false;
    }

    // Check video mode (59.94i / 1080i59.94 or 50i)
    BMDDisplayMode displayMode = bmdModeHD1080i5994;
    m_timeScale = 30000;
    m_frameDuration = 1001;

    if (format == "50i") {
        displayMode = bmdModeHD1080i50;
        m_timeScale = 25000;
        m_frameDuration = 1000;
    }
    
    result = m_deckLinkOutput->EnableVideoOutput(displayMode, bmdVideoOutputFlagDefault);
    if (FAILED(result))
    {
        std::cerr << "Could not enable video output for " << format << std::endl;
        return false;
    }

    // Enable External Keying using IDeckLinkKeyer
    // This will output Fill on SDI Out and Key on SDI Out 2
    IDeckLinkKeyer* keyer = nullptr;
    if (m_deckLink->QueryInterface(IID_IDeckLinkKeyer, (void**)&keyer) == S_OK)
    {
        // Enable(true) = External Keying, Enable(false) = Internal Keying
        result = keyer->Enable(true);  // TRUE for External Key!
        if (SUCCEEDED(result))
        {
            keyer->SetLevel(255);  // Full opacity
            std::cout << "External Keying enabled successfully! Fill on SDI Out, Key on SDI Out 2." << std::endl;
        }
        else
        {
            std::cerr << "Warning: Could not enable external keying - result = " << std::hex << result << std::endl;
        }
        keyer->Release();
    }
    else
    {
        std::cerr << "Warning: IDeckLinkKeyer interface not available on this device." << std::endl;
    }

    m_deckLinkOutput->SetScheduledFrameCompletionCallback(this);
    
    return true;
}

bool DeckLinkDevice::StartOutput()
{
    if (m_isSimulated) {
        if (m_simulationRunning) return true;
        m_simulationRunning = true;
        m_simulationThread = std::thread(&DeckLinkDevice::SimulationLoop, this);
        std::cout << "[DeckLink] Started SIMULATOR Mode (29.97fps)" << std::endl;
        return true;
    }

    if (!m_deckLinkOutput) return false;

    // Create Frame Queue (DeckLink SDK pattern - use 5 frames like reference)
    const int kFrameQueueSize = 5;
    
    for (int i = 0; i < kFrameQueueSize; i++)
    {
        IDeckLinkMutableVideoFrame* frame = nullptr;
        HRESULT hr = m_deckLinkOutput->CreateVideoFrame(
            1920, 1080,
            1920 * 4,  // ARGB = 4 bytes per pixel
            bmdFormat8BitARGB, 
            bmdFrameFlagDefault, 
            &frame);
            
        if (FAILED(hr)) {
            // Clean up frames created so far
            while (!m_frameQueue.empty()) {
                m_frameQueue.front()->Release();
                m_frameQueue.pop_front();
            }
            return false;
        }

        // Initialize with opaque black
        void* pBuffer = nullptr;
        IDeckLinkVideoBuffer* videoBuffer = nullptr;
        if (frame->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&videoBuffer) == S_OK)
        {
            if (videoBuffer->GetBytes(&pBuffer) == S_OK)
            {
                uint32_t* p32 = (uint32_t*)pBuffer;
                long pixels = 1920 * 1080;
                for(long j=0; j<pixels; ++j) {
                    *p32++ = 0xFF000000; // A=255, R=0, G=0, B=0 (opaque black)
                }
            }
            videoBuffer->Release();
        }
        
        m_frameQueue.push_back(frame);
    }

    // Preroll frames (schedule first batch)
    for (int i = 0; i < kFrameQueueSize; i++)
    {
        // Get frame from queue
        IDeckLinkMutableVideoFrame* frame = m_frameQueue.front();
        m_frameQueue.pop_front();
        m_frameQueue.push_back(frame);  // Return to back of queue
        
        // Schedule it
        m_deckLinkOutput->ScheduleVideoFrame(
            frame, 
            m_totalFramesScheduled * m_frameDuration, 
            m_frameDuration, 
            m_timeScale);
        m_totalFramesScheduled++;
    }

    m_deckLinkOutput->StartScheduledPlayback(0, m_timeScale, 1.0);
    return true;
}

void DeckLinkDevice::StopOutput()
{
    if (m_isSimulated) {
        if (m_simulationRunning) {
            m_simulationRunning = false;
            if (m_simulationThread.joinable()) {
                m_simulationThread.join();
            }
            std::cout << "[DeckLink] Stopped SIMULATOR Mode" << std::endl;
        }
        return;
    }

    if (m_deckLinkOutput)
    {
        m_deckLinkOutput->StopScheduledPlayback(0, nullptr, 0);
        m_deckLinkOutput->SetScheduledFrameCompletionCallback(nullptr); // Detach callback
        m_deckLinkOutput->DisableVideoOutput();
    }
}

bool DeckLinkDevice::WaitForNextFrame(unsigned int timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    
    // Wait for callback signal with timeout
    if (timeoutMs == 0) {
        // Non-blocking check
        if (!m_frameCompletedSignal) return false;
    } else {
        if (!m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]{ return m_frameCompletedSignal; })) {
            return false;
        }
    }
    
    m_frameCompletedSignal = false; // Reset
    return true;
}

// IUnknown Implementation
HRESULT DeckLinkDevice::QueryInterface(REFIID iid, LPVOID *ppv)
{
    HRESULT result = E_NOINTERFACE;

    if (ppv == nullptr)
        return E_INVALIDARG;

    *ppv = nullptr;

    if (iid == IID_IUnknown)
    {
        *ppv = static_cast<IUnknown*>(static_cast<IDeckLinkVideoOutputCallback*>(this));
        AddRef();
        result = S_OK;
    }
    else if (iid == IID_IDeckLinkVideoOutputCallback)
    {
        *ppv = static_cast<IDeckLinkVideoOutputCallback*>(this);
        AddRef();
        result = S_OK;
    }

    return result;
}

ULONG DeckLinkDevice::AddRef()
{
    return ++m_refCount;
}

ULONG DeckLinkDevice::Release()
{
    ULONG newRefValue = --m_refCount;
    if (newRefValue == 0)
    {
        // Don't delete self if stack allocated, but usually this is heap.
        // For this simple example, we assume Main owns it and keeps a reference, 
        // so we won't `delete this` here to be safe with stack usage in main.
        // OR we just make sure Main calls AddRef once.
    }
    return newRefValue;
}

// Callback
HRESULT DeckLinkDevice::ScheduledFrameCompleted(IDeckLinkVideoFrame *completedFrame, BMDOutputFrameCompletionResult result)
{
    (void)completedFrame;
    
    // Store result
    m_lastCompletionResult = result;

    std::lock_guard<std::mutex> lock(m_mutex);
    
    // 1. Get next frame from queue
    if (m_frameQueue.empty()) return S_OK; // Should not happen with preroll
    
    m_currentFrame = m_frameQueue.front();
    m_frameQueue.pop_front();
    
    // 2. Render to frame (if callback provided)
    if (m_renderCallback)
    {
        IDeckLinkVideoBuffer* videoBuffer = nullptr;
        if (m_currentFrame->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&videoBuffer) == S_OK)
        {
            void* pBuffer = nullptr;
            
            // Start Access -> GetBytes -> CALLBACK -> EndAccess -> Release
            if (videoBuffer->StartAccess(bmdBufferAccessWrite) == S_OK)
            {
                if (videoBuffer->GetBytes(&pBuffer) == S_OK)
                {
                    // === EXECUTE APP RENDERING HERE ===
                    m_renderCallback(pBuffer);
                }
                videoBuffer->EndAccess(bmdBufferAccessWrite);
            }
            videoBuffer->Release();
        }
    }
    
    // 3. Late/Dropped Frame Compensation (Reference Logic)
    if (m_lastCompletionResult == bmdOutputFrameDisplayedLate) {
        m_totalFramesScheduled += 2; // Skip ahead to catch up
        static uint64_t lateCount = 0;
        lateCount++;
        std::ostringstream oss;
        oss << "LATE frame #" << m_totalFramesScheduled << " (cumulative late=" << lateCount << ")";
        DLLog("[WARN]", oss.str());
    } else if (m_lastCompletionResult == bmdOutputFrameDropped) {
        m_totalFramesScheduled += 2; // Skip ahead to catch up
        static uint64_t dropCount = 0;
        dropCount++;
        std::ostringstream oss;
        oss << "DROPPED frame #" << m_totalFramesScheduled << " (cumulative drops=" << dropCount << ")";
        DLLog("[ERROR]", oss.str());
    }

    // 4. Schedule Frame
    HRESULT hr = m_deckLinkOutput->ScheduleVideoFrame(
        m_currentFrame, 
        m_totalFramesScheduled * m_frameDuration, 
        m_frameDuration, 
        m_timeScale
    );
    
    if (SUCCEEDED(hr)) {
        m_totalFramesScheduled++;
        m_frameQueue.push_back(m_currentFrame); // Return to back
    } else {
        std::ostringstream oss;
        oss << "ScheduleVideoFrame FAILED hr=0x" << std::hex << hr
            << " frameNum=" << std::dec << m_totalFramesScheduled
            << " queueSize=" << m_frameQueue.size();
        DLLog("[ERROR]", oss.str());
        m_frameQueue.push_front(m_currentFrame); // Return to front on failure
    }
    
    m_currentFrame = nullptr;
    
    // Signal main loop (just for logging/status now)
    m_frameCompletedSignal = true;
    m_cv.notify_one();
    
    return S_OK;
}

HRESULT DeckLinkDevice::ScheduledPlaybackHasStopped()
{
    return S_OK;
}

bool DeckLinkDevice::GetFrameBuffer(void** pBuffer)
{
    if (!m_deckLinkOutput || !pBuffer || m_frameQueue.empty()) return false;

    // Get next frame from queue (DeckLink SDK pattern)
    m_currentFrame = m_frameQueue.front();
    m_frameQueue.pop_front();
    
    // Get buffer access
    IDeckLinkVideoBuffer* videoBuffer = nullptr;
    if (m_currentFrame->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&videoBuffer) != S_OK)
    {
        // Return frame to queue on failure
        m_frameQueue.push_front(m_currentFrame);
        m_currentFrame = nullptr;
        return false;
    }

    // Start access - KEEP IT LOCKED for writing!
    if (videoBuffer->StartAccess(bmdBufferAccessWrite) != S_OK) {
        videoBuffer->Release();
        m_frameQueue.push_front(m_currentFrame);
        m_currentFrame = nullptr;
        return false;
    }

    // Get bytes
    if (videoBuffer->GetBytes(pBuffer) != S_OK) {
        videoBuffer->EndAccess(bmdBufferAccessWrite);
        videoBuffer->Release();
        m_frameQueue.push_front(m_currentFrame);
        m_currentFrame = nullptr;
        return false;
    }

    // NOTE: Do NOT call EndAccess here! Buffer must stay locked until ScheduleNextFrame()
    // Release the query interface (but buffer stays locked)
    videoBuffer->Release();
    
    return true;
}

bool DeckLinkDevice::ScheduleNextFrame()
{
    if (!m_deckLinkOutput || !m_currentFrame) return false;
    
    // EndAccess BEFORE scheduling (DeckLink SDK pattern)
    IDeckLinkVideoBuffer* videoBuffer = nullptr;
    if (m_currentFrame->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&videoBuffer) == S_OK)
    {
        videoBuffer->EndAccess(bmdBufferAccessWrite);
        videoBuffer->Release();
    }
    
    // Check for late/dropped frames from previous completion (Reference Logic)
    // If the last completed frame was late or dropped, bump the scheduled time further into the future
    if (m_lastCompletionResult == bmdOutputFrameDisplayedLate || m_lastCompletionResult == bmdOutputFrameDropped) {
        m_totalFramesScheduled += 2; // Skip ahead to catch up
    }

    // Schedule the frame
    HRESULT hr = m_deckLinkOutput->ScheduleVideoFrame(
        m_currentFrame, 
        m_totalFramesScheduled * m_frameDuration, 
        m_frameDuration, 
        m_timeScale
    );
    
    if (SUCCEEDED(hr)) {
        m_totalFramesScheduled++;
        // Return frame to back of queue
        m_frameQueue.push_back(m_currentFrame);
        m_currentFrame = nullptr;
        return true;
    }
    
    // On failure, return frame to front of queue
    m_frameQueue.push_front(m_currentFrame);
    m_currentFrame = nullptr;
    return false;
}

void DeckLinkDevice::SimulationLoop() {
    // 59.94 fields per second = 29.97 frames per second ?? 
    // Wait, the hardware is 1080i5994. The callback `ScheduledFrameCompleted` usually fires at field rate or frame rate?
    // Actually DeckLink Output callback fires when a *frame* (which might contain 2 fields) is completed.
    // 1080i59.94 has a frame rate of 29.97 fps.
    // Frame duration: 1001 / 30000 seconds = 33.3666 ms.
    
    // Target interval in seconds
    const double targetInterval = static_cast<double>(m_frameDuration) / static_cast<double>(m_timeScale);
    const std::chrono::duration<double> intervalDuration(targetInterval);

    // Dummy buffer for pipeline
    std::vector<uint8_t> dummyBuffer(1920 * 1080 * 4, 0);

    auto nextWakeTime = std::chrono::steady_clock::now();

    while (m_simulationRunning) {
        auto now = std::chrono::steady_clock::now();
        
        // Render Callback execution
        // This mimics the DeckLink thread calling into our code
        if (m_renderCallback) {
            m_renderCallback(dummyBuffer.data());
        }

        // Notify Main Thread (for logging mostly)
        // Also this mimics the behavior of "waiting for next frame" in main loop
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_frameCompletedSignal = true;
        }
        m_cv.notify_one();

        // Calculate next wake time
        nextWakeTime += std::chrono::duration_cast<std::chrono::nanoseconds>(intervalDuration);
        
        // Sleep until next frame
        // If we are behind, we don't sleep (but we update nextWakeTime to try to catch up, or just yield)
        if (nextWakeTime > now) {
            std::this_thread::sleep_until(nextWakeTime);
        } else {
            // If we are lagging significantly, reset the base time
             if ((now - nextWakeTime) > std::chrono::milliseconds(100)) {
                 nextWakeTime = now + std::chrono::duration_cast<std::chrono::nanoseconds>(intervalDuration);
             }
        }
    }
}
