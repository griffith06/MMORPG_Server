#pragma once

#include "../Network/NetworkFwd.h"
#include "../Common/Types.h"
#include "IOThreadPool.h"
#include "LocalThread.h"

#include <atomic>


//=========================================================================
// CThreadManager - 스레드 관리 중앙 클래스
//=========================================================================
class CThreadManager
{
public:
    static CThreadManager& Instance();

    bool Initialize(size_t io_thread_count = 2, size_t local_thread_count = MAX_LOCAL_THREADS);
    void Shutdown();
    bool IsRunning() const { return bIsRunning.load(std::memory_order_acquire); }

    CIOThreadPool& GetIOThreadPool() { return IoThreadPool; }
    asio::io_context& GetIOContext() { return IoThreadPool.GetIOContext(); }

    size_t GetLocalThreadCount() const { return LocalThreadCount; }
    CLocalThread* GetLocalThread(ThreadId id);
    CLocalThread* GetLeastLoadedLocalThread();
    ThreadId GetNextThreadId();

    // 새 연결 라우팅
    bool RouteNewConnection(CSocket* socket);

    // 모든 스레드에 브로드캐스트
    bool BroadcastToAllThreads(const CLocalThread::FCommand& command);

private:
    CThreadManager();
    ~CThreadManager();

    CThreadManager(const CThreadManager&) = delete;
    CThreadManager& operator=(const CThreadManager&) = delete;

private:
    std::atomic<bool> bIsRunning;

    CIOThreadPool IoThreadPool;

    CLocalThread LocalThreads[MAX_LOCAL_THREADS];
    size_t LocalThreadCount;
    std::atomic<size_t> NextThreadIndex;
};
