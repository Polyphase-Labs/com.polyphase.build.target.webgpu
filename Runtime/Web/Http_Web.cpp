/**
 * @file Http_Web.cpp
 * @brief Web HTTP backend — the browser fetch() API behind the engine's Http:: API.
 *
 * The engine's HttpBackend interface (Network/Http/Backends/HttpBackend.h) is
 * deliberately BLOCKING: HttpClient.cpp wraps it in a worker thread to make the
 * public API async. The browser build is single-threaded (no -pthread, no
 * -sASYNCIFY), so that seam is unusable here. Instead this file implements the
 * whole Http:: namespace directly — fetch() is already async, which is exactly
 * what the public API wants.
 *
 * HttpClient.cpp compiles to nothing when PLATFORM_WEB is set, leaving these
 * definitions as the only ones in the link. Same arrangement as Network_Web.cpp
 * providing the NET_* surface.
 *
 * Threading model: none. Every completion lands in a JS-side queue, and
 * Http::Tick() (called once per frame from Engine::Update, after the network
 * pump) drains it and fires callbacks on the main thread. That ordering is what
 * makes Lua callbacks safe.
 *
 * Data is PULLED from JS rather than pushed into C++. JS never calls _malloc or
 * an exported C function — it parks the result and C++ asks for the sizes, then
 * hands down buffers it owns. That keeps this file free of any
 * -sEXPORTED_FUNCTIONS / -sEXPORTED_RUNTIME_METHODS requirement, so the build
 * needs no new link flags. String conversion uses TextEncoder/TextDecoder
 * (browser globals) rather than emscripten's UTF8ToString/stringToUTF8 helpers,
 * for the same reason.
 *
 * Known divergences from the native backends, all forced by the platform:
 *   - SendSync() cannot work. A browser cannot block the main thread on a
 *     network round trip. It returns HttpError::Unavailable.
 *   - CORS applies. The server must send Access-Control-Allow-Origin or the
 *     request fails with status 0 (reported as HttpError::Network).
 *   - VerifySsl and MaxRedirects are ignored; the browser owns both policies.
 *   - MaxBodyBytes is enforced after the body arrives, not streamed.
 */
#if defined(POLYPHASE_PLATFORM_ADDON) && defined(PLATFORM_WEB) && PLATFORM_WEB

#include "Network/Http/HttpClient.h"
#include "Log.h"

#include <emscripten.h>

#include <atomic>
#include <memory>
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// JS side
// ---------------------------------------------------------------------------

// Error codes crossing the boundary. Kept as small ints so the JS half doesn't
// need to know about the HttpError enum; MapJsError() below is the only place
// that has to agree.
#define POLY_HTTP_JS_OK        0
#define POLY_HTTP_JS_TIMEOUT   1
#define POLY_HTTP_JS_CANCELLED 2
#define POLY_HTTP_JS_NETWORK   3

// Text slot ids for the Cur*Text accessors.
#define POLY_HTTP_TEXT_HEADERS  0
#define POLY_HTTP_TEXT_FINALURL 1
#define POLY_HTTP_TEXT_ERRMSG   2

EM_JS(void, PolyHttpInit, (), {
    if (Module.__polyHttp) return;

    var h = {
        done: [],     // completed results awaiting pickup by Tick()
        cur:  null,   // the one currently being read out
        ctrl: {},     // id -> AbortController for in-flight requests
        dec:  new TextDecoder("utf-8"),
        enc:  new TextEncoder()
    };

    // Read a NUL-terminated UTF-8 string out of the heap without depending on
    // emscripten's UTF8ToString being present in the build.
    h.str = function(ptr) {
        if (!ptr) return "";
        var end = ptr;
        while (HEAPU8[end] !== 0) ++end;
        return h.dec.decode(HEAPU8.subarray(ptr, end));
    };

    // Encode once, cache on the result object, so Len() and Copy() agree even
    // if the string contains multi-byte characters.
    h.textBytes = function(res, which) {
        if (!res.__enc) res.__enc = {};
        if (res.__enc[which] === undefined) {
            var s = (which === 0) ? res.headers
                  : (which === 1) ? res.finalUrl
                  :                 res.errMsg;
            res.__enc[which] = h.enc.encode(s || "");
        }
        return res.__enc[which];
    };

    Module.__polyHttp = h;
});

