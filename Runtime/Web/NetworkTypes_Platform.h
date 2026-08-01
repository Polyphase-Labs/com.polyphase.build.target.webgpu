/**
 * @file NetworkTypes_Platform.h
 * @brief Web (Emscripten) platform extension for the engine's `NetworkTypes.h` fork.
 *
 * Stub. Browsers expose no raw UDP/TCP sockets, so Network_Web.cpp is a
 * fail-stub set (sanctioned by Network.h for socket-less platforms).
 * SocketHandle maps to int32 like the other Unix-like platforms. A future
 * transport could layer WebSockets / WebRTC DataChannels here.
 */

#pragma once
#include <stdint.h>

typedef int32_t SocketHandle;
