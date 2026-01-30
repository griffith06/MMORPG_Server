#include "DummyClient.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include "../../Core/Common/Protocol.h" // <--- 공통 헤더 참조


// 정적 변수 정의
uint64_t CDummyClient::GlobalMassDropWakeupTime = 0;
bool CDummyClient::bEnableRandomRolling = false;
std::atomic<size_t> CDummyClient::s_ActiveClientCount(0);
std::atomic<bool>   CDummyClient::s_bAllowBroadcast(false);

static uint64_t GetCurrentTimeMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

//=========================================================================
// CDummyClient
//=========================================================================
CDummyClient::CDummyClient(asio::io_context& io_context, uint32_t client_id)
    : IoContext(io_context)
    , Socket(io_context)
    , SendTimer(io_context)
    , LatencyTimer(io_context)
    , ClientId(client_id)
    , ConnectionState(EConnectionState::Disconnected)
    , BotState(EBotState::None)
    , RecvBuffer(RECV_BUFFER_SIZE)
    , SendSize(0)
    , LastSendTime(0)
    , NextSendTime(0)
    , PacketsSent(0)
    , PacketsReceived(0)
    , BytesSent(0)
    , BytesReceived(0)
    , ConnectAttempts(0)
    , DisconnectCount(0)
    , Rng(client_id)
    , PosDistribution(0.0f, 1000.0f)
    , LatencyDistribution(10, 30)
    , TryReconnectCount(0)
    , SuccessReconnectCount(0)
    , NewLoginCount(0)
{
    // ClientId는 0부터 시작하므로, Usn은 1부터 시작하게 설정
    MyUsn = static_cast<uint64_t>(client_id + 1);
}

CDummyClient::~CDummyClient()
{
    Disconnect();
}

void CDummyClient::Connect(const std::string& host, uint16_t port)
{
    if (ConnectionState.load(std::memory_order_acquire) != EConnectionState::Disconnected)
        return;

    // Config 업데이트 (재접속을 위해 저장)
    Config.Host = host;
    Config.Port = port;

    ConnectionState.store(EConnectionState::Connecting, std::memory_order_release);
    ConnectAttempts.fetch_add(1, std::memory_order_relaxed);

    asio::ip::tcp::resolver resolver(IoContext);
    asio::error_code ec;
    auto results = resolver.resolve(host, std::to_string(port), ec);

    if (ec || results.empty())
    {
        ConnectionState.store(EConnectionState::Disconnected, std::memory_order_release);
        return;
    }

    Endpoint = *results.begin();

    Socket.async_connect(Endpoint,
        [this](const asio::error_code& ec)
        {
            OnConnect(ec);
        }
    );
}

bool CDummyClient::ConnectToServer()
{
    // 이미 연결 중이거나 연결된 상태면 스킵
    if (ConnectionState.load(std::memory_order_acquire) != EConnectionState::Disconnected)
        return false;

    Connect(Config.Host, Config.Port);
    // [추가] 연결 시작 시간 기록!
    ConnectStartTime = GetCurrentTimeMs();
    return true;
}

void CDummyClient::Disconnect()
{
    EConnectionState expected = EConnectionState::Connected;
    // 이미 끊겼거나 끊는 중이면 패스
    if (ConnectionState.load(std::memory_order_acquire) == EConnectionState::Disconnected)
        return;

    // CAS로 상태 변경 시도 (Connected -> Closing)
    if (!ConnectionState.compare_exchange_strong(expected, EConnectionState::Closing,
        std::memory_order_acq_rel, std::memory_order_relaxed))
    {
        // Connecting 상태일 수도 있으니 강제 종료 처리
        ConnectionState.store(EConnectionState::Disconnected, std::memory_order_release);
    }
    //접속자 수 감소
    s_ActiveClientCount.fetch_sub(1, std::memory_order_relaxed);

    asio::error_code ec;
    Socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    Socket.close(ec);

    SendTimer.cancel();
    LatencyTimer.cancel();

    ConnectionState.store(EConnectionState::Disconnected, std::memory_order_release);
    DisconnectCount.fetch_add(1, std::memory_order_relaxed);
}

