#include "IOThreadPool.h"

#ifdef _WIN32
#include <Windows.h>
#endif


CIOThreadPool::CIOThreadPool()
    : IoContext()
    , WorkGuard(asio::make_work_guard(IoContext))
    , ThreadCount(0)
    , bIsRunning(false)
{
}

CIOThreadPool::~CIOThreadPool()
{
    Stop();
}

bool CIOThreadPool::Start(size_t thread_count)
{
    if (bIsRunning.load(std::memory_order_acquire))
        return true;

    if (thread_count == 0 || thread_count > MAX_IO_THREADS)
        thread_count = 2;

    ThreadCount = thread_count;
    bIsRunning.store(true, std::memory_order_release);

    for (size_t i = 0; i < ThreadCount; ++i)
    {
        Threads[i] = std::thread(IOThreadFunc, this, i);
    }

    return true;
}

void CIOThreadPool::Stop()
{
    if (!bIsRunning.exchange(false, std::memory_order_acq_rel))
        return;

    WorkGuard.reset();
    IoContext.stop();

    for (size_t i = 0; i < ThreadCount; ++i)
    {
        if (Threads[i].joinable())
        {
            Threads[i].join();
        }
    }

    ThreadCount = 0;
}

void CIOThreadPool::IOThreadFunc(CIOThreadPool* self, size_t thread_index)
{
    (void)thread_index;

    #ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    #endif

    while (self->bIsRunning.load(std::memory_order_acquire))
    {
        try
        {
            self->IoContext.run();
            break;
        }
        catch (...)
        {
            // Log and continue
        }
    }
}
