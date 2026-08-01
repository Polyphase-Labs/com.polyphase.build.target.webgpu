/**
 * @file Network_Web.cpp
 * @brief Web networking backend — the engine's UDP NET_* surface over a WebSocket relay.
 *
 * Browsers expose no raw UDP or TCP sockets, so the engine's connectionless
 * NET_* API cannot be satisfied directly. Every socket here is instead a
 * WebSocket to a relay process (Tools/relay/) that owns a real UDP socket on
 * the client's behalf and forwards datagrams both ways. The peer address rides
 * in a small frame header, which is what keeps the API connectionless from the
 * engine's point of view.
 *
 * One WebSocket per SocketHandle. NetworkManager only ever opens two (the game
 * socket and the discovery socket), and one-WS-per-socket avoids needing a
 * socket-id field in the framing.
 *
 * Discovery works unmodified: BeginSessionSearch binds a socket to
 * OCT_BROADCAST_PORT, the relay binds its UDP socket to the same port with
 * SO_REUSEADDR, and native hosts' NetMsgBroadcast packets arrive with their
 * true source address. UpdateSearch() parses them exactly as it does natively.
 *
 * NET_IsActive() is true only when a relay URL is configured, so a build with
 * no relay behaves exactly like the old stub instead of pretending to work.
 * Set it in shell.html:  window.POLYPHASE_RELAY_URL = "ws://localhost:5150";
 *
 * Implemented in EM_JS against the plain WebSocket global rather than
 * emscripten_websocket_* so the build needs no -lwebsocket.js link flag. Data
 * is pulled from JS by the C++ side (see Http_Web.cpp for the same pattern) so
 * no C function has to be exported to JS either.
 *
 * IP addresses are host-order uint32 with the first octet in the MSB
 * (1.2.3.4 == 0x01020304), matching NET_IpStringToUint32 on the native
 * backends, which returns ntohl(inet_pton(...)).
 *
 * Built only when POLYPHASE_PLATFORM_ADDON is defined.
 */
#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Network/Network.h"
#include "Network/NetworkConstants.h"
#include "Log.h"

#include <emscripten.h>

#include <stdint.h>
#include <stdio.h>

// Relay frame opcodes. Must match Tools/relay/relay.js.
#define POLY_NET_OP_BIND      1   // client->relay: [1][u16 port]
#define POLY_NET_OP_DATA      2   // both ways:     [2][u32 ip][u16 port][payload]
#define POLY_NET_OP_BROADCAST 3   // client->relay: [3][u8 enable]
#define POLY_NET_OP_BOUND     4   // relay->client: [4][u32 localIp][u16 localPort]

// ---------------------------------------------------------------------------
// JS side
// ---------------------------------------------------------------------------

EM_JS(int, PolyNetInit, (), {
    if (Module.__polyNet) return Module.__polyNet.url ? 1 : 0;

    var n = {
        url:     (typeof window !== "undefined" && window.POLYPHASE_RELAY_URL) || "",
        socks:   {},
        nextId:  1,
        localIp: 0
    };
    Module.__polyNet = n;

    if (!n.url) {
        console.log("[polyphase] No window.POLYPHASE_RELAY_URL set -- multiplayer networking disabled.");
        return 0;
    }
    console.log("[polyphase] Network relay: " + n.url);
    return 1;
});

EM_JS(int, PolyNetCreate, (), {
    var n = Module.__polyNet;
    if (!n || !n.url) return -1;

    var id = n.nextId++;
    var s = {
        ws:        null,
        open:      false,
        pending:   [],    // frames queued until the socket opens
        rx:        [],    // received datagrams awaiting NET_SocketRecvFrom
        cur:       null,
        localPort: 0,
        closed:    false
    };
    n.socks[id] = s;

    var ws;
    try {
        ws = new WebSocket(n.url);
    } catch (e) {
        console.error("[polyphase] relay connect failed: " + e);
        delete n.socks[id];
        return -1;
    }
    ws.binaryType = "arraybuffer";
    s.ws = ws;

    ws.onopen = function() {
        s.open = true;
        for (var i = 0; i < s.pending.length; ++i) ws.send(s.pending[i]);
        s.pending = [];
    };

    ws.onmessage = function(ev) {
        var b = new Uint8Array(ev.data);
        if (b.length < 1) return;

        if (b[0] === 2 && b.length >= 7) {          // DATA
            var ip = (b[1] << 24 | b[2] << 16 | b[3] << 8 | b[4]) >>> 0;
            var port = (b[5] << 8 | b[6]);
            s.rx.push({ ip: ip, port: port, data: b.slice(7) });
        } else if (b[0] === 4 && b.length >= 7) {   // BOUND
            n.localIp = (b[1] << 24 | b[2] << 16 | b[3] << 8 | b[4]) >>> 0;
            s.localPort = (b[5] << 8 | b[6]);
        }
    };

    ws.onerror = function() {
        console.error("[polyphase] relay socket error");
    };

    ws.onclose = function() {
        s.open = false;
        s.closed = true;
    };

    return id;
});

