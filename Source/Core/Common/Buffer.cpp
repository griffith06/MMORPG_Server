#include "Buffer.h"
#include <algorithm>
#include <iostream>


//=========================================================================
// CSendBuffer
//=========================================================================
CSendBuffer::CSendBuffer()
    : Data_(nullptr)
    , Capacity(0)
    , WritePos(0)
    , bOwnsMemory(false)
{
}

CSendBuffer::CSendBuffer(size_t capacity)
    : Data_(new uint8_t[capacity])
    , Capacity(capacity)
    , WritePos(0)
    , bOwnsMemory(true)
{
}

CSendBuffer::~CSendBuffer()
{
    if (bOwnsMemory && Data_)
    {
        delete[] Data_;
        Data_ = nullptr;
    }
}

CSendBuffer::CSendBuffer(CSendBuffer&& other) noexcept
    : Data_(other.Data_)
    , Capacity(other.Capacity)
    , WritePos(other.WritePos)
    , bOwnsMemory(other.bOwnsMemory)
{
    other.Data_ = nullptr;
    other.Capacity = 0;
    other.WritePos = 0;
    other.bOwnsMemory = false;
}

CSendBuffer& CSendBuffer::operator=(CSendBuffer&& other) noexcept
{
    if (this != &other)
    {
        if (bOwnsMemory && Data_)
        {
            delete[] Data_;
        }

        Data_ = other.Data_;
        Capacity = other.Capacity;
        WritePos = other.WritePos;
        bOwnsMemory = other.bOwnsMemory;

        other.Data_ = nullptr;
        other.Capacity = 0;
        other.WritePos = 0;
        other.bOwnsMemory = false;
    }
    return *this;
}

bool CSendBuffer::Write(const void* data, size_t size)
{
    if (FreeSpace() < size)
        return false;

    std::memcpy(Data_ + WritePos, data, size);
    WritePos += size;
    return true;
}

bool CSendBuffer::WriteHeader(ProtocolId protocol_id, size_t payload_size)
{
    if (FreeSpace() < PACKET_HEADER_SIZE)
        return false;

    FPacketHeader header;
    header.Size = static_cast<uint16_t>(PACKET_HEADER_SIZE + payload_size);
    header.ProtocolId = protocol_id;

    std::memcpy(Data_ + WritePos, &header, PACKET_HEADER_SIZE);
    WritePos += PACKET_HEADER_SIZE;
    return true;
}

void CSendBuffer::Reset()
{
    WritePos = 0;
}

//=========================================================================
// CRecvBuffer
//=========================================================================
CRecvBuffer::CRecvBuffer(size_t capacity)
    : Data_(new uint8_t[capacity])
    , Capacity(capacity)
    , ReadPos(0)
    , WritePos(0)
{
}

CRecvBuffer::~CRecvBuffer()
{
    delete[] Data_;
}

bool CRecvBuffer::OnWrite(size_t num_bytes)
{
    if (num_bytes > FreeSize())
        return false;

    WritePos += num_bytes;
    return true;
}

bool CRecvBuffer::OnRead(size_t num_bytes)
{
    if (num_bytes > DataSize())
        return false;

    ReadPos += num_bytes;
    return true;
}

void CRecvBuffer::Clean()
{
    size_t data_size = DataSize();
    if (data_size == 0)
    {
        ReadPos = WritePos = 0;
    }
    else if (ReadPos > 0)
    {
        std::memmove(Data_, Data_ + ReadPos, data_size);
        ReadPos = 0;
        WritePos = data_size;
    }
}

void CRecvBuffer::Reset()
{
    ReadPos = 0;
    WritePos = 0;
}

//=========================================================================
// CPacketBufferPool 구현
//=========================================================================
// ------------------------------------------------------------------------
// [NEW] 동적 확장 풀 구현
// ------------------------------------------------------------------------
CPacketBufferPool::CPacketBufferPool()
    : UseCount(0)
    , MaxUseCount(0)
{
   
}

CPacketBufferPool::~CPacketBufferPool()
{
    Shutdown();
}

CPacketBufferPool& CPacketBufferPool::Instance()
{
    static CPacketBufferPool instance;
    return instance;
}

