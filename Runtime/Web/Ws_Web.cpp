/**
 * @file Ws_Web.cpp
 * @brief Web WebSocket client — the browser WebSocket global behind the
 *        engine's WebSocket:: API.
 *
 * The engine's own implementation (Engine/Source/Network/WebSocketClient.cpp)
 * speaks RFC 6455 over a raw NET_ stream socket. A browser has no raw sockets
 * but does have a first-class WebSocket object that already does the
 * handshake, the framing, the masking and TLS — so this file supplies the whole
 * WebSocket:: namespace instead, and WebSocketClient.cpp compiles to nothing
 * here because the Makefile defines POLYPHASE_WS_PROVIDED_BY_ADDON.
 *
 * That define is the opt-in half of the engine's three-arm seam. A build-target
 * package that does NOT define it still links: WebSocketClient.cpp falls
 * through to its stub arm and WebSocket.IsAvailable() reports false. Nothing
 * about this file is load-bearing for other packages.
 *
 * Threading model: none. Every browser event lands in a per-socket JS queue,
 * and WebSocket::Tick() — called once per frame from Engine::Update, right
 * after Http::Tick() — drains it and fires callbacks on the main thread. That
 * ordering is what makes the Lua callbacks safe.
 *
 * Data is PULLED from JS, exactly like Http_Web.cpp: JS never calls _malloc or
 * an exported C function, so this needs no -sEXPORTED_FUNCTIONS /
 * -sEXPORTED_RUNTIME_METHODS and no extra link flags. Strings cross via
 * TextEncoder/TextDecoder rather than emscripten's UTF8 helpers, for the same
 * reason.
 *
 * Known divergences from the native transports, all forced by the platform:
 *   - options.headers is impossible. The browser WebSocket constructor takes a
 *     URL and a subprotocol list, nothing else. Ignored with a one-time warning.
 *   - wss:// is free here, and REQUIRED from an https-served page — a ws:// URL
 *     on an https page is blocked as mixed content. Never define
 *     POLYPHASE_WS_DOWNGRADE_WSS for a web target.
 *   - The outgoing-queue cap maps to the browser's own bufferedAmount.
 *   - Close codes are constrained by the browser: only 1000 or 3000-4999 may
 *     be passed to close(); anything else is coerced to 1000.
 */
#if defined(POLYPHASE_PLATFORM_ADDON) && defined(PLATFORM_WEB) && PLATFORM_WEB

#include "Network/WebSocketClient.h"
#include "Log.h"

#include <emscripten.h>

#include <deque>
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// JS side
// ---------------------------------------------------------------------------

// Event kinds pulled out of a socket's queue by PolyWsTakeEvent.
#define POLY_WS_EV_NONE    0
#define POLY_WS_EV_OPEN    1
#define POLY_WS_EV_MESSAGE 2
#define POLY_WS_EV_ERROR   3
#define POLY_WS_EV_CLOSE   4

EM_JS(void, PolyWsInit, (), {
    if (Module.__polyWs) return;

    var w = {
        socks: {},
        next:  1,
        dec:   new TextDecoder("utf-8"),
        enc:   new TextEncoder()
    };

    w.str = function(ptr) {
        if (!ptr) return "";
        var end = ptr;
        while (HEAPU8[end] !== 0) ++end;
        return w.dec.decode(HEAPU8.subarray(ptr, end));
    };

    Module.__polyWs = w;
});

