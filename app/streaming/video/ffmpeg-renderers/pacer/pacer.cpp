#include "pacer.h"
#include "streaming/streamutils.h"

#include "nullthreadedvsyncsource.h"

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <VersionHelpers.h>
#include "dxvsyncsource.h"
#endif

// Limit the number of queued frames to prevent excessive memory consumption
// if the V-Sync source or renderer is blocked for a while.
#define MAX_QUEUED_FRAMES 8

// We may be woken up slightly late so don't go all the way
// up to the next V-sync since we may accidentally step into
// the next V-sync period. It also takes some amount of time
// to do the render itself, so we can't render right before
// V-sync happens.
#define TIMER_SLACK_MS 3

Pacer::Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats) :
    m_RenderThread(nullptr),
    m_Stopping(false),
    m_VsyncSource(nullptr),
    m_VsyncRenderer(renderer),
    m_MaxVideoFps(0),
    m_DisplayFps(0),
    m_VideoStats(videoStats)
{

}

Pacer::~Pacer()
{
    // Stop V-sync callbacks
    delete m_VsyncSource;
    m_VsyncSource = nullptr;

    // Stop the render thread
    m_Stopping = true;
    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeAll();
        SDL_WaitThread(m_RenderThread, nullptr);
    }
    else {
        // Notify the renderer that it is being destroyed soon
        // NB: This must happen on the same thread that calls renderFrame().
        m_VsyncRenderer->cleanupRenderContext();
    }

    // Delete any remaining unconsumed frames
    while (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        av_frame_free(&frame);
    }
    while (!m_PacingQueue.isEmpty()) {
        AVFrame* frame = m_PacingQueue.dequeue();
        av_frame_free(&frame);
    }
}

void Pacer::renderOnMainThread()
{
    // Ignore this call for renderers that work on a dedicated render thread
    if (m_RenderThread != nullptr) {
        return;
    }

    m_FrameQueueLock.lock();

    if (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        m_FrameQueueLock.unlock();

        renderFrame(frame);
    }
    else {
        m_FrameQueueLock.unlock();
    }
}

int Pacer::renderThread(void* context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to set render thread to high priority: %s",
                    SDL_GetError());
    }

    while (!me->m_Stopping) {
        // Wait for the renderer to be ready for the next frame
        me->m_VsyncRenderer->waitToRender();

        // Acquire the frame queue lock to protect the queue and
        // the not empty condition
        me->m_FrameQueueLock.lock();

        // Wait for a frame to be ready to render
        while (!me->m_Stopping && me->m_RenderQueue.isEmpty()) {
            me->m_RenderQueueNotEmpty.wait(&me->m_FrameQueueLock);
        }

        if (me->m_Stopping) {
            // Exit this thread
            me->m_FrameQueueLock.unlock();
            break;
        }

        AVFrame* frame = me->m_RenderQueue.dequeue();
        me->m_FrameQueueLock.unlock();

        me->renderFrame(frame);
    }

    // Notify the renderer that it is being destroyed soon
    // NB: This must happen on the same thread that calls renderFrame().
    me->m_VsyncRenderer->cleanupRenderContext();

    return 0;
}

void Pacer::enqueueFrameForRenderingAndUnlock(AVFrame *frame)
{
    dropFrameForEnqueue(m_RenderQueue);
    m_RenderQueue.enqueue(frame);

    m_FrameQueueLock.unlock();

    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeOne();
    }
    else {
        SDL_Event event;

        // For main thread rendering, we'll push an event to trigger a callback
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_FRAME_READY;
        SDL_PushEvent(&event);
    }
}

// Called in an arbitrary thread by the IVsyncSource on V-sync
// or an event synchronized with V-sync
void Pacer::vsyncCallback(int timeUntilNextVsyncMillis)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    SDL_assert(timeUntilNextVsyncMillis >= TIMER_SLACK_MS);

    m_FrameQueueLock.lock();

    // If the queue length history entries are large, be strict
    // about dropping excess frames.
    int frameDropTarget = 1;

    // If we may get more frames per second than we can display, use
    // frame history to drop frames only if consistently above the
    // one queued frame mark.
    if (m_MaxVideoFps >= m_DisplayFps) {
        for (int queueHistoryEntry : m_PacingQueueHistory) {
            if (queueHistoryEntry <= 1) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 3;
                break;
            }
        }

        // Keep a rolling 500 ms window of pacing queue history
        if (m_PacingQueueHistory.count() == m_DisplayFps / 2) {
            m_PacingQueueHistory.dequeue();
        }

        m_PacingQueueHistory.enqueue(m_PacingQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_PacingQueue.count() > frameDropTarget) {
        AVFrame* frame = m_PacingQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        m_VideoStats->pacerDroppedFrames++;
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    if (m_PacingQueue.isEmpty()) {
        // Wait for a frame to arrive or our V-sync timeout to expire
        if (!m_PacingQueueNotEmpty.wait(&m_FrameQueueLock, timeUntilNextVsyncMillis - TIMER_SLACK_MS)) {
            // Wait timed out - unlock and bail
            m_FrameQueueLock.unlock();
            return;
        }
    }

    // Place the first frame on the render queue
    enqueueFrameForRenderingAndUnlock(m_PacingQueue.dequeue());
}