void CDummyClient::Update(uint64_t current_time_ms)
{
    uint64_t now = GetCurrentTimeMs();

    // --------------------------------------------------------
    // [Bot State Machine]
    // --------------------------------------------------------
    if (BotState == EBotState::Login)
    {
        // Login 상태가 되었는데(연결 성공 후), 3초가 지나도 Playing이 안 됐다면?
        // -> 로그인 패킷이 씹혔거나 응답이 안 오는 상황
        // ConnectStartTime_은 OnConnect 성공 시 갱신해주는 게 좋지만, 
        // 여기선 간단히 "연결된 지 오래됐는데 아직도 Login 상태면"으로 판단

        // 연결된 후 경과 시간 계산 (현재 시간 - 연결 시작 시간)
        if (now - ConnectStartTime > 3000)
        {
            // printf("[Client %u] Login Timeout! Reset to MassDrop.\n", ClientId);

            Disconnect(); // 연결 끊기

            // [핵심] 다시 테스트 모드(MassDropping)로 강제 복귀!
            // 이걸 해줘야 다음 프레임 Update에서 MassDropping 로직을 탑니다.
            BotState = EBotState::MassDropping;
            GlobalMassDropWakeupTime = now + 100; // 0.1초 뒤 재시도
        }
        return;
    }
    // 1. [Playing] 상태일 때 랜덤 롤링 체크 ('r' 키 모드)
    if (BotState == EBotState::Playing && bEnableRandomRolling)
    {
        // [중요] 아직 로그인 정보(토큰)를 못 받았다면 끊지 말고 기다린다!
        if (!bHasLoginInfo)
            return;

        // 10초에 1번 꼴, 0.1% 확률 (프레임마다 호출되므로 확률 낮게 설정)
        // (rand() % 1000) < 1 은 너무 자주 끊길 수 있으니 조절 필요
        if ((rand() % 5000) < 1)
        {
            Disconnect();
            BotState = EBotState::RollingDisconnect;
            DisconnectTime = now;
            WaitDuration = (rand() % (RECONNECT_TIMEOUT_SEC * 2)) * 1000;  // 50% 확률로 리커넥트
            // printf("Bot[%u] Rolling Disconnect. Wait %llu ms\n", ClientId, WaitDuration);
            return;
        }
    }

    // 2. [RollingDisconnect] : 랜덤 대기 후 복귀
    if (BotState == EBotState::RollingDisconnect)
    {
        if (now - DisconnectTime >= WaitDuration)
        {
            // [수정] 상태를 확실하게 확인
            auto connState = ConnectionState.load(std::memory_order_acquire);

            // 만약 아직 Closing 중이라면 조금 더 대기
            if (connState == EConnectionState::Closing)
            {
                // 100ms 뒤 다시 체크
                DisconnectTime = now - WaitDuration + 100;
                return;
            }
            // 재접속 시도
            if (ConnectToServer())
            {
                // 주의: 비동기 Connect이므로 여기서 바로 SendLoginPacket을 못함.
                // OnConnect에서 처리하거나, State를 LoginWait 등으로 바꿔야 함.
                // 여기서는 단순화를 위해 BotState를 Connecting으로 두고 OnConnect에서 처리
                BotState = EBotState::Connecting;
            }
            else
            {
                // [중요] 실패 시 즉시 포기하지 말고 1초 뒤 재시도하게 시간 갱신
                                    // 기존: DisconnectTime = now; (너무 오래 대기할 수 있음)
                                    // 변경: 1초 뒤 재시도
                WaitDuration += 1000;
            }
        }
        return;
    }

    // 3. [MassDropping] : 약속된 시간에 일제히 기상 ('m' 키 모드)
    if (BotState == EBotState::MassDropping)
    {
        // [추가] ★ 좀비 방지 코드 ★
        // 연결 중(Connecting) 상태인데, 2초(2000ms)가 지났다면?
        if (ConnectionState.load(std::memory_order_acquire) == EConnectionState::Connecting)
        {
            if (now - ConnectStartTime > 2000)
            {
                // "야, 2초 지났다. 그냥 끊어!"
                // Disconnect를 호출하면 소켓이 닫히면서 async_connect가 '취소'됨
                Disconnect();

                // Disconnect() -> 소켓 닫힘 -> OnConnect(에러) 호출됨 -> 재시도 로직 발동!
            }
            return; // 이번 프레임은 여기서 끝
        }
        // 아직 기상 시간 전이면 대기
        if (now < GlobalMassDropWakeupTime)
            return;

        // 접속 시도
        if (ConnectToServer())
        {
            // 성공하면 연결 중 상태로 전환 (OnConnect에서 로그인 패킷 보냄)
            BotState = EBotState::Connecting;
        }
        else
        {
            // [중요] 접속 실패 시(OS 포트 부족 등), 포기하지 말고 0.1초 뒤 다시 시도
            // 이렇게 해야 낙오되는 애들이 없어짐
            GlobalMassDropWakeupTime = now + 100;
        }
        return;
    }

    // 4. [ZombieDead] : 아무것도 안 함 ('t' 키 모드)
    if (BotState == EBotState::ZombieDead)
    {
        return;
    }

    // --------------------------------------------------------
    // [Normal Update]
    // --------------------------------------------------------

	//봇 접속 완료되면 이동 패킷 전송 허용
    if (!s_bAllowBroadcast.load(std::memory_order_acquire))
        return;

    // 연결된 상태가 아니면 패킷 전송 로직 수행 안 함
    if (ConnectionState.load(std::memory_order_acquire) != EConnectionState::Connected)
        return;

    // Playing 상태가 아니면(로그인 전 등) 이동 패킷 안 보냄
    if (BotState != EBotState::Playing)
        return;

    if (current_time_ms >= NextSendTime)
    {
        SendMovePacket();
        NextSendTime = current_time_ms + Config.SendIntervalMs;
    }
}

