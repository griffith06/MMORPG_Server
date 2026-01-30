#include "../Network/Socket.h"
#include "ThreadManager.h"


CThreadManager::CThreadManager()
    : bIsRunning(false)
    , LocalThreadCount(0)
    , NextThreadIndex(0)
{
}

CThreadManager::~CThreadManager()
{
    Shutdown();
}

CThreadManager& CThreadManager::Instance()
{
    static CThreadManager instance;
    return instance;
}

bool CThreadManager::Initialize(size_t io_thread_count, size_t local_thread_count)
{
    if (bIsRunning.load(std::memory_order_acquire))
        return true;

    if (io_thread_count == 0 || io_thread_count > MAX_IO_THREADS)
        io_thread_count = 2;

    if (local_thread_count == 0 || local_thread_count > MAX_LOCAL_THREADS)
        local_thread_count = MAX_LOCAL_THREADS;

    // IO 스레드 시작
    if (!IoThreadPool.Start(io_thread_count))
    {
        return false;
    }

    // Local 스레드 시작
    LocalThreadCount = local_thread_count;
    for (size_t i = 0; i < LocalThreadCount; ++i)
    {
        if (!LocalThreads[i].Start(static_cast<ThreadId>(i)))
        {
            // Rollback
            for (size_t j = 0; j < i; ++j)
            {
                LocalThreads[j].Stop();
            }
            IoThreadPool.Stop();
            return false;
        }
    }

    bIsRunning.store(true, std::memory_order_release);
    return true;
}

void CThreadManager::Shutdown()
{
    if (!bIsRunning.exchange(false, std::memory_order_acq_rel))
        return;

    // Local 스레드 먼저 정지
    for (size_t i = 0; i < LocalThreadCount; ++i)
    {
        LocalThreads[i].Stop();
    }

    // IO 스레드 정지
    IoThreadPool.Stop();

    LocalThreadCount = 0;
}

CLocalThread* CThreadManager::GetLocalThread(ThreadId id)
{
    if (id >= LocalThreadCount)
        return nullptr;

    return &LocalThreads[id];
}

CLocalThread* CThreadManager::GetLeastLoadedLocalThread()
{
    if (LocalThreadCount == 0)
        return nullptr;

    CLocalThread* best = &LocalThreads[0];
    size_t min_sessions = best->GetSessionCount();

    for (size_t i = 1; i < LocalThreadCount; ++i)
    {
        size_t count = LocalThreads[i].GetSessionCount();
        if (count < min_sessions)
        {
            min_sessions = count;
            best = &LocalThreads[i];
        }
    }

    return best;
}

ThreadId CThreadManager::GetNextThreadId()
{
    size_t index = NextThreadIndex.fetch_add(1, std::memory_order_relaxed);
    return static_cast<ThreadId>(index % LocalThreadCount);
}

bool CThreadManager::RouteNewConnection(CSocket* socket)
{
    if (!socket)
        return false;

    // 가장 부하가 적은 스레드에 할당
    CLocalThread* thread = GetLeastLoadedLocalThread();
    if (!thread)
    {
        return false;
    }

    // 스레드의 세션 수 확인
    if (thread->GetSessionCount() >= thread->GetMaxSessions())
    {
        return false;
    }

    return thread->PostCreateSession(socket);
}

bool CThreadManager::BroadcastToAllThreads(const CLocalThread::FCommand& command)
{
    bool all_success = true;
    for (size_t i = 0; i < LocalThreadCount; ++i)
    {
        if (!LocalThreads[i].PostCommand(command))
        {
            all_success = false;
        }
    }
    return all_success;
}
