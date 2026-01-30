#pragma once

#include "Types.h" // FPacketHeader가 여기에 있다면 include

#pragma pack(push, 1)

// -----------------------------------------------------------
// [Protocol ID Definition]
// -----------------------------------------------------------
enum : uint16_t
{
    PKT_MOVE = 1,
    PKT_LOGIN_REQ = 100,
    PKT_LOGIN_RES = 101,
};

//로그인 실패.
enum LoginFailReason : uint8_t
{
    FAIL_NONE = 0,
    FAIL_WRONG_PASSWORD,
    FAIL_ALREADY_CONNECTED, // "이미 접속 중입니다. 잠시 후 다시 시도하세요."
};

enum KickReason : uint8_t
{
    KICK_DUPLICATE_LOGIN,   // "다른 기기에서 접속하여 종료됩니다." 중복접속
    KICK_GM_BAN,
};

// 로그인 실패 알림 (서버 -> 새 클라)
struct FLoginFailPacket
{
    FPacketHeader Header;
    uint8_t Reason;
};

// 강제 퇴장 알림 (서버 -> 헌 클라)
struct FKickPacket
{
    FPacketHeader Header;
    uint8_t Reason;
};
// -----------------------------------------------------------
// [Packet Structures]
// -----------------------------------------------------------

// 1. 이동 패킷
struct FMovePacket
{
    FPacketHeader Header;
    uint32_t ClientId;
    float PosX;
    float PosY;
    float VelocityX;
    float VelocityY;
    uint32_t Timestamp;

    void Initialize(uint32_t id, float x, float y, float vx, float vy, uint32_t ts)
    {
        Header.Size = sizeof(FMovePacket);
        Header.ProtocolId = PKT_MOVE;
        ClientId = id;
        PosX = x;
        PosY = y;
        VelocityX = vx;
        VelocityY = vy;
        Timestamp = ts;
    }
};

// 2. 로그인 요청 패킷
struct FLoginPacket
{
    FPacketHeader Header;
    uint64_t Usn;         // USN (User Serial Number)
    uint64_t Token;       // 재접속 토큰
    bool IsReconnect;     // (옵션)

    void Initialize(uint64_t usn, uint64_t token, bool is_reconnect)
    {
        Header.Size = sizeof(FLoginPacket);
        Header.ProtocolId = PKT_LOGIN_REQ;
        Usn = usn;
        Token = token;
        IsReconnect = is_reconnect;
    }
};

// 3. 로그인 응답 패킷
struct FLoginResponsePacket
{
    FPacketHeader Header;
    uint64_t SessionId;
    uint64_t Token;
    bool Success;

    // 생성자나 Init 함수를 만들어두면 서버에서 보내기 편함
    void Initialize(uint64_t sessionId, uint64_t token, bool success)
    {
        Header.Size = sizeof(FLoginResponsePacket);
        Header.ProtocolId = PKT_LOGIN_RES;
        SessionId = sessionId;
        Token = token;
        Success = success;
    }
};

#pragma pack(pop)