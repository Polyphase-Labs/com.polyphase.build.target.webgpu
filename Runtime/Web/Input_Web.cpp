/**
 * @file Input_Web.cpp
 * @brief Web (Emscripten) input: keyboard, mouse, wheel, touch, Gamepad API,
 *        pointer lock — all via emscripten/html5.h callbacks.
 *
 * Key-code alignment: the engine's fallback KeyCode enum (the arm addon
 * platforms compile, InputTypes.h `#else`) is Windows-VK-based — and the
 * browser's legacy KeyboardEvent.keyCode is ALSO VK-based, so the DOM code
 * maps 1:1 into the engine's mKeys[256] for letters, digits, arrows, F-keys,
 * space/enter/shift/ctrl and the OEM punctuation range. No translation table
 * needed beyond a range clamp.
 *
 * Threading: the module is single-threaded; DOM callbacks fire between
 * WebFrame invocations (never mid-frame), so they write the engine's
 * InputState directly. Persisted state (keys/buttons/touches) is set/cleared
 * by events; per-frame deltas (wheel, mouse movement) accumulate in statics
 * that INP_Update() transfers and resets.
 *
 * Gamepads: Gamepad API via emscripten_sample_gamepad_data() each frame,
 * standard-mapping layout mapped onto the engine's GamepadState. Y axes are
 * browser-down-positive and get inverted (engine: +Y = up).
 *
 * Built only when POLYPHASE_PLATFORM_ADDON is defined.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Input/Input.h"
#include "Input/InputUtils.h"
#include "Engine.h"
#include "Log.h"
#include "Maths.h"

#include <emscripten.h>
#include <emscripten/html5.h>

#include <string.h>

namespace
{
    // Per-frame accumulators, transferred and reset in INP_Update.
    int32_t sWheelDelta   = 0;
    int32_t sMouseDeltaX  = 0;
    int32_t sMouseDeltaY  = 0;

    // Latest mouse position in canvas CSS pixels (scaled to backing store in
    // the mousemove handler).
    int32_t sMouseX = 0;
    int32_t sMouseY = 0;

    bool sPointerLocked = false;

    // ----- Shadow input state ---------------------------------------------
    // DOM callbacks fire BETWEEN frames. If they wrote the engine's current
    // state directly, InputAdvanceFrame's current->previous snapshot (start of
    // INP_Update) would already contain the new press, and Just-Down edge
    // detection would never fire (buttons unclickable, key taps lost). So
    // callbacks write these shadow arrays, and INP_Update applies them to the
    // engine's current state AFTER the snapshot — matching the ordering of
    // the Windows message pump.
    bool    sShadowKeys[INPUT_MAX_KEYS] = {};
    bool    sShadowMouse[MOUSE_BUTTON_COUNT] = {};
    bool    sShadowTouches[INPUT_MAX_TOUCHES] = {};
    int32_t sShadowTouchX[INPUT_MAX_TOUCHES] = {};
    int32_t sShadowTouchY[INPUT_MAX_TOUCHES] = {};
    bool    sPendingRepeatKeys[INPUT_MAX_KEYS] = {};

    // Scale a CSS-pixel canvas coordinate to the backing-store (device-pixel)
    // space the engine renders in.
    void CssToBacking(double cssX, double cssY, int32_t& outX, int32_t& outY)
    {
        double cssW = 0.0, cssH = 0.0;
        emscripten_get_element_css_size("#canvas", &cssW, &cssH);
        const uint32_t bw = GetEngineState()->mWindowWidth;
        const uint32_t bh = GetEngineState()->mWindowHeight;
        const double sx = (cssW > 0.0) ? (double)bw / cssW : 1.0;
        const double sy = (cssH > 0.0) ? (double)bh / cssH : 1.0;
        outX = (int32_t)(cssX * sx + 0.5);
        outY = (int32_t)(cssY * sy + 0.5);
    }

    // ----- Keyboard --------------------------------------------------------

    EM_BOOL OnKey(int eventType, const EmscriptenKeyboardEvent* e, void* /*userData*/)
    {
        if (e == nullptr) return EM_FALSE;

        const bool down = (eventType == EMSCRIPTEN_EVENT_KEYDOWN);
        const uint32_t code = e->keyCode; // legacy VK-style code — matches the engine enum

        if (code < INPUT_MAX_KEYS)
        {
            if (eventType == EMSCRIPTEN_EVENT_KEYDOWN || eventType == EMSCRIPTEN_EVENT_KEYUP)
            {
                sShadowKeys[code] = down;
                if (down && e->repeat)
                {
                    sPendingRepeatKeys[code] = true;
                }
            }
        }

        // Swallow keys the page would otherwise act on (scrolling, tab-focus,
        // quick-find) but let browser shortcuts (Ctrl/Meta combos, F5, F11,
        // F12) through so refresh/devtools/fullscreen keep working.
        if (e->ctrlKey || e->metaKey) return EM_FALSE;
        switch (code)
        {
            case 32:  // space
            case 9:   // tab
            case 37: case 38: case 39: case 40: // arrows
            case 33: case 34: // page up/down
            case 8:   // backspace (history-back!)
            case 191: // '/' (firefox quick find)
                return EM_TRUE;
            default:
                return (code >= 112 && code <= 123 && code != 116 && code != 122 && code != 123)
                           ? EM_TRUE   // F-keys except F5/F11/F12
                           : EM_FALSE;
        }
    }

    // ----- Mouse -----------------------------------------------------------

    EM_BOOL OnMouseButton(int eventType, const EmscriptenMouseEvent* e, void* /*userData*/)
    {
        if (e == nullptr) return EM_FALSE;

        const bool down = (eventType == EMSCRIPTEN_EVENT_MOUSEDOWN);

        // DOM button: 0=left 1=middle 2=right 3=back 4=forward.
        int32_t engineButton = -1;
        switch (e->button)
        {
            case 0: engineButton = MOUSE_LEFT;   break;
            case 1: engineButton = MOUSE_MIDDLE; break;
            case 2: engineButton = MOUSE_RIGHT;  break;
            case 3: engineButton = MOUSE_X1;     break;
            case 4: engineButton = MOUSE_X2;     break;
            default: break;
        }
        if (engineButton >= 0 && engineButton < MOUSE_BUTTON_COUNT)
        {
            sShadowMouse[engineButton] = down;
        }

        if (down)
        {
            // mouseup registers on the window (so releases outside the canvas
            // aren't lost); its targetX/Y are window-relative — don't let them
            // corrupt the cursor position.
            CssToBacking(e->targetX, e->targetY, sMouseX, sMouseY);
        }
        return EM_TRUE;
    }

    EM_BOOL OnMouseMove(int /*eventType*/, const EmscriptenMouseEvent* e, void* /*userData*/)
    {
        if (e == nullptr) return EM_FALSE;

        if (sPointerLocked)
        {
            // movementX/Y are raw deltas while locked.
            sMouseDeltaX += (int32_t)e->movementX;
            sMouseDeltaY += (int32_t)e->movementY;
        }
        else
        {
            int32_t x = 0, y = 0;
            CssToBacking(e->targetX, e->targetY, x, y);
            sMouseDeltaX += x - sMouseX;
            sMouseDeltaY += y - sMouseY;
            sMouseX = x;
            sMouseY = y;
        }
        return EM_FALSE;
    }

    EM_BOOL OnWheel(int /*eventType*/, const EmscriptenWheelEvent* e, void* /*userData*/)
    {
        if (e == nullptr) return EM_FALSE;
        // Normalise to +/-1 notches (deltaY > 0 = scroll down; engine
        // convention is positive = up, matching Windows WM_MOUSEWHEEL).
        if (e->deltaY > 0.0)      sWheelDelta -= 1;
        else if (e->deltaY < 0.0) sWheelDelta += 1;
        return EM_TRUE; // prevent page zoom/scroll
    }

    EM_BOOL OnPointerLockChange(int /*eventType*/, const EmscriptenPointerlockChangeEvent* e,
                                void* /*userData*/)
    {
        sPointerLocked = (e != nullptr && e->isActive);
        GetEngineState()->mInput.mCursorLocked = sPointerLocked;
        return EM_TRUE;
    }

    // ----- Touch -----------------------------------------------------------

    EM_BOOL OnTouch(int eventType, const EmscriptenTouchEvent* e, void* /*userData*/)
    {
        if (e == nullptr) return EM_FALSE;

        // Rebuild the whole touch set from the event's active-touch list;
        // identifiers are sticky per finger but slot order is what the engine
        // consumes, so pack actives into slots 0..N. Written to the shadow
        // state (see above) so touch-tap edges survive the frame snapshot.
        bool slotUsed[INPUT_MAX_TOUCHES] = {};
        int32_t slot = 0;
        for (int i = 0; i < e->numTouches && slot < INPUT_MAX_TOUCHES; ++i)
        {
            const EmscriptenTouchPoint& tp = e->touches[i];
            const bool active =
                !(eventType == EMSCRIPTEN_EVENT_TOUCHEND ||
                  eventType == EMSCRIPTEN_EVENT_TOUCHCANCEL) || !tp.isChanged;
            if (!active) continue;

            int32_t x = 0, y = 0;
            CssToBacking(tp.targetX, tp.targetY, x, y);
            sShadowTouches[slot] = true;
            sShadowTouchX[slot] = x;
            sShadowTouchY[slot] = y;
            slotUsed[slot] = true;
            ++slot;
        }
        for (int32_t i = 0; i < INPUT_MAX_TOUCHES; ++i)
        {
            if (!slotUsed[i]) sShadowTouches[i] = false;
        }

        // Mirror the primary touch into the mouse-pointer slot so pointer-
        // driven UI (buttons) works on touch screens, matching Android.
        if (slot > 0)
        {
            sMouseX = sShadowTouchX[0];
            sMouseY = sShadowTouchY[0];
            sShadowMouse[MOUSE_LEFT] = true;
        }
        else if (eventType == EMSCRIPTEN_EVENT_TOUCHEND ||
                 eventType == EMSCRIPTEN_EVENT_TOUCHCANCEL)
        {
            sShadowMouse[MOUSE_LEFT] = false;
        }

        return EM_TRUE; // prevent synthetic mouse events / page scroll
    }

    // ----- Gamepads --------------------------------------------------------

    // Standard-mapping button indices (https://w3c.github.io/gamepad/#remapping).
    enum StdButton
    {
        STD_A = 0, STD_B = 1, STD_X = 2, STD_Y = 3,
        STD_L1 = 4, STD_R1 = 5, STD_L2 = 6, STD_R2 = 7,
        STD_SELECT = 8, STD_START = 9,
        STD_THUMBL = 10, STD_THUMBR = 11,
        STD_UP = 12, STD_DOWN = 13, STD_LEFT = 14, STD_RIGHT = 15,
        STD_HOME = 16
    };

    constexpr float kAnalogDeadzone = 0.25f;

    float ApplyDeadzone(float v)
    {
        if (v >  kAnalogDeadzone) return (v - kAnalogDeadzone) / (1.0f - kAnalogDeadzone);
        if (v < -kAnalogDeadzone) return (v + kAnalogDeadzone) / (1.0f - kAnalogDeadzone);
        return 0.0f;
    }

    // Browsers only expose a gamepad after a button is pressed on it while
    // the page has focus (fingerprinting protection). These callbacks make
    // that moment visible in the console and confirm the mapping mode.
    EM_BOOL OnGamepadConnected(int /*eventType*/, const EmscriptenGamepadEvent* e, void* /*userData*/)
    {
        if (e != nullptr)
        {
            LogDebug("[Input_Web] gamepad connected: index=%d \"%s\" mapping=\"%s\" "
                     "(%d buttons, %d axes)%s",
                     (int)e->index, e->id, e->mapping,
                     (int)e->numButtons, (int)e->numAxes,
                     (e->mapping[0] != 's') ? "  [WARNING: non-standard mapping — buttons may be scrambled]" : "");
        }
        return EM_TRUE;
    }

    EM_BOOL OnGamepadDisconnected(int /*eventType*/, const EmscriptenGamepadEvent* e, void* /*userData*/)
    {
        if (e != nullptr)
        {
            LogDebug("[Input_Web] gamepad disconnected: index=%d", (int)e->index);
        }
        return EM_TRUE;
    }

    // Browser pad index promoted to engine gamepad slot 0. Multiple browser
    // pads can represent ONE physical controller (e.g. DSX exposes a virtual
    // Xbox 360 pad while the real DualSense also enumerates — and whichever
    // one the remapper captures exclusively reads all-zeros). Scripts and
    // Button widgets read slot 0, so slot 0 must follow ACTIVITY, not
    // enumeration order: any pad showing a button press / stick deflection
    // becomes primary.
    int sPrimaryPad = -1;

    bool PadHasActivity(const EmscriptenGamepadEvent& pad)
    {
        for (int b = 0; b < pad.numButtons; ++b)
        {
            if (pad.digitalButton[b]) return true;
        }
        for (int a = 0; a < pad.numAxes && a < 4; ++a)
        {
            if (pad.axis[a] > 0.35 || pad.axis[a] < -0.35) return true;
        }
        return false;
    }

    void UpdateGamepads()
    {
        InputState& input = GetEngineState()->mInput;

        if (emscripten_sample_gamepad_data() != EMSCRIPTEN_RESULT_SUCCESS)
        {
            return; // Gamepad API unavailable (or no gamepad event seen yet)
        }

        const int numPads = emscripten_get_num_gamepads();

        // Pass 1: pick/refresh the primary pad by activity.
        for (int padIdx = 0; padIdx < numPads; ++padIdx)
        {
            EmscriptenGamepadEvent pad = {};
            if (emscripten_get_gamepad_status(padIdx, &pad) != EMSCRIPTEN_RESULT_SUCCESS ||
                !pad.connected)
            {
                if (padIdx == sPrimaryPad) sPrimaryPad = -1;   // primary unplugged
                continue;
            }
            if (padIdx != sPrimaryPad && PadHasActivity(pad))
            {
                sPrimaryPad = padIdx;
            }
        }

        // Pass 2: fill engine slots — primary first, the rest in index order.
        int32_t connected = 0;
        for (int pass = 0; pass < 2 && connected < INPUT_MAX_GAMEPADS; ++pass)
        for (int padIdx = 0; padIdx < numPads && connected < INPUT_MAX_GAMEPADS; ++padIdx)
        {
            const bool isPrimary = (padIdx == sPrimaryPad);
            if ((pass == 0) != isPrimary) continue;   // pass 0: primary only; pass 1: the rest

            EmscriptenGamepadEvent pad = {};
            if (emscripten_get_gamepad_status(padIdx, &pad) != EMSCRIPTEN_RESULT_SUCCESS ||
                !pad.connected)
            {
                continue;
            }

            GamepadState& gp = input.mGamepads[connected];
            gp.mDevice = padIdx;
            gp.mType = GamepadType::Standard;
            gp.mConnected = true;

            auto btn = [&](int stdIdx) -> int32_t {
                return (stdIdx < pad.numButtons && pad.digitalButton[stdIdx]) ? 1 : 0;
            };
            auto analogBtn = [&](int stdIdx) -> float {
                return (stdIdx < pad.numButtons) ? (float)pad.analogButton[stdIdx] : 0.0f;
            };

            gp.mButtons[GAMEPAD_A] = btn(STD_A);
            gp.mButtons[GAMEPAD_B] = btn(STD_B);
            gp.mButtons[GAMEPAD_X] = btn(STD_X);
            gp.mButtons[GAMEPAD_Y] = btn(STD_Y);

            gp.mButtons[GAMEPAD_L1] = btn(STD_L1);
            gp.mButtons[GAMEPAD_R1] = btn(STD_R1);
            gp.mButtons[GAMEPAD_L2] = btn(STD_L2);
            gp.mButtons[GAMEPAD_R2] = btn(STD_R2);

            gp.mButtons[GAMEPAD_SELECT] = btn(STD_SELECT);
            gp.mButtons[GAMEPAD_START]  = btn(STD_START);
            gp.mButtons[GAMEPAD_THUMBL] = btn(STD_THUMBL);
            gp.mButtons[GAMEPAD_THUMBR] = btn(STD_THUMBR);

            gp.mButtons[GAMEPAD_UP]    = btn(STD_UP);
            gp.mButtons[GAMEPAD_DOWN]  = btn(STD_DOWN);
            gp.mButtons[GAMEPAD_LEFT]  = btn(STD_LEFT);
            gp.mButtons[GAMEPAD_RIGHT] = btn(STD_RIGHT);
            gp.mButtons[GAMEPAD_HOME]  = btn(STD_HOME);

            // Axes: 0=LX 1=LY 2=RX 3=RY, browser Y is down-positive — invert
            // for the engine's +Y = up convention.
            const float lx = (pad.numAxes > 0) ? (float)pad.axis[0] : 0.0f;
            const float ly = (pad.numAxes > 1) ? (float)pad.axis[1] : 0.0f;
            const float rx = (pad.numAxes > 2) ? (float)pad.axis[2] : 0.0f;
            const float ry = (pad.numAxes > 3) ? (float)pad.axis[3] : 0.0f;

            gp.mAxes[GAMEPAD_AXIS_LTHUMB_X] = glm::clamp(ApplyDeadzone( lx), -1.0f, 1.0f);
            gp.mAxes[GAMEPAD_AXIS_LTHUMB_Y] = glm::clamp(ApplyDeadzone(-ly), -1.0f, 1.0f);
            gp.mAxes[GAMEPAD_AXIS_RTHUMB_X] = glm::clamp(ApplyDeadzone( rx), -1.0f, 1.0f);
            gp.mAxes[GAMEPAD_AXIS_RTHUMB_Y] = glm::clamp(ApplyDeadzone(-ry), -1.0f, 1.0f);

            gp.mAxes[GAMEPAD_AXIS_LTRIGGER] = analogBtn(STD_L2);
            gp.mAxes[GAMEPAD_AXIS_RTRIGGER] = analogBtn(STD_R2);

            ++connected;
        }

        // Disconnect the remainder so scripts never read stale pads.
        for (int32_t i = connected; i < INPUT_MAX_GAMEPADS; ++i)
        {
            input.mGamepads[i].mConnected = false;
        }
        input.mNumControllers = connected;
    }
}

