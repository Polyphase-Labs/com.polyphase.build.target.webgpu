/**
 * @file SystemTypes_Platform.h
 * @brief Web (Emscripten) platform extension for the engine's `SystemTypes.h` fork.
 *
 * Picked up automatically when `POLYPHASE_PLATFORM_ADDON=1` is defined:
 * ActionManager generates `<projectDir>/Generated/PolyphasePlatform_SystemTypes.h`
 * which includes this file, and Makefile_Web puts Generated/ on the include
 * path.
 *
 * The build is SINGLE-THREADED wasm (no -pthread): SYS_CreateThread returns
 * nullptr and the engine's null-thread fallbacks (AssetManager's synchronous
 * drain) take over. The typedefs below are inert placeholders that keep the
 * engine's thread/mutex plumbing compiling; System_Web.cpp implements every
 * mutex op as a no-op.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

// Emscripten's musl-flavoured libc provides POSIX dirent/stat over MEMFS.
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>

// ----- Threading typedefs --------------------------------------------------
// No threads exist in a single-threaded wasm module. ThreadObject stays a
// pointer type so `SYS_CreateThread(...) == nullptr` is expressible and the
// engine's null-checks work; MutexObject is a dummy int (System_Web's
// SYS_CreateMutex hands out a heap token, lock/unlock are no-ops). ThreadFuncRet
// is void* — like the Linux/pthread arm — so THREAD_RETURN() resolves to
// `return 0;` and we do NOT define POLYPHASE_PLATFORM_ADDON_VOID_THREAD_RETURN.
typedef void* ThreadObject;
typedef int   MutexObject;
typedef void* ThreadFuncRet;

// ----- DirEntry injection --------------------------------------------------
// Directory iteration walks MEMFS via opendir/readdir. Same collect-then-
// process pattern as the console ports (drain the whole directory into a heap
// vector at SYS_OpenDirectory time, iterate the snapshot) — MEMFS has no
// interleaving hazard, but the snapshot keeps the implementation shared-shape
// with DC/PSP and immune to mutation during iteration. Kept as void* to avoid
// pulling <vector> into the engine's SystemTypes.h.
#define POLYPHASE_PLATFORM_ADDON_DIRENTRY_MEMBERS \
    void*        mDirDrain = nullptr; \
    unsigned int mDirIndex = 0;

// ----- SystemState injection -----------------------------------------------
// The "window" is the browser canvas. Track a quit flag (Quit() from script /
// page teardown) and a background flag (document visibility).
#define POLYPHASE_PLATFORM_ADDON_SYSTEMSTATE_MEMBERS \
    bool mQuitRequested = false; \
    bool mInBackground  = false;