void CDummyClient::SetTestMode(EBotState mode)
{
    // 외부(Manager)에서 강제 호출
    if (mode == EBotState::MassDropping)
    {
        Disconnect();
        BotState = EBotState::MassDropping;
        // 5초 뒤 다 같이 재접속하도록 설정
        GlobalMassDropWakeupTime = GetCurrentTimeMs() + 3000;
    }
    else if (mode == EBotState::ZombieDead)
    {
        Disconnect();
        BotState = EBotState::ZombieDead;
    }
}

CDummyClient::FStats CDummyClient::GetStats() const
{
    FStats stats;
    stats.PacketsSent = PacketsSent.load(std::memory_order_relaxed);
    stats.PacketsReceived = PacketsReceived.load(std::memory_order_relaxed);
    stats.BytesSent = BytesSent.load(std::memory_order_relaxed);
    stats.BytesReceived = BytesReceived.load(std::memory_order_relaxed);
    stats.ConnectAttempts = ConnectAttempts.load(std::memory_order_relaxed);
    stats.DisconnectCount = DisconnectCount.load(std::memory_order_relaxed);
    stats.TryReconnectCount = TryReconnectCount.load(std::memory_order_relaxed);
    stats.SuccessReconnectCount = SuccessReconnectCount.load(std::memory_order_relaxed);
    stats.NewLoginCount = NewLoginCount.load(std::memory_order_relaxed);
    return stats;
}

void CDummyClient::ResetStats()
{
    PacketsSent.store(0, std::memory_order_relaxed);
    PacketsReceived.store(0, std::memory_order_relaxed);
    BytesSent.store(0, std::memory_order_relaxed);
    BytesReceived.store(0, std::memory_order_relaxed);
    ConnectAttempts.store(0, std::memory_order_relaxed);
    DisconnectCount.store(0, std::memory_order_relaxed);
    TryReconnectCount.store(0, std::memory_order_relaxed);
    SuccessReconnectCount.store(0, std::memory_order_relaxed);
    NewLoginCount.store(0, std::memory_order_relaxed);
}

void CDummyClient::OnConnect(const asio::error_code& ec)
{
    if (ec)
    {
        ConnectionState.store(EConnectionState::Disconnected, std::memory_order_release);

        // [핵심] 실패했다고 끝내지 말고, 테스트 모드라면 재시도 로직 가동!
        if (BotState == EBotState::MassDropping)
        {
            // 실패했으니 0.1초 뒤에 다시 시도하도록 시간 설정
            GlobalMassDropWakeupTime = GetCurrentTimeMs() + 100;
            // 상태는 MassDropping 유지 (그래야 Update에서 다시 Connect 시도함)
        }
        else if (BotState == EBotState::RollingDisconnect)
        {
            // 롤링 모드도 마찬가지로 1초 뒤 재시도
            WaitDuration += 1000;
        }
        else
        {
            // 일반적인 경우라면 Connecting 상태를 풀어줘야 함 (안 그러면 좀비 됨)
            printf("#####Critical!!! [Client %u] Became ZOMBIE! (State was %d)\n", ClientId, BotState);
            BotState = EBotState::None;
        }
        return;
    }

    //접속자 수 증가
    s_ActiveClientCount.fetch_add(1, std::memory_order_relaxed);

    asio::error_code opt_ec;
    Socket.set_option(asio::ip::tcp::no_delay(true), opt_ec);

    ConnectionState.store(EConnectionState::Connected, std::memory_order_release);
    ConnectStartTime = GetCurrentTimeMs(); //로그인 시작 시간.
    LastSendTime = GetCurrentTimeMs();
    //모든 봇이 동시에 패킷을 쏘지 않도록, 초기 전송 시간을 랜덤하게 분산
    uint32_t random_delay = Rng() % Config.SendIntervalMs;
    NextSendTime = LastSendTime + Config.SendIntervalMs + random_delay;

    // [수정된 로직] 
    // 재접속 여부 판단: 
    // 1. Connecting 상태 (Update에서 바꾼 경우)
    // 2. RollingDisconnect 상태 (Update에서 바꾸기 전에 콜백이 온 경우)
    // 3. MassDropping 상태 (상동)
    bool is_reconnect = (BotState == EBotState::Connecting || 
                            BotState == EBotState::RollingDisconnect || 
                            BotState == EBotState::MassDropping);

    // [복구!] 여기서 로그인 패킷을 보내야 합니다!
    SendLoginPacket(is_reconnect);

    // [수정] 바로 Playing으로 가지 않고, 응답 대기 상태로!
    BotState = EBotState::Login;
//        BotState = EBotState::Playing; // 바로 플레이 상태로 전환

    DoRead();
}

