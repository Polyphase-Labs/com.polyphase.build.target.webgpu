# Polyphase WebGL2 network relay

Browsers cannot open UDP sockets, and the Polyphase multiplayer protocol is UDP.
This relay bridges the two: the WebGL2 build tunnels every datagram over a
WebSocket to this process, which owns a real UDP socket per connection and
forwards traffic both ways.

HTTP does **not** need the relay — `Http.Get` and friends use `fetch()` directly
(`Runtime/Web/Http_Web.cpp`). This is only for `Network.*` multiplayer.

## Running it

No dependencies. Node 16+.

```sh
npm start
# or
node relay.js
```

Then uncomment the relay URL in `Runtime/Web/shell.html` and rebuild:

```js
window.POLYPHASE_RELAY_URL = "ws://localhost:5150";
```

With that line absent, `NET_IsActive()` returns false and networking is simply
off — the same behaviour as before the relay existed. Nothing else changes.

## Options

| Flag | Default | Meaning |
| --- | --- | --- |
| `--host H` | `127.0.0.1` | Interface for the WebSocket listener |
| `--port P` | `5150` | Port for the WebSocket listener |
| `--allow CIDR` | — | Add a permitted UDP destination range (repeatable) |
| `--allow-any` | off | Disable the destination allowlist entirely |
| `-v` | off | Log per-connection activity |

## Security

**This is a UDP proxy.** Anything that can reach it can emit UDP packets from
this machine to any address the allowlist permits. That is why it binds to
loopback and restricts destinations to private ranges by default:

```
127.0.0.0/8  10.0.0.0/8  172.16.0.0/12  192.168.0.0/16  169.254.0.0/16  255.255.255.255/32
```

Widen it deliberately. Do not expose it to the internet without authentication
in front of it, and treat `--allow-any` as a debugging tool rather than a
deployment setting.

## Serving the game over HTTPS

A page loaded over `https://` cannot open a plaintext `ws://` socket — browsers
block mixed content. Terminate TLS in front of the relay (nginx, Caddy) and
point `POLYPHASE_RELAY_URL` at `wss://…`.

## How discovery works

`Network.BeginSessionSearch()` binds a socket to `OCT_BROADCAST_PORT` (15151).
The relay binds its UDP socket to the same port with `SO_REUSEADDR`, so session
broadcasts from native hosts on the LAN arrive with their true source address
and `NetworkManager::UpdateSearch` parses them unmodified. Sending to
`255.255.255.255` from the browser turns into a real LAN broadcast.

`SO_REUSEADDR` broadcast fan-out to multiple sockets on one port is reliable on
Linux and generally works on Windows; if two browser clients on the same machine
search simultaneously and one sees no sessions, that is the thing to suspect.

## Protocol

Binary WebSocket frames, big-endian. Opcodes match `POLY_NET_OP_*` in
`Runtime/Web/Network_Web.cpp`.

| Direction | Frame | Meaning |
| --- | --- | --- |
| client → relay | `[1][u16 port]` | bind the UDP socket |
| client → relay | `[2][u32 ip][u16 port][payload]` | `sendto` |
| client → relay | `[3][u8 enable]` | `SO_BROADCAST` |
| relay → client | `[2][u32 ip][u16 port][payload]` | `recvfrom` |
| relay → client | `[4][u32 localIp][u16 localPort]` | our address, after bind |

IPs are host-order `uint32` with the first octet in the MSB
(`1.2.3.4 == 0x01020304`), matching `NET_IpStringToUint32` on the native
backends.
