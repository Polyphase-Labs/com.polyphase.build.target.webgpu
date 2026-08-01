/**
 * @file InputTypes_Platform.h
 * @brief Web (Emscripten) platform extension for the engine's `InputTypes.h` fork.
 *
 * Nothing platform-specific needs to leak into the engine's input layer:
 * Input_Web.cpp translates emscripten/html5.h events (keyboard, mouse, wheel,
 * touch, Gamepad API) into the engine's platform-agnostic state internally.
 */

#pragma once

#include <stdint.h>