EM_JS(void, PolyNetBind, (int handle, int port), {
    var n = Module.__polyNet;
    var s = n && n.socks[handle];
    if (!s) return;

    var f = new Uint8Array(3);
    f[0] = 1;
    f[1] = (port >> 8) & 0xFF;
    f[2] = port & 0xFF;

    if (s.open) s.ws.send(f); else s.pending.push(f);
});

EM_JS(void, PolyNetSetBroadcast, (int handle, int enable), {
    var n = Module.__polyNet;
    var s = n && n.socks[handle];
    if (!s) return;

    var f = new Uint8Array(2);
    f[0] = 3;
    f[1] = enable ? 1 : 0;

    if (s.open) s.ws.send(f); else s.pending.push(f);
});

EM_JS(int, PolyNetSendTo, (int handle, const char* buf, int len, unsigned int ip, int port), {
    var n = Module.__polyNet;
    var s = n && n.socks[handle];
    if (!s || s.closed) return -1;

    var f = new Uint8Array(7 + len);
    f[0] = 2;
    f[1] = (ip >>> 24) & 0xFF;
    f[2] = (ip >>> 16) & 0xFF;
    f[3] = (ip >>> 8) & 0xFF;
    f[4] = ip & 0xFF;
    f[5] = (port >> 8) & 0xFF;
    f[6] = port & 0xFF;
    if (len > 0) f.set(HEAPU8.subarray(buf, buf + len), 7);

    // Queue rather than drop while the WebSocket is still opening -- the
    // engine's first Connect message goes out immediately after socket
    // creation and would otherwise always be lost.
    if (s.open) s.ws.send(f); else s.pending.push(f);
    return len;
});

// Moves the next datagram into the read-out slot. Returns its payload length,
// or -1 when nothing is queued (the engine's recv loops test for > 0).
EM_JS(int, PolyNetTake, (int handle), {
    var n = Module.__polyNet;
    var s = n && n.socks[handle];
    if (!s || s.rx.length === 0) return -1;
    s.cur = s.rx.shift();
    return s.cur.data.length;
});

EM_JS(unsigned int, PolyNetCurIp,   (int handle), { return Module.__polyNet.socks[handle].cur.ip;   });
EM_JS(int,          PolyNetCurPort, (int handle), { return Module.__polyNet.socks[handle].cur.port; });

EM_JS(void, PolyNetCurCopy, (int handle, char* dst, int maxLen), {
    var s = Module.__polyNet.socks[handle];
    var d = s.cur.data;
    var n = d.length < maxLen ? d.length : maxLen;
    if (n > 0) HEAPU8.set(d.subarray(0, n), dst);
});

EM_JS(void, PolyNetClose, (int handle), {
    var n = Module.__polyNet;
    var s = n && n.socks[handle];
    if (!s) return;
    try { if (s.ws) s.ws.close(); } catch (e) {}
    delete n.socks[handle];
});

EM_JS(unsigned int, PolyNetLocalIp, (), {
    var n = Module.__polyNet;
    return n ? n.localIp : 0;
});

EM_JS(int, PolyNetLocalPort, (int handle), {
    var n = Module.__polyNet;
    var s = n && n.socks[handle];
    return s ? s.localPort : 0;
});

// ---------------------------------------------------------------------------
// C++ side
// ---------------------------------------------------------------------------

namespace
{
    bool sRelayConfigured = false;
}

void NET_Initialize()
{
    sRelayConfigured = (PolyNetInit() != 0);
}

void NET_Shutdown()
{
    sRelayConfigured = false;
}

void NET_Update()
{
    // Nothing to pump -- WebSocket callbacks fill the receive queues from the
    // browser event loop, and the engine drains them via NET_SocketRecvFrom.
}

bool NET_IsActive()
{
    return sRelayConfigured;
}

SocketHandle NET_SocketCreate()
{
    if (!sRelayConfigured)
    {
        return NET_INVALID_SOCKET;
    }

    const int handle = PolyNetCreate();
    if (handle < 0)
    {
        LogError("Failed to open relay socket.");
        return NET_INVALID_SOCKET;
    }

    return SocketHandle(handle);
}

void NET_SocketBind(SocketHandle socketHandle, uint32_t ipAddr, uint16_t port)
{
    // ipAddr is always NET_ANY_IP in engine use; the relay owns the interface.
    (void)ipAddr;

    if (socketHandle >= 0)
    {
        PolyNetBind(int(socketHandle), int(port));
    }
}

