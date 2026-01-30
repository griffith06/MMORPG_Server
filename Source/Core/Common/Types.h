#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <algorithm>
//=========================================================================
// 1VM  8core/16GB RAM 기준    최대 10,000명 동접 가능 설계.
// 접속자 수,   상황,            큐 제한(Limit),    메모리 사용량(예상),       상태(16GB 서버)
//  100명,   평상시(최소),            -               약 400 MB,                  매우 쾌적
//           네트워크 장애(최대),   400개,            약 480 MB,                  매우 쾌적
// 1,000명,  평상시(최소),            -               약 410 MB,                  매우 쾌적
//           네트워크 장애(최대),   400개,            약  1.2GB,                  매우 쾌적
// 10,000명, 평상시(최소),            -               약 500 MB,                  매우 쾌적
//           네트워크 장애(최대),   400개,            약  8.5GB,                  매우 쾌적
//=========================================================================
// 서버 방어: 네트웍 단절또는 많은 동접시 send버퍼가 밀리면 유저들 쳐내기 시작함. 테스트시 메모리 2G선에서 싹 정리.
// 네트웍 하드웨어 대역폭 1Gbps(약 125MB / s)
// 현재 네트웍 모듈은 200,000PPS 가 안정적인 수준 초당 20만개(최대 25만개) -> 실제 VM에서는 더 낮게 해야할 듯.
// PPS 최적화를 위한 방안.   MMORPG에서는 recv보다 send가 압도적으로 많음.  3번방법이 제일 효과 좋음 무빙패킷만 적용해도 됨.
// 1. 변화만 있을때 보내는 방법.
// 2. 거리 기반 갱신 주기 차등화(LOD - Level Of Detail) 거리에 따라 띄엄띄엄 보내기.
// 3. 패킷 뭉치기 (Packet Aggregation) 내가 움직일때 주변 30명에게 따로 보내지말고 내 주변 30명이 움직임을 한번에 send하기


// 260114 스트레스 테스트 
// MAX_SIM_PLAYERS = 10,000명 MAX_BROADCAST_TARGETS = 30, config.SendIntervalMs = 1000;   //1초에 한번씩 30명 브로드캐스트
// 전체 메모리 4.5G, 풀 페이지 179개 (약 179만 버퍼), 10000 접속시 , 1686명을 쳐내고 8,674 안정적으로 유지.
// 한계는 "25만~26만 PPS"



//=========================================================================
// ID Types
//=========================================================================
using SessionId = uint64_t;
using AccountId = uint64_t;
using MapId = uint32_t;
using ProtocolId = uint16_t;
using ThreadId = uint16_t;
//=========================================================================
// FPacketHeader - 패킷 헤더 구조체
//=========================================================================
#pragma pack(push, 1)
struct FPacketHeader
{
    uint16_t Size;
    ProtocolId ProtocolId;
};
#pragma pack(pop)


    //=========================================================================
    // Thread & Session Limits
    //=========================================================================
    constexpr size_t MAX_SIM_PLAYERS = 10000;     //최대 동접인원
    constexpr size_t MAX_LOCAL_THREADS = 4;     //쓰레드 개수. config로 뺄것         
    constexpr size_t MAX_IO_THREADS = 8;        //io 쓰레드 개수.accept send,recv 담당
    constexpr size_t MAX_SESSIONS_PER_THREAD = std::max((size_t)2000, (MAX_SIM_PLAYERS * 20 / 10) / MAX_LOCAL_THREADS); // [변경] 좀비 세션(Waiting)을 고려하여 여유분(Buffer)을 50% 추가
    constexpr size_t MAX_TOTAL_SESSIONS = MAX_LOCAL_THREADS * MAX_SESSIONS_PER_THREAD;

    //=========================================================================
	// Buffer Constants     8core/16GB RAM 기준
	// OS 2G, 게임 컨텐츠, ~4G, 네트워크 버퍼  ~2G, 여유 8G
    // 메모리 사용량: 최악의 경우(1만명 단절) 약 7.6GB (변동 없음).
    // 유저당 전송 대기열(유저 10,000 VM간 단절시 memory) 최대 크기 (2KB * 400개 * 10,000유저 = 약 7.6GB) 
    // 초당 패킷 40개면 10초 동안 버팀.
    //=========================================================================
    constexpr size_t RECV_BUFFER_SIZE = 8192;   // 8KB (수신용)
    constexpr size_t SEND_BUFFER_SIZE = 65536;  //패킷을 직렬화(Serialize)할 때 임시로 쓰는 용도 
    constexpr size_t MAX_PACKET_SIZE = 2048;    // 2KB (개발 편의성 + 메모리 절약 밸런스형)
    constexpr size_t PACKET_HEADER_SIZE = 4;
    constexpr size_t SEND_BATCH_SIZE = 32;      //Lock - Free 큐에서 한 번에 꺼내오는 최대 개수, async_write_some을 연속으로 호출
    constexpr size_t SEND_QUEUE_CAPACITY = 8192;    //FPacketBuffer* 

    constexpr size_t MAX_SEND_QUEUE_SIZE_LOD0 = 4000;
    constexpr size_t MAX_SEND_QUEUE_SIZE_LOD1 = 2000;
    constexpr size_t MAX_SEND_QUEUE_SIZE_LOD2 = 600;