void CDummyClient::DoRead()
{
    if (ConnectionState.load(std::memory_order_acquire) != EConnectionState::Connected)
        return;

    if (RecvBuffer.FreeSize() < MAX_PACKET_SIZE)
    {
        RecvBuffer.Clean();
    }

    Socket.async_read_some(
        asio::buffer(RecvBuffer.GetWritePos(), RecvBuffer.FreeSize()),
        [this](const asio::error_code& ec, size_t bytes_transferred)
        {
            OnRead(ec, bytes_transferred);
        }
    );
}

void CDummyClient::OnRead(const asio::error_code& ec, size_t bytes_transferred)
{
    if (ec)
    {
        Disconnect();
        return;
    }

    RecvBuffer.OnWrite(bytes_transferred);
    BytesReceived.fetch_add(bytes_transferred, std::memory_order_relaxed);

    while (RecvBuffer.DataSize() >= PACKET_HEADER_SIZE)
    {
        FPacketHeader header;
        std::memcpy(&header, RecvBuffer.GetReadPos(), PACKET_HEADER_SIZE);

        if (header.Size > MAX_PACKET_SIZE || header.Size < PACKET_HEADER_SIZE)
        {
            Disconnect();
            return;
        }

        if (RecvBuffer.DataSize() < header.Size)
            break;

        // [추가] 여기서 패킷 ID 확인!
        // 프로토콜 ID PKT_LOGIN_RES이 로그인 응답이라고 가정 (서버랑 맞춰야 함!)
        if (header.ProtocolId == PKT_LOGIN_RES)
        {
            // [디버그] 클라가 패킷을 물리적으로 받았는지, 사이즈는 몇인지 확인
            //다량로그
            //printf("[Client] RECV LoginRes. PktSize:%u vs StructSize:%zu\n", header.Size, sizeof(FLoginResponsePacket));
            OnLoginResponse(RecvBuffer.GetReadPos(), header.Size);
        }

        PacketsReceived.fetch_add(1, std::memory_order_relaxed);
        RecvBuffer.OnRead(header.Size);
    }

    DoRead();
}
// 2. [추가] 로그인 응답 핸들러 구현
void CDummyClient::OnLoginResponse(const uint8_t* data, size_t size)
{
    if (size < sizeof(FLoginResponsePacket))
    {
        printf("[Client] ERROR: Packet too small! (%zu < %zu)\n", size, sizeof(FLoginResponsePacket));
        return;
    }

    //다량로그
    //printf("[Client] Login Info Saved! Token: %llu\n", ReconnectToken);
    FLoginResponsePacket packet;
    std::memcpy(&packet, data, sizeof(FLoginResponsePacket));
    if (packet.Success)
    {
        // 1. 내가 리커넥트를 시도했던 상황이고 (Wait 후 접속)
        if (bJustRestored)
        {
            // 2. 서버가 준 토큰이 내 옛날 토큰과 같다면 -> 복구 성공!
            // (만약 서버가 세션을 새로 만들었다면 토큰이 갱신되었을 것임)
            if (ReconnectToken != 0 && ReconnectToken == packet.Token)
            {
                SuccessReconnectCount.fetch_add(1, std::memory_order_relaxed);
                printf("[Client %u] Reconnect SUCCESS (Restored)\n", ClientId);
            }
            else
            {
                // 토큰이 달라짐 -> 서버가 기존 세션 못 찾아서 새로 만듦 (만료됨)
                NewLoginCount.fetch_add(1, std::memory_order_relaxed);
                printf("[Client %u] Reconnect FAILED -> New Login (Expired)\n", ClientId);
            }
        }
        // 중요: 서버가 준 정보를 저장한다!
        ServerSessionId = packet.SessionId;
        ReconnectToken = packet.Token;
        bHasLoginInfo = true;
        BotState = EBotState::Playing; //게임 시작
    }
    else
        printf("[Client %u] Login Fail! SessID=%llu Token=%llu\n", ClientId, ServerSessionId, ReconnectToken);
}
void CDummyClient::DoWrite()
{
    if (ConnectionState.load(std::memory_order_acquire) != EConnectionState::Connected)
        return;

    if (SendSize == 0)
        return;

    if (Config.bEnableArtificialLatency)
    {
        uint32_t latency = LatencyDistribution(Rng);
        LatencyTimer.expires_after(std::chrono::milliseconds(latency));
        LatencyTimer.async_wait(
            [this](const asio::error_code& ec)
            {
                if (!ec && ConnectionState.load(std::memory_order_acquire) == EConnectionState::Connected)
                {
                    Socket.async_write_some(
                        asio::buffer(SendBuffer, SendSize),
                        [this](const asio::error_code& write_ec, size_t bytes_transferred)
                        {
                            OnWrite(write_ec, bytes_transferred);
                        }
                    );
                }
            }
        );
    }
    else
    {
        Socket.async_write_some(
            asio::buffer(SendBuffer, SendSize),
            [this](const asio::error_code& ec, size_t bytes_transferred)
            {
                OnWrite(ec, bytes_transferred);
            }
        );
    }
}