bool Pacer::initialize(SDL_Window* window, int maxVideoFps, bool enablePacing)
{
    m_MaxVideoFps = maxVideoFps;
    m_DisplayFps = StreamUtils::getDisplayRefreshRate(window);
    m_RendererAttributes = m_VsyncRenderer->getRendererAttributes();

    if (enablePacing) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing active: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);

    #if defined(Q_OS_WIN32)
        // Don't use D3DKMTWaitForVerticalBlankEvent() on Windows 7, because
        // it blocks during other concurrent DX operations (like actually rendering).
        if (IsWindows8OrGreater()) {
            m_VsyncSource = new DxVsyncSource(this);
        }
    #else
        // Platforms without a VsyncSource will just render frames
        // immediately like they used to.
    #endif

        SDL_assert(m_VsyncSource != nullptr || !(m_RendererAttributes & RENDERER_ATTRIBUTE_FORCE_PACING));

        if (m_VsyncSource != nullptr && !m_VsyncSource->initialize(window, m_DisplayFps)) {
            return false;
        }
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing disabled: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);
    }

    if (m_VsyncRenderer->isRenderThreadSupported()) {
        m_RenderThread = SDL_CreateThread(Pacer::renderThread, "PacerRender", this);
    }

    return true;
}

void Pacer::renderFrame(AVFrame* frame)
{
    // Count time spent in Pacer's queues
    Uint32 beforeRender = SDL_GetTicks();
    m_VideoStats->totalPacerTime += beforeRender - frame->pkt_dts;

    // Render it
    m_VsyncRenderer->renderFrame(frame);
    Uint32 afterRender = SDL_GetTicks();

    m_VideoStats->totalRenderTime += afterRender - beforeRender;
    m_VideoStats->renderedFrames++;
    av_frame_free(&frame);

    // Drop frames if we have too many queued up for a while
    m_FrameQueueLock.lock();

    int frameDropTarget;

    if (m_RendererAttributes & RENDERER_ATTRIBUTE_NO_BUFFERING) {
        // Renderers that don't buffer any frames but don't support waitToRender() need us to buffer
        // an extra frame to ensure they don't starve while waiting to present.
        frameDropTarget = 1;
    }
    else {
        frameDropTarget = 0;
        for (int queueHistoryEntry : m_RenderQueueHistory) {
            if (queueHistoryEntry == 0) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 2;
                break;
            }
        }

        // Keep a rolling 500 ms window of render queue history
        if (m_RenderQueueHistory.count() == m_MaxVideoFps / 2) {
            m_RenderQueueHistory.dequeue();
        }

        m_RenderQueueHistory.enqueue(m_RenderQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_RenderQueue.count() > frameDropTarget) {
        AVFrame* frame = m_RenderQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        m_VideoStats->pacerDroppedFrames++;
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    m_FrameQueueLock.unlock();
}

void Pacer::dropFrameForEnqueue(QQueue<AVFrame*>& queue)
{
    SDL_assert(queue.size() <= MAX_QUEUED_FRAMES);
    if (queue.size() == MAX_QUEUED_FRAMES) {
        AVFrame* frame = queue.dequeue();
        av_frame_free(&frame);
    }
}

void Pacer::submitFrame(AVFrame* frame)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    // Queue the frame and possibly wake up the render thread
    m_FrameQueueLock.lock();
    if (m_VsyncSource != nullptr) {
        dropFrameForEnqueue(m_PacingQueue);
        m_PacingQueue.enqueue(frame);
        m_FrameQueueLock.unlock();
        m_PacingQueueNotEmpty.wakeOne();
    }
    else {
        enqueueFrameForRenderingAndUnlock(frame);
    }
}