// Returns a handle, or 0 if the URL was rejected by the browser.
EM_JS(int, PolyWsCreate, (const char* urlPtr, const char* protocolsPtr), {
    var w = Module.__polyWs;
    var url = w.str(urlPtr);
    var protoBlob = w.str(protocolsPtr);

    var s = {
        ws: null,
        q: [],          // queued events, in arrival order
        cur: null,      // the one being read out
        protocol: "",
        state: 0        // CONNECTING
    };

    try {
        if (protoBlob.length > 0) {
            s.ws = new WebSocket(url, protoBlob.split("\n"));
        } else {
            s.ws = new WebSocket(url);
        }
    } catch (e) {
        return 0;
    }

    s.ws.binaryType = "arraybuffer";

    s.ws.onopen = function() {
        s.state = 1;
        s.protocol = s.ws.protocol || "";
        s.q.push({ kind: 1 });
    };

    s.ws.onmessage = function(ev) {
        var bytes;
        var binary;
        if (typeof ev.data === "string") {
            bytes = w.enc.encode(ev.data);
            binary = 0;
        } else {
            bytes = new Uint8Array(ev.data);
            binary = 1;
        }
        s.q.push({ kind: 2, body: bytes, binary: binary });
    };

    s.ws.onerror = function() {
        // The browser deliberately withholds the reason; onclose follows and
        // carries whatever detail there is.
        s.q.push({ kind: 3, text: w.enc.encode("WebSocket error") });
    };

    s.ws.onclose = function(ev) {
        s.state = 3;
        s.q.push({
            kind: 4,
            code: ev.code,
            text: w.enc.encode(ev.reason || ""),
            clean: ev.wasClean ? 1 : 0
        });
    };

    var handle = w.next++;
    w.socks[handle] = s;
    return handle;
});

EM_JS(int, PolyWsState, (int handle), {
    var s = Module.__polyWs.socks[handle];
    if (!s) return 3;
    if (s.state === 3) return 3;
    if (!s.ws) return 3;
    // Map the browser readyState (same 0..3 ordering as WsState).
    return s.ws.readyState;
});

EM_JS(int, PolyWsSend, (int handle, const char* data, int len, int binary), {
    var s = Module.__polyWs.socks[handle];
    if (!s || !s.ws || s.ws.readyState !== 1) return 0;

    try {
        if (binary) {
            // Copy out of the heap: ALLOW_MEMORY_GROWTH can detach the
            // underlying ArrayBuffer, and a stale view would throw.
            s.ws.send(HEAPU8.slice(data, data + len));
        } else {
            s.ws.send(Module.__polyWs.dec.decode(HEAPU8.subarray(data, data + len)));
        }
    } catch (e) {
        return 0;
    }
    return 1;
});

EM_JS(int, PolyWsBufferedAmount, (int handle), {
    var s = Module.__polyWs.socks[handle];
    return (s && s.ws) ? s.ws.bufferedAmount : 0;
});

EM_JS(void, PolyWsClose, (int handle, int code, const char* reasonPtr), {
    var w = Module.__polyWs;
    var s = w.socks[handle];
    if (!s || !s.ws) return;

    // The browser only accepts 1000 or 3000-4999.
    var c = (code === 1000 || (code >= 3000 && code <= 4999)) ? code : 1000;
    try { s.ws.close(c, w.str(reasonPtr)); } catch (e) {}
});

EM_JS(int, PolyWsTakeEvent, (int handle), {
    var s = Module.__polyWs.socks[handle];
    if (!s || s.q.length === 0) return 0;
    s.cur = s.q.shift();
    return s.cur.kind;
});

EM_JS(int, PolyWsCurLen, (int handle), {
    var c = Module.__polyWs.socks[handle].cur;
    if (!c) return 0;
    if (c.body) return c.body.length;
    if (c.text) return c.text.length;
    return 0;
});

EM_JS(void, PolyWsCurCopy, (int handle, char* dst), {
    var c = Module.__polyWs.socks[handle].cur;
    if (!c) return;
    var b = c.body ? c.body : c.text;
    if (b && b.length > 0) HEAPU8.set(b, dst);
});

EM_JS(int, PolyWsCurBinary,    (int handle), { var c = Module.__polyWs.socks[handle].cur; return c ? (c.binary | 0) : 0; });
EM_JS(int, PolyWsCurCloseCode, (int handle), { var c = Module.__polyWs.socks[handle].cur; return c ? (c.code   | 0) : 0; });
EM_JS(int, PolyWsCurWasClean,  (int handle), { var c = Module.__polyWs.socks[handle].cur; return c ? (c.clean  | 0) : 0; });

EM_JS(void, PolyWsCurRelease, (int handle), {
    var s = Module.__polyWs.socks[handle];
    if (s) s.cur = null;
});

