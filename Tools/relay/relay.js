#!/usr/bin/env node
/**
 * Polyphase WebGL2 network relay.
 *
 * Browsers have no UDP sockets, so the engine's NET_* layer (Runtime/Web/
 * Network_Web.cpp) tunnels datagrams over a WebSocket to this process. Each
 * WebSocket connection owns one real UDP socket here; the relay forwards
 * datagrams in both directions, carrying the peer address in a frame header.
 *
 * Frame format (binary, big-endian), matching the POLY_NET_OP_* constants in
 * Network_Web.cpp:
 *
 *   client -> relay
 *     [1][u16 port]                        BIND      bind the UDP socket
 *     [2][u32 ip][u16 port][payload]       DATA      sendto
 *     [3][u8 enable]                       BROADCAST setsockopt SO_BROADCAST
 *
 *   relay -> client
 *     [2][u32 ip][u16 port][payload]       DATA      recvfrom
 *     [4][u32 localIp][u16 localPort]      BOUND     our address, post-bind
 *
 * No npm dependencies -- the WebSocket server is implemented inline against
 * node's http/crypto modules. Payloads are engine datagrams (OCT_MAX_MSG_SIZE,
 * ~500 bytes), so the framing here handles small unfragmented frames plus the
 * 16/64-bit extended lengths for completeness.
 *
 * SECURITY: this is a UDP proxy. Anything that can reach it can send UDP to
 * whatever the allowlist permits, sourced from this machine. It therefore binds
 * to 127.0.0.1 and permits only private/loopback destinations by default.
 * Widen deliberately, never by habit, and never expose it to the internet
 * without putting authentication in front of it.
 *
 * Usage:
 *   node relay.js [--host 127.0.0.1] [--port 5150] [--allow CIDR]... [--allow-any] [-v]
 */

'use strict';

const http   = require('node:http');
const crypto = require('node:crypto');
const dgram  = require('node:dgram');

// ---------------------------------------------------------------------------
// Args
// ---------------------------------------------------------------------------

const args = process.argv.slice(2);
let host = '127.0.0.1';
let port = 5150;
let allowAny = false;
let verbose = false;
const extraAllow = [];

for (let i = 0; i < args.length; ++i) {
    const a = args[i];
    if (a === '--host') host = args[++i];
    else if (a === '--port') port = parseInt(args[++i], 10);
    else if (a === '--allow') extraAllow.push(args[++i]);
    else if (a === '--allow-any') allowAny = true;
    else if (a === '-v' || a === '--verbose') verbose = true;
    else if (a === '-h' || a === '--help') {
        console.log('usage: node relay.js [--host H] [--port P] [--allow CIDR]... [--allow-any] [-v]');
        process.exit(0);
    } else {
        console.error('unknown argument: ' + a);
        process.exit(1);
    }
}

const DEFAULT_ALLOW = [
    '127.0.0.0/8',
    '10.0.0.0/8',
    '172.16.0.0/12',
    '192.168.0.0/16',
    '169.254.0.0/16',
    '255.255.255.255/32',
];

function parseCidr(s) {
    const [addr, bitsStr] = s.split('/');
    const bits = bitsStr === undefined ? 32 : parseInt(bitsStr, 10);
    const parts = addr.split('.').map(Number);
    if (parts.length !== 4 || parts.some(p => !(p >= 0 && p <= 255)) || !(bits >= 0 && bits <= 32)) {
        console.error('bad CIDR: ' + s);
        process.exit(1);
    }
    const base = ((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]) >>> 0;
    const mask = bits === 0 ? 0 : (0xFFFFFFFF << (32 - bits)) >>> 0;
    return { base: (base & mask) >>> 0, mask };
}

const allowList = DEFAULT_ALLOW.concat(extraAllow).map(parseCidr);

function isAllowed(ip) {
    if (allowAny) return true;
    return allowList.some(r => ((ip & r.mask) >>> 0) === r.base);
}

// ---------------------------------------------------------------------------
// Address helpers. The engine uses host-order uint32 with the first octet in
// the MSB: 1.2.3.4 === 0x01020304.
// ---------------------------------------------------------------------------

function ipToUint(s) {
    const p = s.split('.').map(Number);
    if (p.length !== 4 || p.some(x => !(x >= 0 && x <= 255))) return 0;
    return ((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]) >>> 0;
}