// =========================================================================
// INP_* platform surface
// =========================================================================

void INP_Initialize()
{
    InputInit();

    // Keyboard on the window (the canvas doesn't take focus reliably);
    // pointer + touch on the canvas.
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, OnKey);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, OnKey);

    emscripten_set_mousedown_callback("#canvas", nullptr, EM_TRUE, OnMouseButton);
    emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, OnMouseButton);
    emscripten_set_mousemove_callback("#canvas", nullptr, EM_TRUE, OnMouseMove);
    emscripten_set_wheel_callback("#canvas", nullptr, EM_TRUE, OnWheel);
    emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr,
                                              EM_TRUE, OnPointerLockChange);

    emscripten_set_touchstart_callback("#canvas", nullptr, EM_TRUE, OnTouch);
    emscripten_set_touchend_callback("#canvas", nullptr, EM_TRUE, OnTouch);
    emscripten_set_touchmove_callback("#canvas", nullptr, EM_TRUE, OnTouch);
    emscripten_set_touchcancel_callback("#canvas", nullptr, EM_TRUE, OnTouch);

    emscripten_set_gamepadconnected_callback(nullptr, EM_TRUE, OnGamepadConnected);
    emscripten_set_gamepaddisconnected_callback(nullptr, EM_TRUE, OnGamepadDisconnected);

    LogDebug("Input_Web: DOM + Gamepad API input initialised "
             "(gamepads appear after the first button press on them)");
}

