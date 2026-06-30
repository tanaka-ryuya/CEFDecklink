#pragma once

#include <Windows.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <thread>
#include <vector>

// Forward declarations to avoid including heavy DeckLink headers here if possible
// But we usually need them for inheritance.
// Generated headers from IDL
// DeckLinkAPI.idl includes all other IDLs, so we only need this one.
#include "DeckLinkAPI.h" 

class DeckLinkDevice : public IDeckLinkVideoOutputCallback
{
public:
    DeckLinkDevice();
    virtual ~DeckLinkDevice();

    bool Initialize(const std::string& format = "5994i");
    bool StartOutput();
    void StopOutput();

    // Wait for the next VBlank / Frame Completion
    // Returns false if timeout or error
    bool WaitForNextFrame(unsigned int timeoutMs = 100);
    
    // Simulator Mode
    bool IsSimulated() const { return m_isSimulated; }

    // IUnknown
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv);
    virtual ULONG STDMETHODCALLTYPE AddRef();
    virtual ULONG STDMETHODCALLTYPE Release();

    // IDeckLinkVideoOutputCallback
    virtual HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(
        IDeckLinkVideoFrame *completedFrame,
        BMDOutputFrameCompletionResult result);

    virtual HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped();

    // Access to the video buffer for writing
    // pBuffer: receives the pointer to the pixel data
    bool GetFrameBuffer(void** pBuffer);

    // Schedule the frame after writing to it
    bool ScheduleNextFrame();

    // Callback-driven architecture (Reference Pattern)
    // Application provides a function to render into the buffer
    using RenderCallback = std::function<void(void* pBuffer)>;
    void SetRenderCallback(RenderCallback callback);

private:
    std::atomic<ULONG> m_refCount;
    
    IDeckLink* m_deckLink;
    IDeckLinkOutput* m_deckLinkOutput;
    
    // Sync
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_frameCompletedSignal;
    BMDOutputFrameCompletionResult m_lastCompletionResult; // Store result for main loop

    // Output State - Frame Queue Pattern (DeckLink SDK)
    std::deque<IDeckLinkMutableVideoFrame*> m_frameQueue;
    IDeckLinkMutableVideoFrame* m_currentFrame;  // Frame currently being written
    long long m_totalFramesScheduled;
    BMDTimeScale m_timeScale;
    BMDTimeValue m_frameDuration;
    
    RenderCallback m_renderCallback;

    // Simulation Mode
    bool m_isSimulated;
    std::thread m_simulationThread;
    std::atomic<bool> m_simulationRunning;
    void SimulationLoop();
};