int32_t NET_SocketRecvFrom(SocketHandle socketHandle, char* buffer, uint32_t size, uint32_t& addr, uint16_t& port)
{
    if (socketHandle < 0)
    {
        return -1;
    }

    const int len = PolyNetTake(int(socketHandle));
    if (len < 0)
    {
        return -1;
    }

    addr = PolyNetCurIp(int(socketHandle));
    port = uint16_t(PolyNetCurPort(int(socketHandle)));

    const int copyLen = (len < int(size)) ? len : int(size);
    PolyNetCurCopy(int(socketHandle), buffer, copyLen);

    if (len > int(size))
    {
        LogWarning("Truncated datagram (%d bytes into a %u byte buffer)", len, size);
    }

    return copyLen;
}

int32_t NET_SocketSendTo(SocketHandle socketHandle, const char* buffer, uint32_t size, uint32_t addr, uint16_t port)
{
    if (socketHandle < 0)
    {
        return -1;
    }

    return PolyNetSendTo(int(socketHandle), buffer, int(size), addr, int(port));
}

void NET_SocketSetBlocking(SocketHandle socketHandle, bool blocking)
{
    // Always non-blocking. There is no way to block on a browser event loop,
    // and the engine only ever asks for non-blocking anyway.
    (void)socketHandle;
    (void)blocking;
}

void NET_SocketSetBroadcast(SocketHandle socketHandle, bool broadcast)
{
    if (socketHandle >= 0)
    {
        PolyNetSetBroadcast(int(socketHandle), broadcast ? 1 : 0);
    }
}

void NET_SocketGetIpAndPort(SocketHandle socketHandle, uint32_t& outIp, uint16_t& outPort)
{
    outIp = PolyNetLocalIp();
    outPort = (socketHandle >= 0) ? uint16_t(PolyNetLocalPort(int(socketHandle))) : 0;
}

void NET_SocketClose(SocketHandle socketHandle)
{
    if (socketHandle >= 0)
    {
        PolyNetClose(int(socketHandle));
    }
}

// --- Stream sockets ---------------------------------------------------------
// No TCP in the browser. HTTP is served by Http_Web.cpp via fetch(); nothing
// else in the engine needs a stream socket on this platform.

int32_t NET_SocketRecv(SocketHandle, char*, uint32_t) { return -1; }
SocketHandle NET_SocketCreateStream()                 { return NET_INVALID_SOCKET; }
bool NET_SocketConnect(SocketHandle, uint32_t, uint16_t, int32_t) { return false; }
int32_t NET_SocketSend(SocketHandle, const char*, uint32_t)       { return -1; }

// --- Address helpers --------------------------------------------------------

uint32_t NET_IpStringToUint32(const char* ipString)
{
    if (ipString == nullptr)
    {
        return 0;
    }

    uint32_t octets[4] = {};
    int count = 0;
    uint32_t value = 0;
    bool haveDigit = false;

    for (const char* c = ipString; ; ++c)
    {
        if (*c >= '0' && *c <= '9')
        {
            value = value * 10 + uint32_t(*c - '0');
            if (value > 255)
            {
                return 0;
            }
            haveDigit = true;
        }
        else if (*c == '.' || *c == '\0')
        {
            if (!haveDigit || count >= 4)
            {
                return 0;
            }

            octets[count++] = value;
            value = 0;
            haveDigit = false;

            if (*c == '\0')
            {
                break;
            }
        }
        else
        {
            return 0;   // not a dotted quad (a hostname, most likely)
        }
    }

    if (count != 4)
    {
        return 0;
    }

    return (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
}

uint32_t NET_ResolveHost(const char* hostname)
{
    // No synchronous DNS in a browser. Literal dotted-quads still resolve;
    // real names would need an async round trip through the relay, and the only
    // caller (the Dolphin HTTP backend) isn't built here.
    return NET_IpStringToUint32(hostname);
}

void NET_IpUint32ToString(uint32_t ip, char* outIpString)
{
    if (outIpString == nullptr) return;
    sprintf(outIpString, "%u.%u.%u.%u",
                 (unsigned)((ip >> 24) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
                 (unsigned)((ip >> 8) & 0xFF),  (unsigned)(ip & 0xFF));
}

uint32_t NET_GetIpAddress()
{
    // Whatever the relay reports as its own LAN address.
    return PolyNetLocalIp();
}

uint32_t NET_GetSubnetMask()
{
    // 0 makes NetworkManager::OpenSession compute a broadcast IP of
    // 255.255.255.255 (netIp | ~mask), which the relay turns into a real LAN
    // broadcast. Same reasoning as the PLATFORM_ANDROID branch there.
    return 0;
}

#endif // POLYPHASE_PLATFORM_ADDON