function uintToIp(v) {
    return [(v >>> 24) & 0xFF, (v >>> 16) & 0xFF, (v >>> 8) & 0xFF, v & 0xFF].join('.');
}

function localIpv4() {
    const nets = require('node:os').networkInterfaces();
    for (const name of Object.keys(nets)) {
        for (const ni of nets[name]) {
            if (ni.family === 'IPv4' && !ni.internal) return ipToUint(ni.address);
        }
    }
    return ipToUint('127.0.0.1');
}

const LOCAL_IP = localIpv4();

// ---------------------------------------------------------------------------
// Minimal WebSocket server (RFC 6455, binary frames only)
// ---------------------------------------------------------------------------

// RFC 6455 handshake GUID. Verified against the spec's test vector:
// "dGhlIHNhbXBsZSBub25jZQ==" -> "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

function wsAccept(key) {
    return crypto.createHash('sha1').update(key + WS_GUID).digest('base64');
}

function wsEncode(payload) {
    const len = payload.length;
    let header;

    if (len < 126) {
        header = Buffer.alloc(2);
        header[1] = len;
    } else if (len < 65536) {
        header = Buffer.alloc(4);
        header[1] = 126;
        header.writeUInt16BE(len, 2);
    } else {
        header = Buffer.alloc(10);
        header[1] = 127;
        header.writeBigUInt64BE(BigInt(len), 2);
    }

    header[0] = 0x82;   // FIN | binary
    return Buffer.concat([header, payload]);
}

// Pulls complete frames out of an accumulating buffer. Returns the number of
// bytes consumed; emits via the supplied callbacks.
function wsDecode(buf, onBinary, onClose, onPing) {
    let off = 0;

    for (;;) {
        if (buf.length - off < 2) break;

        const b0 = buf[off];
        const b1 = buf[off + 1];
        const opcode = b0 & 0x0F;
        const masked = (b1 & 0x80) !== 0;
        let len = b1 & 0x7F;
        let p = off + 2;

        if (len === 126) {
            if (buf.length - p < 2) break;
            len = buf.readUInt16BE(p);
            p += 2;
        } else if (len === 127) {
            if (buf.length - p < 8) break;
            const big = buf.readBigUInt64BE(p);
            if (big > 0x7FFFFFFFn) return -1;   // absurd for our traffic
            len = Number(big);
            p += 8;
        }

        let maskKey = null;
        if (masked) {
            if (buf.length - p < 4) break;
            maskKey = buf.subarray(p, p + 4);
            p += 4;
        }

        if (buf.length - p < len) break;

        const payload = Buffer.from(buf.subarray(p, p + len));
        if (maskKey) {
            for (let i = 0; i < payload.length; ++i) payload[i] ^= maskKey[i & 3];
        }
        p += len;
        off = p;

        if (opcode === 0x8) { onClose(); return -1; }
        else if (opcode === 0x9) { onPing(payload); }
        else if (opcode === 0x2 || opcode === 0x1 || opcode === 0x0) { onBinary(payload); }
        // 0xA (pong) ignored.
    }

    return off;
}

// ---------------------------------------------------------------------------
// Connection handling
// ---------------------------------------------------------------------------

let connSeq = 0;

