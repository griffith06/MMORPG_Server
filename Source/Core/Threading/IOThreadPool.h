#pragma once

#include "../Network/NetworkFwd.h"
#include "../Common/Types.h"

#ifdef _WIN32
#define _WIN32_WINNT 0x0601
//#define ASIO_STANDALONE
//#define ASIO_HAS_STD_ATOMIC
#endif

#include <asio.hpp>
#include <thread>
#include <atomic>


//=========================================================================
// CIOThreadPool - IO 스레드 풀 (Asio io_context 실행)
//=========================================================================
class CIOThreadPool
{
public:
    CIOThreadPool();
    ~CIOThreadPool();

    CIOThreadPool(const CIOThreadPool&) = delete;
    CIOThreadPool& operator=(const CIOThreadPool&) = delete;

    bool Start(size_t thread_count = 2);
    void Stop();
    bool IsRunning() const { return bIsRunning.load(std::memory_order_acquire); }

    asio::io_context& GetIOContext() { return IoContext; }
    size_t GetThreadCount() const { return ThreadCount; }

private:
    static void IOThreadFunc(CIOThreadPool* self, size_t thread_index);

private:
    asio::io_context IoContext;
    asio::executor_work_guard<asio::io_context::executor_type> WorkGuard;

    std::thread Threads[MAX_IO_THREADS];
    size_t ThreadCount;

    std::atomic<bool> bIsRunning;
};
