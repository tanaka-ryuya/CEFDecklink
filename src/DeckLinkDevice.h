#pragma once

#include <Windows.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>

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

    bool Initialize();
    bool StartOutput();
    void StopOutput();

    // Wait for the next VBlank / Frame Completion
    // Returns false if timeout or error
    bool WaitForNextFrame(unsigned int timeoutMs = 100);

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

private:
    std::atomic<ULONG> m_refCount;
    
    IDeckLink* m_deckLink;
    IDeckLinkOutput* m_deckLinkOutput;
    
    // Sync
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_frameCompletedSignal;

    // Output State
    IDeckLinkMutableVideoFrame* m_videoFrame; // Reusable frame for sync
    IDeckLinkVideoBuffer* m_currentBuffer;    // Current buffer for EndAccess
    long long m_totalFramesScheduled;
    BMDTimeScale m_timeScale;
    BMDTimeValue m_frameDuration;
};