function handleConnection(sock) {
    const id = ++connSeq;
    let udp = null;
    let broadcast = false;
    let closed = false;
    let acc = Buffer.alloc(0);

    const log = (...m) => { if (verbose) console.log(`[conn ${id}]`, ...m); };

    function send(payload) {
        if (!closed && sock.writable) sock.write(wsEncode(payload));
    }

    function sendBound() {
        if (!udp) return;
        let addr;
        try { addr = udp.address(); } catch (e) { return; }

        const f = Buffer.alloc(7);
        f[0] = 4;
        f.writeUInt32BE(LOCAL_IP, 1);
        f.writeUInt16BE(addr.port, 5);
        send(f);
        log('bound to udp port', addr.port);
    }

    function ensureUdp(bindPort, cb) {
        if (udp) { if (cb) cb(); return; }

        // reuseAddr matters for the discovery socket: several clients (and a
        // native host on the same machine) all bind OCT_BROADCAST_PORT to hear
        // the same session broadcasts.
        udp = dgram.createSocket({ type: 'udp4', reuseAddr: true });

        udp.on('error', (err) => {
            console.error(`[conn ${id}] udp error: ${err.message}`);
            try { udp.close(); } catch (e) {}
            udp = null;
        });

        udp.on('message', (msg, rinfo) => {
            const f = Buffer.alloc(7 + msg.length);
            f[0] = 2;
            f.writeUInt32BE(ipToUint(rinfo.address), 1);
            f.writeUInt16BE(rinfo.port, 5);
            msg.copy(f, 7);
            send(f);
        });

        udp.bind(bindPort, () => {
            if (broadcast) { try { udp.setBroadcast(true); } catch (e) {} }
            sendBound();
            if (cb) cb();
        });
    }

    function onFrame(payload) {
        if (payload.length < 1) return;
        const op = payload[0];

        if (op === 1) {                                  // BIND
            if (payload.length < 3) return;
            const p = payload.readUInt16BE(1);
            log('bind', p);
            ensureUdp(p);
        } else if (op === 3) {                           // BROADCAST
            broadcast = payload.length >= 2 && payload[1] !== 0;
            if (udp) { try { udp.setBroadcast(broadcast); } catch (e) {} }
        } else if (op === 2) {                           // DATA
            if (payload.length < 7) return;
            const ip = payload.readUInt32BE(1);
            const port = payload.readUInt16BE(5);
            const body = payload.subarray(7);

            if (!isAllowed(ip)) {
                console.warn(`[conn ${id}] blocked send to ${uintToIp(ip)} (not in allowlist; --allow to permit)`);
                return;
            }

            // An unbound socket is the client-side case: bind ephemeral, then
            // send. NetworkManager::Connect creates a socket and immediately
            // sends without binding.
            const doSend = () => {
                if (!udp) return;
                const dest = uintToIp(ip);
                if (dest === '255.255.255.255' && !broadcast) {
                    try { udp.setBroadcast(true); broadcast = true; } catch (e) {}
                }
                udp.send(body, port, dest, (err) => {
                    if (err) log('send failed:', err.message);
                });
            };

            if (!udp) ensureUdp(0, doSend); else doSend();
        }
    }

    function cleanup() {
        if (closed) return;
        closed = true;
        if (udp) { try { udp.close(); } catch (e) {} udp = null; }
        try { sock.destroy(); } catch (e) {}
        log('closed');
    }

    sock.on('data', (chunk) => {
        acc = acc.length ? Buffer.concat([acc, chunk]) : chunk;

        const consumed = wsDecode(
            acc,
            onFrame,
            cleanup,
            (p) => {                                    // ping -> pong
                const h = Buffer.from([0x8A, p.length]);
                if (!closed && sock.writable) sock.write(Buffer.concat([h, p]));
            }
        );

        if (consumed < 0) { cleanup(); return; }
        acc = consumed > 0 ? acc.subarray(consumed) : acc;
    });

    sock.on('close', cleanup);
    sock.on('error', cleanup);

    log('open');
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

const server = http.createServer((req, res) => {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('Polyphase WebGL2 network relay. Connect a WebSocket to this address.\n');
});

server.on('upgrade', (req, sock, head) => {
    const key = req.headers['sec-websocket-key'];
    if (req.headers.upgrade !== 'websocket' || !key) {
        sock.destroy();
        return;
    }

    sock.write(
        'HTTP/1.1 101 Switching Protocols\r\n' +
        'Upgrade: websocket\r\n' +
        'Connection: Upgrade\r\n' +
        'Sec-WebSocket-Accept: ' + wsAccept(key) + '\r\n\r\n'
    );

    sock.setNoDelay(true);
    handleConnection(sock);

    if (head && head.length) sock.unshift(head);
});

server.listen(port, host, () => {
    console.log(`Polyphase relay listening on ws://${host}:${port}`);
    console.log(`Local address reported to clients: ${uintToIp(LOCAL_IP)}`);
    console.log(allowAny
        ? 'Destination allowlist: DISABLED (--allow-any). This is an open UDP proxy.'
        : 'Destination allowlist: ' + DEFAULT_ALLOW.concat(extraAllow).join(', '));
    console.log('');
    console.log('In shell.html set:  window.POLYPHASE_RELAY_URL = "ws://' + host + ':' + port + '";');
});