EM_JS(int, PolyWsProtocolLen, (int handle), {
    var s = Module.__polyWs.socks[handle];
    if (!s) return 0;
    if (!s.__protoEnc) s.__protoEnc = Module.__polyWs.enc.encode(s.protocol || "");
    return s.__protoEnc.length;
});

EM_JS(void, PolyWsProtocolCopy, (int handle, char* dst), {
    var s = Module.__polyWs.socks[handle];
    if (!s || !s.__protoEnc || s.__protoEnc.length === 0) return;
    HEAPU8.set(s.__protoEnc, dst);
});

EM_JS(void, PolyWsDestroy, (int handle), {
    var w = Module.__polyWs;
    var s = w.socks[handle];
    if (!s) return;

    if (s.ws) {
        s.ws.onopen = null;
        s.ws.onmessage = null;
        s.ws.onerror = null;
        s.ws.onclose = null;
        try { s.ws.close(); } catch (e) {}
    }
    delete w.socks[handle];
});

EM_JS(void, PolyWsDestroyAll, (), {
    var w = Module.__polyWs;
    if (!w) return;
    for (var k in w.socks) {
        var s = w.socks[k];
        if (s.ws) {
            s.ws.onopen = null;
            s.ws.onmessage = null;
            s.ws.onerror = null;
            s.ws.onclose = null;
            try { s.ws.close(); } catch (e) {}
        }
    }
    w.socks = {};
});

// ---------------------------------------------------------------------------
// C++ side
// ---------------------------------------------------------------------------

namespace
{
    bool sWarnedAboutHeaders = false;

    std::string ReadCurText(int handle)
    {
        const int len = PolyWsCurLen(handle);
        if (len <= 0)
        {
            return std::string();
        }

        std::string out;
        out.resize(size_t(len));
        PolyWsCurCopy(handle, &out[0]);
        return out;
    }

    class WsWebConnection : public WsConnection
    {
    public:

        explicit WsWebConnection(int handle, uint32_t maxQueuedBytes)
            : mHandle(handle)
            , mMaxQueuedBytes(maxQueuedBytes != 0 ? maxQueuedBytes : WS_DEFAULT_MAX_QUEUED_BYTES)
        {
        }

        ~WsWebConnection() override
        {
            if (mHandle != 0)
            {
                PolyWsDestroy(mHandle);
                mHandle = 0;
            }
        }

        // -- WsConnection ---------------------------------------------------

        WsState GetState() const override { return mState; }

        bool SendText(const char* data, uint32_t size) override
        {
            return Send((const uint8_t*)data, size, false);
        }

        bool SendBinary(const uint8_t* data, uint32_t size) override
        {
            return Send(data, size, true);
        }

        void Close(uint16_t code, const char* reason) override
        {
            if (mState == WsState::Closed || mState == WsState::Closing || mHandle == 0)
            {
                return;
            }

            mState = WsState::Closing;
            PolyWsClose(mHandle, (int)code, reason != nullptr ? reason : "");
        }

        void SetOpenCallback(WsOpenCallback cb) override      { mOpenCallback    = std::move(cb); }
        void SetMessageCallback(WsMessageCallback cb) override { mMessageCallback = std::move(cb); }
        void SetErrorCallback(WsErrorCallback cb) override     { mErrorCallback   = std::move(cb); }
        void SetClosedCallback(WsClosedCallback cb) override   { mClosedCallback  = std::move(cb); }

        uint32_t GetAvailablePacketCount() const override { return (uint32_t)mInQueue.size(); }

        bool TakePacket(WsMessage& outMessage) override
        {
            if (mInQueue.empty())
            {
                return false;
            }

            outMessage = std::move(mInQueue.front());
            mInQueue.pop_front();
            mInQueuedBytes -= (uint32_t)outMessage.mData.size();
            return true;
        }

        const std::string& GetSelectedProtocol() const override { return mSelectedProtocol; }
        uint16_t           GetCloseCode() const override        { return mCloseCode; }
        const std::string& GetCloseReason() const override      { return mCloseReason; }
        bool               WasDowngraded() const override       { return false; }

        // -- Pumped by WebSocket::Tick --------------------------------------

