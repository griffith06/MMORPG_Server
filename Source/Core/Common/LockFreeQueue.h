#pragma once

#include <atomic>
#include <cstddef>


constexpr size_t CACHE_LINE_SIZE = 64;

//=========================================================================
// SPSCQueue - Single Producer Single Consumer Lock-Free Queue
// Session의 IncomingPacketQueue에 사용 (포인터 저장용 최적화)
//=========================================================================
template<typename T, size_t Capacity>
class SPSCQueue
{
public:
    SPSCQueue();
    ~SPSCQueue() = default;

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // Producer (single thread)
    bool Push(T item);

    // Consumer (single thread)
    bool Pop(T& out_item);

    bool IsEmpty() const;
    size_t Size() const;
    size_t GetCapacity() const { return Capacity; }

private:
    struct alignas(CACHE_LINE_SIZE) FAlignedIndex
    {
        std::atomic<size_t> Value{0};
        char Padding[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>)];
    };

    FAlignedIndex Head_;
    FAlignedIndex Tail_;
    alignas(CACHE_LINE_SIZE) T Buffer_[Capacity];
};

//=========================================================================
// MPSCQueue - Multiple Producer Single Consumer Lock-Free Queue
// Socket의 SendQueue에 사용
//=========================================================================
template<typename T, size_t Capacity>
class MPSCQueue
{
public:
    MPSCQueue();
    ~MPSCQueue() = default;

    MPSCQueue(const MPSCQueue&) = delete;
    MPSCQueue& operator=(const MPSCQueue&) = delete;

    // Producer (multiple threads)
    bool Push(T item);
    bool PushAndCheckWasEmpty(T item);

    // Consumer (single thread)
    bool Pop(T& out_item);
    size_t PopBatch(T* out_items, size_t max_count);

    bool IsEmpty() const;
    size_t ApproximateSize() const;
    size_t GetCapacity() const { return Capacity; }

private:
    struct alignas(CACHE_LINE_SIZE) FAlignedIndex
    {
        std::atomic<size_t> Value{0};
        char Padding[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>)];
    };

    FAlignedIndex Head_;
    FAlignedIndex Tail_;
    alignas(CACHE_LINE_SIZE) std::atomic<bool> Slots_[Capacity];
    alignas(CACHE_LINE_SIZE) T Buffer_[Capacity];
};

//=========================================================================
// SPSCQueue Implementation
//=========================================================================
template<typename T, size_t Capacity>
SPSCQueue<T, Capacity>::SPSCQueue()
{
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    for (size_t i = 0; i < Capacity; ++i)
    {
        Buffer_[i] = T{};
    }
}

template<typename T, size_t Capacity>
bool SPSCQueue<T, Capacity>::Push(T item)
{
    const size_t tail = Tail_.Value.load(std::memory_order_relaxed);
    const size_t head = Head_.Value.load(std::memory_order_acquire);

    if (tail - head >= Capacity)
        return false;

    Buffer_[tail & (Capacity - 1)] = item;
    Tail_.Value.store(tail + 1, std::memory_order_release);
    return true;
}

template<typename T, size_t Capacity>
bool SPSCQueue<T, Capacity>::Pop(T& out_item)
{
    const size_t head = Head_.Value.load(std::memory_order_relaxed);
    const size_t tail = Tail_.Value.load(std::memory_order_acquire);

    if (head >= tail)
        return false;

    out_item = Buffer_[head & (Capacity - 1)];
    Head_.Value.store(head + 1, std::memory_order_release);
    return true;
}

template<typename T, size_t Capacity>
bool SPSCQueue<T, Capacity>::IsEmpty() const
{
    return Head_.Value.load(std::memory_order_acquire) >=
            Tail_.Value.load(std::memory_order_acquire);
}

template<typename T, size_t Capacity>
size_t SPSCQueue<T, Capacity>::Size() const
{
    const size_t head = Head_.Value.load(std::memory_order_acquire);
    const size_t tail = Tail_.Value.load(std::memory_order_acquire);
    return tail >= head ? tail - head : 0;
}

//=========================================================================
// MPSCQueue Implementation
//=========================================================================
template<typename T, size_t Capacity>
MPSCQueue<T, Capacity>::MPSCQueue()
{
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    for (size_t i = 0; i < Capacity; ++i)
    {
        Slots_[i].store(false, std::memory_order_relaxed);
        Buffer_[i] = T{};
    }
}

template<typename T, size_t Capacity>
bool MPSCQueue<T, Capacity>::Push(T item)
{
    size_t tail = Tail_.Value.fetch_add(1, std::memory_order_acq_rel);
    size_t index = tail & (Capacity - 1);

    size_t head = Head_.Value.load(std::memory_order_acquire);
    if (tail - head >= Capacity)
    {
        Tail_.Value.fetch_sub(1, std::memory_order_release);
        return false;
    }

    Buffer_[index] = item;
    Slots_[index].store(true, std::memory_order_release);
    return true;
}

template<typename T, size_t Capacity>
bool MPSCQueue<T, Capacity>::PushAndCheckWasEmpty(T item)
{
    size_t tail = Tail_.Value.fetch_add(1, std::memory_order_acq_rel);
    size_t index = tail & (Capacity - 1);

    size_t head = Head_.Value.load(std::memory_order_acquire);
    if (tail - head >= Capacity)
    {
        Tail_.Value.fetch_sub(1, std::memory_order_release);
        return false;
    }

    bool was_empty = (tail == head);
    Buffer_[index] = item;
    Slots_[index].store(true, std::memory_order_release);
    return was_empty;
}

template<typename T, size_t Capacity>
bool MPSCQueue<T, Capacity>::Pop(T& out_item)
{
    size_t head = Head_.Value.load(std::memory_order_relaxed);
    size_t index = head & (Capacity - 1);

    if (!Slots_[index].load(std::memory_order_acquire))
        return false;

    out_item = Buffer_[index];
    Slots_[index].store(false, std::memory_order_release);
    Head_.Value.store(head + 1, std::memory_order_release);
    return true;
}

template<typename T, size_t Capacity>
size_t MPSCQueue<T, Capacity>::PopBatch(T* out_items, size_t max_count)
{
    size_t count = 0;
    while (count < max_count)
    {
        size_t head = Head_.Value.load(std::memory_order_relaxed);
        size_t index = head & (Capacity - 1);

        if (!Slots_[index].load(std::memory_order_acquire))
            break;

        out_items[count] = Buffer_[index];
        Slots_[index].store(false, std::memory_order_release);
        Head_.Value.store(head + 1, std::memory_order_release);
        ++count;
    }
    return count;
}

template<typename T, size_t Capacity>
bool MPSCQueue<T, Capacity>::IsEmpty() const
{
    size_t head = Head_.Value.load(std::memory_order_acquire);
    size_t index = head & (Capacity - 1);
    return !Slots_[index].load(std::memory_order_acquire);
}

template<typename T, size_t Capacity>
size_t MPSCQueue<T, Capacity>::ApproximateSize() const
{
    size_t head = Head_.Value.load(std::memory_order_acquire);
    size_t tail = Tail_.Value.load(std::memory_order_acquire);
    return tail >= head ? tail - head : 0;
}