EM_JS(void, PolyHttpSend, (int id,
                           const char* methodPtr,
                           const char* urlPtr,
                           const char* headersPtr,
                           const char* bodyPtr,
                           int bodyLen,
                           int timeoutMs), {
    var h = Module.__polyHttp;
    var url = h.str(urlPtr);

    var init = { method: h.str(methodPtr) };

    var headerBlob = h.str(headersPtr);
    if (headerBlob.length > 0) {
        var hdrs = {};
        headerBlob.split("\n").forEach(function(line) {
            if (!line) return;
            var i = line.indexOf(":");
            if (i > 0) hdrs[line.substring(0, i)] = line.substring(i + 1);
        });
        init.headers = hdrs;
    }

    if (bodyLen > 0) {
        // Copy out of the heap: ALLOW_MEMORY_GROWTH can detach the underlying
        // ArrayBuffer while the request is in flight, and a view into the old
        // buffer would throw when fetch() reads it.
        init.body = HEAPU8.slice(bodyPtr, bodyPtr + bodyLen);
    }

    var ctrl = new AbortController();
    init.signal = ctrl.signal;
    h.ctrl[id] = ctrl;

    var timedOut = false;
    var timer = 0;
    if (timeoutMs > 0) {
        timer = setTimeout(function() { timedOut = true; ctrl.abort(); }, timeoutMs);
    }

    var finish = function(res) {
        if (timer) clearTimeout(timer);
        delete h.ctrl[id];
        res.id = id;
        h.done.push(res);
    };

    var status = 0;
    var finalUrl = url;
    var headerOut = "";

    fetch(url, init).then(function(resp) {
        status = resp.status;
        finalUrl = resp.url || url;
        resp.headers.forEach(function(v, k) { headerOut += k + ":" + v + "\n"; });
        return resp.arrayBuffer();
    }).then(function(buf) {
        finish({
            status:   status,
            err:      0,
            headers:  headerOut,
            finalUrl: finalUrl,
            errMsg:   "",
            body:     new Uint8Array(buf)
        });
    }).catch(function(e) {
        var code = timedOut ? 1 : (ctrl.signal.aborted ? 2 : 3);
        finish({
            status:   status,
            err:      code,
            headers:  headerOut,
            finalUrl: finalUrl,
            // A CORS rejection or DNS failure surfaces here as an opaque
            // "Failed to fetch" with status 0 -- the browser deliberately
            // withholds the detail. Say so, so it isn't read as a dead server.
            errMsg:   String((e && e.message) ? e.message : e) +
                      (code === 3 ? " (status 0 usually means a CORS rejection or an unreachable host)" : ""),
            body:     new Uint8Array(0)
        });
    });
});

// Moves the next completed result into the read-out slot. Returns its id, or 0
// when the queue is empty.
EM_JS(int, PolyHttpTakeNext, (), {
    var h = Module.__polyHttp;
    if (!h || h.done.length === 0) return 0;
    h.cur = h.done.shift();
    return h.cur.id;
});

EM_JS(int, PolyHttpCurStatus,  (), { return Module.__polyHttp.cur.status; });
EM_JS(int, PolyHttpCurError,   (), { return Module.__polyHttp.cur.err;    });
EM_JS(int, PolyHttpCurBodyLen, (), { return Module.__polyHttp.cur.body.length; });

EM_JS(void, PolyHttpCurCopyBody, (char* dst), {
    var b = Module.__polyHttp.cur.body;
    if (b.length > 0) HEAPU8.set(b, dst);
});

EM_JS(int, PolyHttpCurTextLen, (int which), {
    var h = Module.__polyHttp;
    return h.textBytes(h.cur, which).length;
});

EM_JS(void, PolyHttpCurCopyText, (int which, char* dst), {
    var h = Module.__polyHttp;
    var b = h.textBytes(h.cur, which);
    if (b.length > 0) HEAPU8.set(b, dst);
});

EM_JS(void, PolyHttpCurRelease, (), { Module.__polyHttp.cur = null; });

EM_JS(void, PolyHttpAbort, (int id), {
    var h = Module.__polyHttp;
    if (!h) return;
    var c = h.ctrl[id];
    if (c) c.abort();
});

EM_JS(void, PolyHttpAbortAll, (), {
    var h = Module.__polyHttp;
    if (!h) return;
    for (var k in h.ctrl) { h.ctrl[k].abort(); }
    h.ctrl = {};
    h.done = [];
    h.cur = null;
});