void CDummyClient::OnWrite(const asio::error_code& ec, size_t bytes_transferred)
{
    if (ec)
    {
        Disconnect();
        return;
    }

    BytesSent.fetch_add(bytes_transferred, std::memory_order_relaxed);
    PacketsSent.fetch_add(1, std::memory_order_relaxed);
    SendSize = 0;
}

void CDummyClient::SendLoginPacket(bool is_reconnect)
{
    if (SendSize > 0) return; // 이전 전송 대기 중이면 스킵 (간단 처리)

    // 재접속 시도라면 플래그 켜기
    if (is_reconnect)
    {
        bJustRestored = true;
        // "나 리커넥트 시도한다" 카운트 증가
        TryReconnectCount.fetch_add(1, std::memory_order_relaxed);
    }

    // 리커넥트 의도(is_reconnect)가 있고 + 저장된 정보(bHasLoginInfo)가 있을 때만 토큰 전송
    // 그 외(최초 접속, 정보 없음)에는 0 전송 -> 서버가 New Login으로 처리함
    uint64_t token_to_send = 0;
    FLoginPacket packet;
    if (is_reconnect && bHasLoginInfo)
    {
        // 재접속: 저장해둔 토큰을 보냄
        token_to_send = ReconnectToken;
    }
    // [핵심] 내 USN을 담아서 보낸다.
    // 토큰은 있으면 보내고, 없으면 0 (서버가 USN으로 찾을 거니까 토큰 의존도 낮춤)
    packet.Initialize(MyUsn, token_to_send, is_reconnect);


    std::memcpy(SendBuffer, &packet, sizeof(FLoginPacket));
    SendSize = sizeof(FLoginPacket);

    DoWrite();
}

void CDummyClient::SendMovePacket()
{
    if (SendSize > 0) return;

    // [추가] 재접속 후 첫 이동이라면 로그 출력
    if (bJustRestored)
    {
        printf("[Client %u] Reconnected & Resumed Moving!\n", ClientId);
        bJustRestored = false; // 한 번만 출력하고 끄기
    }

    float x, y;
    GenerateRandomMove(x, y);

    FMovePacket packet;
    packet.Initialize(
        ClientId,
        x,
        y,
        PosDistribution(Rng) * 0.1f - 50.0f,
        PosDistribution(Rng) * 0.1f - 50.0f,
        static_cast<uint32_t>(GetCurrentTimeMs())
    );

    std::memcpy(SendBuffer, &packet, sizeof(FMovePacket));
    SendSize = sizeof(FMovePacket);
    LastSendTime = GetCurrentTimeMs();

    DoWrite();
}

void CDummyClient::GenerateRandomMove(float& x, float& y)
{
    x = PosDistribution(Rng);
    y = PosDistribution(Rng);
}
 