        void Pump()
        {
            if (mHandle == 0 || mState == WsState::Closed)
            {
                return;
            }

            for (;;)
            {
                const int kind = PolyWsTakeEvent(mHandle);
                if (kind == POLY_WS_EV_NONE)
                {
                    break;
                }

                switch (kind)
                {
                case POLY_WS_EV_OPEN:
                {
                    PolyWsCurRelease(mHandle);

                    const int protocolLen = PolyWsProtocolLen(mHandle);
                    if (protocolLen > 0)
                    {
                        mSelectedProtocol.resize(size_t(protocolLen));
                        PolyWsProtocolCopy(mHandle, &mSelectedProtocol[0]);
                    }

                    mState = WsState::Open;
                    if (mOpenCallback)
                    {
                        mOpenCallback();
                    }
                    break;
                }

                case POLY_WS_EV_MESSAGE:
                {
                    const int  len    = PolyWsCurLen(mHandle);
                    const bool binary = PolyWsCurBinary(mHandle) != 0;

                    std::vector<uint8_t> data;
                    data.resize(size_t(len > 0 ? len : 0));
                    if (len > 0)
                    {
                        PolyWsCurCopy(mHandle, (char*)data.data());
                    }
                    PolyWsCurRelease(mHandle);

                    DeliverMessage(data, binary);
                    break;
                }

                case POLY_WS_EV_ERROR:
                {
                    const std::string message = ReadCurText(mHandle);
                    PolyWsCurRelease(mHandle);

                    if (mErrorCallback)
                    {
                        mErrorCallback(message.empty() ? "WebSocket error" : message.c_str());
                    }
                    break;
                }

                case POLY_WS_EV_CLOSE:
                {
                    const uint16_t code     = (uint16_t)PolyWsCurCloseCode(mHandle);
                    const bool     wasClean = PolyWsCurWasClean(mHandle) != 0;
                    const std::string reason = ReadCurText(mHandle);
                    PolyWsCurRelease(mHandle);

                    mCloseCode   = code != 0 ? code : uint16_t(WS_CLOSE_ABNORMAL);
                    mCloseReason = reason;
                    mState       = WsState::Closed;

                    // Closed is terminal, so release every callback here - a
                    // Lua callback that captured this handle would otherwise
                    // form a registry -> closure -> userdata cycle the
                    // collector can never break. Same reasoning as the engine
                    // implementation's FinishClosed().
                    WsClosedCallback closedCallback;
                    closedCallback.swap(mClosedCallback);
                    mOpenCallback    = WsOpenCallback();
                    mMessageCallback = WsMessageCallback();
                    mErrorCallback   = WsErrorCallback();

                    if (closedCallback)
                    {
                        closedCallback(mCloseCode, mCloseReason.c_str(), wasClean);
                    }
                    return;
                }

                default:
                    PolyWsCurRelease(mHandle);
                    break;
                }

                if (mState == WsState::Closed)
                {
                    return;
                }
            }
        }

    private:

        bool Send(const uint8_t* data, uint32_t size, bool binary)
        {
            if (mState != WsState::Open || mHandle == 0)
            {
                return false;
            }

            const uint32_t buffered = (uint32_t)PolyWsBufferedAmount(mHandle);
            if (uint64_t(buffered) + uint64_t(size) > uint64_t(mMaxQueuedBytes))
            {
                if (!mLoggedSendOverflow)
                {
                    mLoggedSendOverflow = true;
                    LogWarning("WebSocket: outgoing queue is full (%u bytes buffered, cap %u) - dropping sends",
                        (unsigned)buffered, (unsigned)mMaxQueuedBytes);
                }
                return false;
            }

            return PolyWsSend(mHandle, (const char*)data, (int)size, binary ? 1 : 0) != 0;
        }

        void DeliverMessage(std::vector<uint8_t>& data, bool binary)
        {
            if (mMessageCallback)
            {
                mMessageCallback(data.empty() ? nullptr : data.data(), (uint32_t)data.size(), binary);
                return;
            }

            if (mInQueuedBytes + data.size() > mMaxQueuedBytes)
            {
                if (!mLoggedRecvOverflow)
                {
                    mLoggedRecvOverflow = true;
                    LogWarning("WebSocket: incoming queue is full (cap %u bytes) - dropping messages. "
                               "Set a message callback or drain with GetPacket().",
                        (unsigned)mMaxQueuedBytes);
                }
                return;
            }

            WsMessage queued;
            queued.mBinary = binary;
            queued.mData.swap(data);
            mInQueuedBytes += (uint32_t)queued.mData.size();
            mInQueue.push_back(std::move(queued));
        }

