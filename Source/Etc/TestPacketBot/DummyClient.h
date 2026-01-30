#pragma once

#include "../../Core/Common/Types.h"
#include "../../Core/Common/Buffer.h"

#ifdef _WIN32
#define _WIN32_WINNT 0x0601
//#define ASIO_STANDALONE
//#define ASIO_HAS_STD_ATOMIC
#endif

#include <asio.hpp>
#include <atomic>
#include <random>

// [Bot State Definition]
enum class EBotState {
    None,
    Connecting,
    Login,
    Playing,            // 정상 플레이

    // [Test Modes]
    RollingDisconnect,  // 랜덤 롤링: 끊고 대기 중
    MassDropping,       // 매스 드랍: 다 같이 끊고 대기 중
    ZombieDead          // 타임아웃 클린업: 영구 사망 (재접속 안 함)
};


//=========================================================================
// CDummyClient - 부하 테스트용 더미 클라이언트
//=========================================================================
class CDummyClient
{
public:
    // 소켓 연결 상태 (내부 관리용)
    enum class EConnectionState : uint8_t
    {
        Disconnected,
        Connecting,
        Connected,
        Closing
    };

    struct FStats
    {
        uint64_t PacketsSent;
        uint64_t PacketsReceived;
        uint64_t BytesSent;
        uint64_t BytesReceived;
        uint64_t ConnectAttempts;
        uint64_t DisconnectCount;
        uint64_t TryReconnectCount;     // 리커넥트 시도 횟수
        uint64_t SuccessReconnectCount; // 리커넥트 성공(세션 복구) 횟수
        uint64_t NewLoginCount;         // 리커넥트 실패 후 신규 로그인 횟수
    };

    struct FConfig
    {
        uint32_t SendIntervalMs;
        uint32_t MinLatencyMs;
        uint32_t MaxLatencyMs;
        bool bEnableArtificialLatency;

        // 접속 정보 (재접속용)
        std::string Host;
        uint16_t Port;

        FConfig()
            : SendIntervalMs(300)
            , MinLatencyMs(10)
            , MaxLatencyMs(30)
            , bEnableArtificialLatency(false)
            , Host("127.0.0.1")
            , Port(7777)
        {
        }
    };

    // 전역 테스트 설정 (모든 봇 공유)
    static bool bEnableRandomRolling;
    static std::atomic<size_t> s_ActiveClientCount; // 현재 접속 성공한 봇 수
    static std::atomic<bool>   s_bAllowBroadcast;   // 이동 패킷 전송 허용 여부
public:
    CDummyClient(asio::io_context& io_context, uint32_t client_id);
    ~CDummyClient();

    CDummyClient(const CDummyClient&) = delete;
    CDummyClient& operator=(const CDummyClient&) = delete;

    // [Test Mode Control]
    void SetTestMode(EBotState mode);

    // 연결 관련
    void Connect(const std::string& host, uint16_t port);
    void Disconnect();
    bool ConnectToServer(); // 내부 재접속용 래퍼

    bool IsConnected() const { return ConnectionState.load(std::memory_order_acquire) == EConnectionState::Connected; }

    void SetConfig(const FConfig& config) { Config = config; }
    const FConfig& GetConfig() const { return Config; }

    void Update(uint64_t current_time_ms);

    FStats GetStats() const;
    void ResetStats();

    uint32_t GetClientId() const { return ClientId; }
    EConnectionState GetConnectionState() const { return ConnectionState.load(std::memory_order_acquire); }

private:
    // [추가] 로그인 응답 처리 함수
    void OnLoginResponse(const uint8_t* data, size_t size);
    void OnConnect(const asio::error_code& ec);
    void DoRead();
    void OnRead(const asio::error_code& ec, size_t bytes_transferred);
    void DoWrite();
    void OnWrite(const asio::error_code& ec, size_t bytes_transferred);

    // 패킷 전송 헬퍼
    void SendLoginPacket(bool is_reconnect);
    void SendMovePacket();
    void GenerateRandomMove(float& x, float& y);

private:
    // [Bot Logical State]
    EBotState BotState = EBotState::None;
    // [추가] 서버가 발급해준 정보 저장용
    uint64_t ServerSessionId = 0; // 서버가 준 진짜 ID
    uint64_t ReconnectToken = 0;  // 재접속용 토큰
    bool bHasLoginInfo = false;   // 로그인 정보를 받았는지 여부
    // [추가] 재접속 직후인지 확인하는 플래그 (로그 출력용)
    bool bJustRestored = false;
    uint64_t ConnectStartTime = 0; // 연결 시도 시각
    uint64_t MyUsn = 0;

    // [Connection Physical State]
    std::atomic<EConnectionState> ConnectionState;

    // 재접속용 데이터
    uint64_t DisconnectTime = 0;
    uint64_t WaitDuration = 0;

    // 매스 드랍용 공통 시간
    static uint64_t GlobalMassDropWakeupTime;

    // Asio & Network
    asio::io_context& IoContext;
    asio::ip::tcp::socket Socket;
    asio::steady_timer SendTimer;
    asio::steady_timer LatencyTimer;
    asio::ip::tcp::endpoint Endpoint;

    uint32_t ClientId;
    FConfig Config;

    CRecvBuffer RecvBuffer;
    uint8_t SendBuffer[MAX_PACKET_SIZE];
    size_t SendSize;

    uint64_t LastSendTime;
    uint64_t NextSendTime;

    // 통계
    std::atomic<uint64_t> PacketsSent;
    std::atomic<uint64_t> PacketsReceived;
    std::atomic<uint64_t> BytesSent;
    std::atomic<uint64_t> BytesReceived;
    std::atomic<uint64_t> ConnectAttempts;
    std::atomic<uint64_t> DisconnectCount;
    std::atomic<uint64_t> TryReconnectCount;
    std::atomic<uint64_t> SuccessReconnectCount;
    std::atomic<uint64_t> NewLoginCount;

    // 랜덤
    std::mt19937 Rng;
    std::uniform_real_distribution<float> PosDistribution;
    std::uniform_int_distribution<uint32_t> LatencyDistribution;
};
