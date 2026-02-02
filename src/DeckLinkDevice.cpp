#include "DeckLinkDevice.h"
#include <comdef.h>
#include <iostream>
#include "DeckLinkAPI_i.c" // Helper for GUIDs

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
      m_videoFrame(nullptr),
      m_frameCompletedSignal(false),
      m_totalFramesScheduled(0),
      m_timeScale(30000),     // 30000 / 1001 ~= 29.97 fps (59.94i)
      m_frameDuration(1001)
{
}

DeckLinkDevice::~DeckLinkDevice()
{
    StopOutput();
    SafeRelease(&m_videoFrame);
    SafeRelease(&m_deckLinkOutput);
    SafeRelease(&m_deckLink);
}

bool DeckLinkDevice::Initialize()
{
    IDeckLinkIterator* deckLinkIterator = nullptr;
    HRESULT result = CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator, (void**)&deckLinkIterator);

    if (FAILED(result))
    {
        std::cerr << "DeckLink drivers may not be installed." << std::endl;
        return false;
    }

    // Get the first device
    if (deckLinkIterator->Next(&m_deckLink) != S_OK)
    {
        std::cerr << "No DeckLink device found." << std::endl;
        deckLinkIterator->Release();
        return false;
    }

    deckLinkIterator->Release();

    // Get Output Interface
    if (m_deckLink->QueryInterface(IID_IDeckLinkOutput, (void**)&m_deckLinkOutput) != S_OK)
    {
        return false;
    }

    // Check video mode (59.94i / 1080i59.94)
    // Mode: bmdModeHD1080i5994
    BMDDisplayMode displayMode = bmdModeHD1080i5994;
    
    result = m_deckLinkOutput->EnableVideoOutput(displayMode, bmdVideoOutputFlagDefault);
    if (FAILED(result))
    {
        std::cerr << "Could not enable video output for 1080i59.94" << std::endl;
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
    if (!m_deckLinkOutput) return false;

    // Create a reusable frame - ARGB for External Key (Dual Port)
    // 1920x1080, 4 Bytes per pixel (ARGB = 32bpp) -> RowBytes = 1920 * 4
    // UltraStudio HD Mini will output:
    //   SDI Out: Fill (RGB)
    //   SDI Out 2: Key (Alpha)
    if (!m_videoFrame)
    {
        HRESULT hr = m_deckLinkOutput->CreateVideoFrame(
            1920, 1080,
            1920 * 4,  // ARGB = 4 bytes per pixel
            bmdFormat8BitARGB, 
            bmdFrameFlagDefault, 
            &m_videoFrame);
            
        if (FAILED(hr)) return false;

        // Fill with opaque black
        void* pBuffer = nullptr;
        IDeckLinkVideoBuffer* videoBuffer = nullptr;
        if (m_videoFrame->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&videoBuffer) == S_OK)
        {
            if (videoBuffer->GetBytes(&pBuffer) == S_OK)
            {
                uint32_t* p32 = (uint32_t*)pBuffer;
                long pixels = 1920 * 1080;
                for(long i=0; i<pixels; ++i) {
                    *p32++ = 0xFF000000; // A=255, R=0, G=0, B=0 (opaque black)
                }
            }
            videoBuffer->Release();
        }
    }

    // Preroll 10 frames to prevent initial underflow
    for (int i = 0; i < 10; i++)
    {
         m_deckLinkOutput->ScheduleVideoFrame(m_videoFrame, m_totalFramesScheduled * m_frameDuration, m_frameDuration, m_timeScale);
         m_totalFramesScheduled++;
    }

    m_deckLinkOutput->StartScheduledPlayback(0, m_timeScale, 1.0);
    return true;
}

void DeckLinkDevice::StopOutput()
{
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
    if (m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]{ return m_frameCompletedSignal; }))
    {
        m_frameCompletedSignal = false; // Reset
        return true;
    }
    return false; // Timeout
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
    // Signal main loop that a frame completed
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_frameCompletedSignal = true;
    }
    m_cv.notify_one();
    
    return S_OK;
}

HRESULT DeckLinkDevice::ScheduledPlaybackHasStopped()
{
    return S_OK;
}

bool DeckLinkDevice::GetFrameBuffer(void** pBuffer, void** pKeyBuffer)
{
    if (!m_videoFrame) return false;

    IDeckLinkVideoBuffer* videoBuffer = nullptr;
    if (m_videoFrame->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&videoBuffer) == S_OK)
    {
        videoBuffer->GetBytes(pBuffer);
        videoBuffer->Release();
        return true;
    }
    return false;
}

bool DeckLinkDevice::ScheduleNextFrame()
{
    if (m_deckLinkOutput && m_videoFrame)
    {
         HRESULT hr = m_deckLinkOutput->ScheduleVideoFrame(m_videoFrame, m_totalFramesScheduled * m_frameDuration, m_frameDuration, m_timeScale);
         if (SUCCEEDED(hr))
         {
             m_totalFramesScheduled++;
             return true;
         }
    }
    return false;
}