        int      mHandle = 0;
        WsState  mState  = WsState::Connecting;

        std::string mSelectedProtocol;
        std::string mCloseReason;
        uint16_t    mCloseCode = 0;

        std::deque<WsMessage> mInQueue;
        uint32_t              mInQueuedBytes  = 0;
        uint32_t              mMaxQueuedBytes = WS_DEFAULT_MAX_QUEUED_BYTES;
        bool                  mLoggedSendOverflow = false;
        bool                  mLoggedRecvOverflow = false;

        WsOpenCallback    mOpenCallback;
        WsMessageCallback mMessageCallback;
        WsErrorCallback   mErrorCallback;
        WsClosedCallback  mClosedCallback;
    };

    struct WebWsState
    {
        std::vector<std::weak_ptr<WsWebConnection> > mConnections;
    };

    WebWsState* sState = nullptr;
}

namespace WebSocket
{
    void Initialize()
    {
        if (sState != nullptr)
        {
            return;
        }

        PolyWsInit();
        sState = new WebWsState();
    }

    void Shutdown()
    {
        if (sState == nullptr)
        {
            return;
        }

        PolyWsDestroyAll();

        delete sState;
        sState = nullptr;
    }

    void Tick()
    {
        if (sState == nullptr)
        {
            return;
        }

        // Lock every live connection before pumping: a Lua callback fired from
        // Pump() can call WebSocket.Connect, which appends to mConnections.
        std::vector<std::shared_ptr<WsWebConnection> > live;
        live.reserve(sState->mConnections.size());

        size_t write = 0;
        for (size_t i = 0; i < sState->mConnections.size(); ++i)
        {
            std::shared_ptr<WsWebConnection> conn = sState->mConnections[i].lock();
            if (conn == nullptr)
            {
                continue;
            }

            sState->mConnections[write++] = sState->mConnections[i];
            live.push_back(conn);
        }
        sState->mConnections.resize(write);

        for (size_t i = 0; i < live.size(); ++i)
        {
            live[i]->Pump();
        }
    }

    bool IsAvailable()
    {
        return sState != nullptr;
    }

    const char* GetMissingDependencyMessage()
    {
        return sState != nullptr ? "" : "WebSocket::Initialize() was not called";
    }

    std::shared_ptr<WsConnection> Connect(const WsConnectOptions& options, std::string& outError)
    {
        outError.clear();

        if (sState == nullptr)
        {
            outError = "WebSocket::Initialize() was not called";
            return nullptr;
        }

        if (options.mUrl.compare(0, 5, "ws://") != 0 && options.mUrl.compare(0, 6, "wss://") != 0)
        {
            outError = "URL is missing a ws:// or wss:// scheme";
            return nullptr;
        }

        if (!options.mHeaders.empty() && !sWarnedAboutHeaders)
        {
            sWarnedAboutHeaders = true;
            LogWarning("WebSocket: options.headers is ignored on web builds - the browser "
                       "WebSocket constructor cannot set request headers. Put the token in the "
                       "URL query or send it as the first message instead.");
        }

        std::string protocolBlob;
        for (size_t i = 0; i < options.mProtocols.size(); ++i)
        {
            if (i != 0) protocolBlob += '\n';
            protocolBlob += options.mProtocols[i];
        }

        const int handle = PolyWsCreate(options.mUrl.c_str(), protocolBlob.c_str());
        if (handle == 0)
        {
            outError = "The browser refused the WebSocket URL (bad URL, or ws:// from an https page)";
            return nullptr;
        }

        std::shared_ptr<WsWebConnection> conn =
            std::make_shared<WsWebConnection>(handle, options.mMaxQueuedBytes);

        sState->mConnections.push_back(conn);
        return conn;
    }
}

#endif  // POLYPHASE_PLATFORM_ADDON && PLATFORM_WEB
