#pragma once

#include "Types.h"
#include <cstdint>
#include <cstring>
#include <atomic>
#include <vector>
#include <mutex>



//=========================================================================
// CSendBuffer - Zero-copy 친화적 송신 버퍼
//=========================================================================
class CSendBuffer
{
public:
    CSendBuffer();
    explicit CSendBuffer(size_t capacity);
    ~CSendBuffer();

    // 복사 금지
    CSendBuffer(const CSendBuffer&) = delete;
    CSendBuffer& operator=(const CSendBuffer&) = delete;

    // 이동 생성자/대입
    CSendBuffer(CSendBuffer&& other) noexcept;
    CSendBuffer& operator=(CSendBuffer&& other) noexcept;

    bool Write(const void* data, size_t size);
    bool WriteHeader(ProtocolId protocol_id, size_t payload_size);

    uint8_t* Data() { return Data_; }
    const uint8_t* Data() const { return Data_; }
    size_t Size() const { return WritePos; }
    size_t GetCapacity() const { return Capacity; }
    size_t FreeSpace() const { return Capacity - WritePos; }

    void Reset();
    std::atomic<bool> IsAllocated = { false };

private:
    uint8_t* Data_;
    size_t Capacity;
    size_t WritePos;
    bool bOwnsMemory;
};

//=========================================================================
// CRecvBuffer - 수신용 버퍼
//=========================================================================
class CRecvBuffer
{
public:
    explicit CRecvBuffer(size_t capacity);
    ~CRecvBuffer();

    CRecvBuffer(const CRecvBuffer&) = delete;
    CRecvBuffer& operator=(const CRecvBuffer&) = delete;

    uint8_t* GetWritePos() { return Data_ + WritePos; }
    size_t FreeSize() const { return Capacity - WritePos; }
    bool OnWrite(size_t num_bytes);

    const uint8_t* GetReadPos() const { return Data_ + ReadPos; }
    size_t DataSize() const { return WritePos - ReadPos; }
    bool OnRead(size_t num_bytes);

    void Clean();
    void Reset();

private:
    uint8_t* Data_;
    size_t Capacity;
    size_t ReadPos;
    size_t WritePos;
};

//=========================================================================
// FPacketBuffer - 패킷 큐 전송용 고정 크기 버퍼
//=========================================================================
struct FPacketBuffer
{
    uint8_t Data[MAX_PACKET_SIZE];
    uint16_t Size;
    ProtocolId ProtocolID;
    SessionId SessionID;

    FPacketBuffer() : Size(0), ProtocolID(0), SessionID(0) {}
    std::atomic<bool> IsAllocated = { false };

    void CopyFrom(const void* src, size_t src_size, ProtocolId proto_id, SessionId sess_id = 0)
    {
        size_t copy_size = src_size < MAX_PACKET_SIZE ? src_size : MAX_PACKET_SIZE;
        std::memcpy(Data, src, copy_size);
        Size = static_cast<uint16_t>(copy_size);
        ProtocolID = proto_id;
        SessionID = sess_id;
    }

    void Reset()
    {
        Size = 0;
        ProtocolID = 0;
        SessionID = 0;
    }
};

// ============================================================================
// CPacketBufferPool 분기 처리
// ============================================================================
// ------------------------------------------------------------------------
// 동적 확장 패킷 풀 (Growable Packet Pool)
// ------------------------------------------------------------------------

template <typename T>
class LockingStack {
private:
    std::vector<T*> Container;
    // std::mutex _lock; // <--- 무거운 뮤텍스 제거
    std::atomic_flag Flag = ATOMIC_FLAG_INIT; // <--- 가벼운 스핀락 사용

public:
    LockingStack() 
    {
        Container.reserve(500000); // 넉넉하게 예약
    }