// ---------------------------------------------------------------------------
// C++ side
// ---------------------------------------------------------------------------

namespace
{
    struct PendingWeb
    {
        HttpResponseCallback               callback;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        int64_t                            maxBodyBytes = 0;
        bool                               abortSent    = false;
    };

    struct WebHttpState
    {
        std::unordered_map<uint32_t, PendingWeb> pending;
        uint32_t nextId = 1;
    };

    WebHttpState* sState = nullptr;

    HttpError MapJsError(int code)
    {
        switch (code)
        {
        case POLY_HTTP_JS_OK:        return HttpError::None;
        case POLY_HTTP_JS_TIMEOUT:   return HttpError::Timeout;
        case POLY_HTTP_JS_CANCELLED: return HttpError::Cancelled;
        case POLY_HTTP_JS_NETWORK:   return HttpError::Network;
        default:                     return HttpError::Unknown;
        }
    }

    std::string ReadCurText(int which)
    {
        const int len = PolyHttpCurTextLen(which);
        if (len <= 0)
        {
            return std::string();
        }

        std::string out;
        out.resize(size_t(len));
        PolyHttpCurCopyText(which, &out[0]);
        return out;
    }

    // "Key:Value\n" blob -> HttpHeaderMap.
    void ParseHeaderBlob(const std::string& blob, HttpHeaderMap& outHeaders)
    {
        size_t pos = 0;
        while (pos < blob.size())
        {
            size_t eol = blob.find('\n', pos);
            if (eol == std::string::npos)
            {
                eol = blob.size();
            }

            const size_t colon = blob.find(':', pos);
            if (colon != std::string::npos && colon < eol)
            {
                std::string key   = blob.substr(pos, colon - pos);
                std::string value = blob.substr(colon + 1, eol - colon - 1);
                outHeaders[std::move(key)] = std::move(value);
            }

            pos = eol + 1;
        }
    }

    std::string BuildHeaderBlob(const HttpHeaderMap& headers)
    {
        std::string blob;
        for (const auto& kv : headers)
        {
            // Newlines would forge a header split; HTTP forbids them in values
            // anyway, so drop the entry rather than sanitize it silently.
            if (kv.first.find('\n') != std::string::npos ||
                kv.second.find('\n') != std::string::npos ||
                kv.first.find(':') != std::string::npos)
            {
                LogWarning("Http: dropping malformed request header '%s'", kv.first.c_str());
                continue;
            }

            blob += kv.first;
            blob += ':';
            blob += kv.second;
            blob += '\n';
        }
        return blob;
    }
}

namespace Http
{
    void Initialize()
    {
        if (sState != nullptr)
        {
            return;
        }

        sState = new WebHttpState();
        PolyHttpInit();
    }

    void Shutdown()
    {
        if (sState == nullptr)
        {
            return;
        }

        if (!sState->pending.empty())
        {
            LogWarning("Http::Shutdown aborted %zu in-flight request(s)", sState->pending.size());
        }

        PolyHttpAbortAll();

        delete sState;
        sState = nullptr;
    }

    void Tick()
    {
        if (sState == nullptr)
        {
            return;
        }

        // Push cancellations down to the browser. HttpHandle::Cancel only sets
        // the shared flag -- it has no platform hook -- so this is where an
        // abort actually reaches fetch().
        for (auto& kv : sState->pending)
        {
            PendingWeb& p = kv.second;
            if (!p.abortSent &&
                p.cancelFlag != nullptr &&
                p.cancelFlag->load(std::memory_order_acquire))
            {
                p.abortSent = true;
                PolyHttpAbort(int(kv.first));
            }
        }

        // Drain completions. Each iteration hands one result back; the JS queue
        // is only appended to from fetch() callbacks, which cannot interleave
        // with this loop (single-threaded event loop).
        for (;;)
        {
            const int id = PolyHttpTakeNext();
            if (id == 0)
            {
                break;
            }

            HttpResponse response;
            response.SetStatus(PolyHttpCurStatus());

            const int jsErr = PolyHttpCurError();
            if (jsErr != POLY_HTTP_JS_OK)
            {
                response.SetError(MapJsError(jsErr), ReadCurText(POLY_HTTP_TEXT_ERRMSG));
            }

            response.SetFinalUrl(ReadCurText(POLY_HTTP_TEXT_FINALURL));
            ParseHeaderBlob(ReadCurText(POLY_HTTP_TEXT_HEADERS), response.MutableHeaders());

            const int bodyLen = PolyHttpCurBodyLen();
            if (bodyLen > 0)
            {
                std::vector<uint8_t>& body = response.MutableBody();
                body.resize(size_t(bodyLen));
                PolyHttpCurCopyBody((char*)body.data());
            }

            PolyHttpCurRelease();

            auto it = sState->pending.find(uint32_t(id));
            if (it == sState->pending.end())
            {
                // Completed after Shutdown, or an id we never issued.
                continue;
            }

            PendingWeb pending = std::move(it->second);
            sState->pending.erase(it);

            if (pending.maxBodyBytes > 0 &&
                int64_t(response.GetBody().size()) > pending.maxBodyBytes)
            {
                response.MutableBody().clear();
                response.SetError(HttpError::TooLarge, "Response body exceeded MaxBodyBytes");
            }

            if (pending.cancelFlag != nullptr &&
                pending.cancelFlag->load(std::memory_order_acquire) &&
                response.GetError() == HttpError::None)
            {
                response.SetError(HttpError::Cancelled, "Cancelled");
            }

            if (pending.callback)
            {
                pending.callback(response);
            }
        }
    }