//    constexpr size_t PACKET_BUFFER_POOL_SIZE = std::max((size_t)10000, MAX_SIM_PLAYERS * 100);
    constexpr size_t BUFFER_POOL_INIT_SIZE = std::max((size_t)10000, MAX_SIM_PLAYERS * 30);;
    constexpr size_t BUFFER_POOL_EXPAND_SIZE = std::max((size_t)10000, MAX_SIM_PLAYERS * 10);;
    // [추가된 부분 1] 브로드캐스트 최대 타겟 수 제한
    constexpr size_t MAX_BROADCAST_TARGETS = 30;

    //=========================================================================
    // Reconnection
    //=========================================================================
    constexpr uint32_t RECONNECT_TIMEOUT_SEC = 10;// 30;// 300;

    //=========================================================================
    // Session States
    //=========================================================================
    enum class ESessionState : uint8_t
    {
        None = 0,               // [0] 초기 상태 / 공석
        Connecting,             // 소켓 연결(Accept)은 되었으나, 아직 '로그인 패킷'을 통해 인증을 받지 못한 상태입니다  이 상태에서 일정 시간 내에 로그인을 안 하면 해킹 의심
        Active,                 // 로그인 인증 완료! 게임을 플레이중 패킷을 주고받을 수 있는 상태
        TempDisconnect,         // 임시 연결 끊김(리커넥트 대기) 소켓은 끊어졌지만, 메모리상에서 플레이어 정보(위치, 아이템 등)를 RECONNECT_TIMEOUT_SEC(10초) 동안 보존하고 있는 상태 패킷을 보내면 안 됩
        Disconnecting,          // 종료 진행 중(선택적)비동기 DB 저장
        Closed                  // 이 상태가 되면 곧 재활용되거나 메모리가 초기화
    };

    static_assert(sizeof(FPacketHeader) == PACKET_HEADER_SIZE, "FPacketHeader size mismatch");

    //=========================================================================
    // FReconnectToken - 재접속 토큰
    //=========================================================================
    struct FReconnectToken
    {
        uint64_t Token;
        uint64_t Timestamp;
    };

    //=========================================================================
    // Utility Functions
    //=========================================================================
    inline SessionId GenerateSessionId()
    {
        static std::atomic<uint64_t> Counter{1};
        return Counter.fetch_add(1, std::memory_order_relaxed);
    }

    inline uint64_t GenerateReconnectToken()
    {
        static std::atomic<uint64_t> Counter{1};
        return Counter.fetch_add(1, std::memory_order_relaxed);
    }