void INP_Shutdown()
{
    InputShutdown();
}

void INP_Update()
{
    // Snapshot current -> previous FIRST, then apply the shadow state the DOM
    // callbacks accumulated since last frame. This ordering is what makes
    // Just-Down / Just-Up edge detection work (see the shadow-state comment).
    InputAdvanceFrame();

    InputState& input = GetEngineState()->mInput;

    memcpy(input.mKeys, sShadowKeys, sizeof(input.mKeys));
    memcpy(input.mMouseButtons, sShadowMouse, sizeof(input.mMouseButtons));
    memcpy(input.mTouches, sShadowTouches, sizeof(input.mTouches));

    // Mirror the left mouse button into pointer/touch slot 0, exactly like
    // the engine's INP_SetMouseButton helper does for the desktop backends —
    // Button widgets are driven by IsPointerDown(0), which reads mTouches[0],
    // NOT the mouse-button array. Real touches (shadow slot 0) take priority.
    if (!sShadowTouches[0])
    {
        input.mTouches[0] = sShadowMouse[MOUSE_LEFT];
    }

    for (int32_t i = 0; i < INPUT_MAX_KEYS; ++i)
    {
        if (sPendingRepeatKeys[i])
        {
            input.mRepeatKeys[i] = true;
            sPendingRepeatKeys[i] = false;
        }
    }

    for (int32_t i = 0; i < INPUT_MAX_TOUCHES; ++i)
    {
        if (sShadowTouches[i])
        {
            input.mPointerX[i] = sShadowTouchX[i];
            input.mPointerY[i] = sShadowTouchY[i];
        }
    }

    // Mouse pointer (slot 0 doubles as the mouse position, like the other
    // desktop-ish platforms). Touch positions above win when a touch is live.
    if (!input.mTouches[0])
    {
        input.mPointerX[0] = sMouseX;
        input.mPointerY[0] = sMouseY;
    }

    input.mScrollWheelDelta = sWheelDelta;
    input.mMouseDeltaX = sMouseDeltaX;
    input.mMouseDeltaY = sMouseDeltaY;
    sWheelDelta  = 0;
    sMouseDeltaX = 0;
    sMouseDeltaY = 0;

    UpdateGamepads();

    InputPostUpdate();
}

void INP_SetCursorPos(int32_t /*x*/, int32_t /*y*/)
{
    // Browsers cannot warp the cursor.
}

void INP_ShowCursor(bool show)
{
    GetEngineState()->mInput.mCursorShown = show;
    EM_ASM({
        var c = document.getElementById('canvas');
        if (c) c.style.cursor = $0 ? 'default' : 'none';
    }, show ? 1 : 0);
}

void INP_LockCursor(bool lock)
{
    if (lock)
    {
        // Deferred-until-gesture: the browser only grants pointer lock inside
        // a user-gesture callstack; deferUntilInEventHandler covers the
        // engine calling this from Tick.
        emscripten_request_pointerlock("#canvas", EM_TRUE);
    }
    else
    {
        emscripten_exit_pointerlock();
    }
}

void INP_TrapCursor(bool trap)
{
    // No cursor-confine API in browsers; pointer lock is the nearest thing.
    INP_LockCursor(trap);
}

void INP_TrapCursorToRect(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/) {}

const char* INP_ShowSoftKeyboard(bool /*show*/) { return nullptr; }
bool INP_IsSoftKeyboardShown() { return false; }

#endif // POLYPHASE_PLATFORM_ADDON