void CPacketBufferPool::Shutdown()
{
    std::lock_guard<std::mutex> lock(PageLock);
    for (Page* page : Pages)
    {
        delete page;
    }
    Pages.clear();
    TotalBufferSize = 0;
}
// thread_local 사용 쓰레드당 로컬 캐시 구현
static thread_local std::vector<FPacketBuffer*> t_LocalCache;
static const size_t LOCAL_CACHE_CAPACITY = 1000;
static const size_t BATCH_SIZE = 500;

FPacketBuffer* CPacketBufferPool::Acquire()
{
    // 1. 내 주머니(TLS) 확인 (락 없음, 매우 빠름)
    if (!t_LocalCache.empty())
    {
        FPacketBuffer* buf = t_LocalCache.back();
        t_LocalCache.pop_back();

        buf->Reset();
        buf->IsAllocated.store(true, std::memory_order_release);
        // UseCount는 전역이라 여기서 건드리면 캐시라인 핑퐁 발생. 
        // 통계 오차가 좀 생겨도 냅두거나, 별도 TLS 카운터 사용 권장.
        size_t current = UseCount.fetch_add(1, std::memory_order_relaxed) + 1;
        UpdateMaxUsage(current);
        return buf;
    }
    // [Global] 주머니가 비었음 ->TLS 용량 확보
    t_LocalCache.reserve(LOCAL_CACHE_CAPACITY);
    // FreeList에 쌓여있는게 있으면 가져옵니다.
    size_t popped_count = FreeList.PopBatch(t_LocalCache, BATCH_SIZE);
    if (popped_count > 0)
    {
        // 가져온 것 중 하나를 리턴
        FPacketBuffer* buf = t_LocalCache.back();
        t_LocalCache.pop_back();

        buf->Reset();
        buf->IsAllocated.store(true, std::memory_order_release);
        size_t current = UseCount.fetch_add(1, std::memory_order_relaxed) + 1;
        UpdateMaxUsage(current);
        return buf;
    }
    // 3. [Alloc] 전역 풀도 비었음 -> lock사용 새 페이지 할당
    return AllocateNewPageAndPop();
}

void CPacketBufferPool::Release(FPacketBuffer* buffer)
{
    if (!buffer) return;
    // 이미 반납된(false) 녀석이라면 무시해서 Double Free를 막습니다.
    bool expected = true;
    if (!buffer->IsAllocated.compare_exchange_strong(expected, false))
    {
        // 이미 반납된 버퍼입니다. (로그를 찍어보면 여기서 방어했음을 알 수 있음)
        return;
    }
    UseCount.fetch_sub(1, std::memory_order_relaxed);
    // 1. 내 주머니(TLS)에 넣기 (락 없음)
    if (t_LocalCache.size() < LOCAL_CACHE_CAPACITY)
    {
        t_LocalCache.push_back(buffer);
        return;
    }

    // 2. [Global] 주머니 꽉 찼음 -> 절반 정도를 전역 풀로 반납 (락 발생)
    std::vector<FPacketBuffer*> batch_to_return;
    batch_to_return.reserve(BATCH_SIZE);

    // 캐시에서 BATCH_SIZE만큼 꺼내서 옮김
    for (size_t i = 0; i < BATCH_SIZE && !t_LocalCache.empty(); ++i)
    {
        batch_to_return.push_back(t_LocalCache.back());
        t_LocalCache.pop_back();
    }

    // std::vector를 통째로 PushBatch
    FreeList.PushBatch(batch_to_return);

    // 방금 받은 1개는 빈자리가 생긴 내 주머니에 넣음
    t_LocalCache.push_back(buffer);
}
// [신규] 페이지 할당 함수 (사이즈 지정 가능)
void CPacketBufferPool::AllocateNewPageInternal(size_t count)
{
    Page* new_page = new Page(count);
    Pages.push_back(new_page);
    TotalBufferSize.fetch_add(count, std::memory_order_relaxed);

    printf("[Pool] Expanded! Page Size: %zu (Total Buffers: %zu)\n",
        count, TotalBufferSize.load());

    // 생성된 버퍼들을 FreeList에 등록
    std::vector<FPacketBuffer*> batch_buffers;
    batch_buffers.reserve(count);

    for (size_t i = 0; i < count; ++i)
    {
        // 아직 아무도 안 쓰니까 IsAllocated는 false 상태 그대로 둠
        batch_buffers.push_back(&new_page->Buffers[i]);
    }
    FreeList.PushBatch(batch_buffers);
    size_t current = UseCount.fetch_add(1, std::memory_order_relaxed) + 1;
    UpdateMaxUsage(current);
}
void CPacketBufferPool::AllocateNewPage(size_t count)
{
    std::lock_guard<std::mutex> lock(PageLock);
    AllocateNewPageInternal(count);
}

