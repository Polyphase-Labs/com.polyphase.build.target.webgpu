/**
 * @file AudioTypes_Platform.h
 * @brief Web (Emscripten) platform extension for the engine's `AudioTypes.h` fork.
 *
 * Stub — the engine's `AudioTypes.h` only forks on Windows today (XAudio2).
 * Web audio runs through emscripten's built-in OpenAL (over Web Audio);
 * Audio_Web.cpp keeps every AL type internal.
 */

#pragma once
#include <stdint.h>