    void Push(T* item) 
    {
        // [SpinLock Acquire]
        // 뮤텍스처럼 잠들지 않고, 락이 풀릴 때까지 뺑뺑이(Spin)를 돕니다.
        // 패킷 넣고 빼는 건 순식간(나노초 단위)이라서 이게 훨씬 빠릅니다.
        while (Flag.test_and_set(std::memory_order_acquire)) 
        {
            // CPU 과열 방지용 힌트 (선택 사항)
            // std::this_thread::yield(); 
        }

        Container.push_back(item);

        // [SpinLock Release]
        Flag.clear(std::memory_order_release);
    }
    void PushBatch(const std::vector<T*>& items) 
    {
        if (items.empty()) return;

        // SpinLock Acquire
        while (Flag.test_and_set(std::memory_order_acquire));

        // 벡터의 내용물을 뒤에 갖다 붙임 (복사)
        Container.insert(Container.end(), items.begin(), items.end());

        // SpinLock Release
        Flag.clear(std::memory_order_release);
    }
    // 한 번에 여러 개를 밀어넣는 함수 (고속 충전용)
    size_t PopBatch(std::vector<T*>& out_dst, size_t count) 
    {
        // SpinLock Acquire
        while (Flag.test_and_set(std::memory_order_acquire));

        size_t pop_count = 0;
        size_t available = Container.size();

        if (available > 0) {
            // 달라는 것보다 적으면 있는 만큼만 줌
            size_t actual_count = (available < count) ? available : count;

            // 벡터의 끝에서부터 N개를 잘라서 가져옴 (고속 복사)
            auto start_iter = Container.end() - actual_count;
            out_dst.insert(out_dst.end(), start_iter, Container.end());
            Container.erase(start_iter, Container.end());

            pop_count = actual_count;
        }

        // SpinLock Release
        Flag.clear(std::memory_order_release);
        return pop_count;
    }

    T* Pop() 
    {
        // [SpinLock Acquire]
        while (Flag.test_and_set(std::memory_order_acquire));

        T* item = nullptr;
        if (!Container.empty()) {
            item = Container.back();
            Container.pop_back();
        }

        // [SpinLock Release]
        Flag.clear(std::memory_order_release);

        return item;
    }
};
class CPacketBufferPool
{
    struct Page 
    {
        FPacketBuffer* Buffers; // 동적 배열 포인터
        size_t Count;

        Page(size_t count) : Count(count) {
            Buffers = new FPacketBuffer[count];
        }
        ~Page() {
            delete[] Buffers;
        }
    };

public:
    static CPacketBufferPool& Instance();

    // 초기화/종료 (인터페이스 호환성용, 실제로는 Lazy Init)
    bool Initialize(size_t init_size) { AllocateNewPage(init_size); return true; }
    void Shutdown();

    FPacketBuffer* Acquire();
    void Release(FPacketBuffer* buffer);

    // [추가됨] 기존 로그와의 호환성 및 모니터링을 위한 함수
    size_t GetPoolSize() const { return TotalBufferSize.load(std::memory_order_relaxed); }
    size_t GetFreeCount() const 
    {
        size_t total = GetPoolSize();
        size_t used = UseCount.load(std::memory_order_relaxed);
        return (total > used) ? (total - used) : 0;
    }
    size_t GetMaxUsedCount() const {    return  MaxUseCount.load(std::memory_order_relaxed);   }
    size_t GetTotalPages() const { return Pages.size(); }// 통계용 (대략적)

private:
    CPacketBufferPool();
    ~CPacketBufferPool();

    // 페이지 할당 내부 로직 (cpp 구현)
    void AllocateNewPageInternal(size_t count);
    void AllocateNewPage(size_t count);
    FPacketBuffer* AllocateNewPageAndPop();

private:
    LockingStack<FPacketBuffer> FreeList;
    std::mutex PageLock;
    std::vector<Page*> Pages;

    std::atomic<size_t> TotalBufferSize{ 0 }; // 전체 버퍼 개수 추적
    // [추가됨] 현재 사용 중인 버퍼 개수 추적
    std::atomic<size_t> UseCount;
    std::atomic<size_t> MaxUseCount;

    void UpdateMaxUsage(size_t current_usage);
};



// ========================================================================
// [3] 송신 버퍼 풀
// 기존 Atomic Index 방식을 버리고 Mutex 방식으로 전면 교체
// ========================================================================
class CSendBufferPool
{
public:
    static CSendBufferPool& Instance();

    CSendBuffer* Acquire(size_t min_capacity = SEND_BUFFER_SIZE);
    void Release(CSendBuffer* buffer);

private:
    CSendBufferPool() = default;
    ~CSendBufferPool();

    // 안전한 Mutex 방식
    std::mutex Lock;
    std::vector<CSendBuffer*> Pool;
};

