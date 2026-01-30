#pragma once

#include "../Common/Types.h"


// Forward declarations - Network
class CSocket;
class CGameSession;
class CListener;

// Forward declarations - Threading
class CLocalThread;
class CIOThreadPool;
class CThreadManager;

// Forward declarations - Buffer
struct FPacketBuffer;

// Callback function types
typedef void (*FnAcceptCallback)(CSocket*);
typedef void (*FnDisconnectCallback)(SessionId);
typedef void (*FnPacketHandler)(CGameSession*, FPacketBuffer*);
typedef void (*FnSessionStateCallback)(CGameSession*, ESessionState, ESessionState);