FPacketBuffer* CPacketBufferPool::AllocateNewPageAndPop()
{
    std::lock_guard<std::mutex> lock(PageLock);

    // 락을 얻은 사이에 다른 스레드가 반환했을 수 있으므로 다시 체크 (Double Check)
    FPacketBuffer* buf = FreeList.Pop();
    if (buf)
    {
        buf->Reset();
        buf->IsAllocated.store(true, std::memory_order_release);
        size_t current = UseCount.fetch_add(1, std::memory_order_relaxed) + 1;
        UpdateMaxUsage(current);
    }
    // 2. [확장] 락 잡고 안전하게 확장
    // 주의: 여러 스레드가 동시에 여기로 들어올 수 있으므로, 
    // 락을 잡은 뒤에 "정말 없는지" 한 번 더 체크하는 게 정석이지만,
    // FreeList는 Lock-free라서 락 밖에서도 동작함.
    // 일단 확장 시도
    AllocateNewPageInternal(BUFFER_POOL_EXPAND_SIZE);

    // [재시도] 루프 돌면서 확인하되, 또 할당하지 않음!
    for (int i = 0; i < 5; ++i) // 5번 정도 시도
    {
        buf = FreeList.Pop();
        if (buf)
        {
            buf->Reset();
            buf->IsAllocated.store(true, std::memory_order_release);
            size_t current = UseCount.fetch_add(1, std::memory_order_relaxed) + 1;
            UpdateMaxUsage(current);
            return buf;
        }
        // 실패하면 잠깐 쉼 (Yield) - 다른 스레드가 PushBatch 중일 수 있음
        std::this_thread::yield();
    }
    // 4. 그래도 없으면? -> 정말 심각한 상황. 
    // 여기서 또 할당하지 말고 nullptr 리턴하거나 로그 출력
    printf("CRITICAL: Buffer Pool Exhausted even after expansion!\n");
    return nullptr;
}

void CPacketBufferPool::UpdateMaxUsage(size_t current_usage)
{
    size_t max_val = MaxUseCount.load(std::memory_order_relaxed);
    // 현재 사용량이 기록된 최대값보다 클 때만 CAS 루프로 갱신
    while (current_usage > max_val)
    {
        if (MaxUseCount.compare_exchange_weak(max_val, current_usage, std::memory_order_relaxed))
            break;
        // 실패 시 max_val은 최신 값으로 업데이트됨, 루프 다시 돔
    }
}

//=========================================================================
// CSendBufferPool 구현 (기존과 동일)
//=========================================================================
CSendBufferPool& CSendBufferPool::Instance() { static CSendBufferPool instance; return instance; }

CSendBufferPool::~CSendBufferPool()
{
    std::lock_guard<std::mutex> lock(Lock);
    for (CSendBuffer* buf : Pool) delete buf;
    Pool.clear();
}

CSendBuffer* CSendBufferPool::Acquire(size_t min_capacity)
{
    CSendBuffer* buf = nullptr;
    {
        std::lock_guard<std::mutex> lock(Lock);
        if (!Pool.empty())
        {
            buf = Pool.back();
            Pool.pop_back();
        }
    }

    if (buf)
    {
        if (buf->GetCapacity() >= min_capacity)
        {
            buf->Reset();
            buf->IsAllocated.store(true); // ★ 안전장치 켜기
            return buf;
        }
        delete buf;
    }

    size_t allocSize = (min_capacity < SEND_BUFFER_SIZE) ? SEND_BUFFER_SIZE : min_capacity;
    buf = new CSendBuffer(allocSize);
    buf->IsAllocated.store(true); // ★ 안전장치 켜기
    return buf;
}

void CSendBufferPool::Release(CSendBuffer* buffer)
{
    if (!buffer) return;

    // ★ [핵심] 이중 해제 방지
    bool expected = true;
    if (!buffer->IsAllocated.compare_exchange_strong(expected, false)) return;

    if (buffer->GetCapacity() > SEND_BUFFER_SIZE * 2) { delete buffer; return; }

    std::lock_guard<std::mutex> lock(Lock);
    if (Pool.size() < 1024)
    {
        buffer->Reset();
        Pool.push_back(buffer);
    }
    else
    {
        delete buffer;
    }
}