    bool IsAvailable()
    {
        return sState != nullptr;
    }

    const char* GetMissingDependencyMessage()
    {
        return (sState != nullptr) ? "" : "Http::Initialize() was not called";
    }

    HttpHandle Send(HttpRequest req, HttpResponseCallback cb)
    {
        std::shared_ptr<std::atomic<bool>> flag = std::make_shared<std::atomic<bool>>(false);

        if (sState == nullptr)
        {
            HttpResponse r;
            r.SetError(HttpError::NotInitialized, "Http::Initialize() was not called");
            if (cb)
            {
                cb(r);
            }
            return HttpHandle(0, flag);
        }

        const uint32_t id = sState->nextId++;
        if (sState->nextId == 0)
        {
            sState->nextId = 1;   // 0 is the "queue empty" sentinel
        }

        PendingWeb p;
        p.callback     = std::move(cb);
        p.cancelFlag   = flag;
        p.maxBodyBytes = req.GetMaxBodyBytes();
        sState->pending[id] = std::move(p);

        const std::string headerBlob = BuildHeaderBlob(req.GetHeaders());
        const std::vector<uint8_t>& body = req.GetBody();

        PolyHttpSend(int(id),
                     HttpVerbToString(req.GetVerb()),
                     req.GetUrl().c_str(),
                     headerBlob.c_str(),
                     body.empty() ? nullptr : (const char*)body.data(),
                     int(body.size()),
                     req.GetTimeoutMs());

        return HttpHandle(id, flag);
    }

    HttpHandle Get(const std::string& url, HttpResponseCallback cb)
    {
        return Send(HttpRequest(HttpVerb::Get, url), std::move(cb));
    }

    HttpHandle Post(const std::string& url, std::vector<uint8_t> body, HttpResponseCallback cb)
    {
        return Send(HttpRequest(HttpVerb::Post, url).Body(std::move(body)), std::move(cb));
    }

    HttpHandle Put(const std::string& url, std::vector<uint8_t> body, HttpResponseCallback cb)
    {
        return Send(HttpRequest(HttpVerb::Put, url).Body(std::move(body)), std::move(cb));
    }

    HttpHandle Patch(const std::string& url, std::vector<uint8_t> body, HttpResponseCallback cb)
    {
        return Send(HttpRequest(HttpVerb::Patch, url).Body(std::move(body)), std::move(cb));
    }

    HttpHandle Delete(const std::string& url, HttpResponseCallback cb)
    {
        return Send(HttpRequest(HttpVerb::Delete, url), std::move(cb));
    }

    HttpHandle PostString(const std::string& url, const std::string& body, HttpResponseCallback cb)
    {
        return Send(HttpRequest(HttpVerb::Post, url).Body(body), std::move(cb));
    }

    HttpResponse SendSync(HttpRequest)
    {
        // Not implementable. Blocking the main thread would deadlock the event
        // loop the response has to arrive on. Callers must use Send().
        HttpResponse response;
        response.SetError(HttpError::Unavailable,
            "Synchronous HTTP is not available in the browser -- use Http::Send / Http.Get with a callback.");
        return response;
    }
}

#endif // POLYPHASE_PLATFORM_ADDON && PLATFORM_WEB
