#pragma once

/**
 * @file LegacyInput.h
 * @brief The C input interface (`GetPreloaderInput`).
 *
 * <p><b>Still in use.</b> The preloader's own {@link GetInputInterface} builds a
 * `PreloaderInput_Interface`, and the C accessor is exported for mods built against it. The
 * callbacks it carries are the same ones {@link pl::runtime} dispatches to; see
 * `pl/runtime/InputBridge.h` for the C++ side.
 */

#include <stdbool.h>
#include <stddef.h>

#include "pl/legacy/LegacyMacro.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*PreloaderInput_OnTouch_Fn)(int action, int pointerId, float x,
                                          float y);
typedef bool (*PreloaderInput_OnKeyEvent_Fn)(int keyCode,
                                             unsigned int unicodeChar,
                                             bool isKeyDown);
typedef bool (*PreloaderInput_OnTextInput_Fn)(const char *text, size_t length);
typedef bool (*PreloaderInput_OnMouse_Fn)(int button, bool isDown);

typedef struct PreloaderInput_Interface {
  void (*RegisterTouchCallback)(PreloaderInput_OnTouch_Fn callback);
  void (*RegisterKeyEventCallback)(PreloaderInput_OnKeyEvent_Fn callback);
  void (*ShowKeyboard)(void);
  void (*HideKeyboard)(void);
  void (*RegisterMouseCallback)(PreloaderInput_OnMouse_Fn callback);
  void (*RegisterTextInputCallback)(PreloaderInput_OnTextInput_Fn callback);
} PreloaderInput_Interface;

PL_LEGACY_EXPORT PreloaderInput_Interface *GetPreloaderInput(void);

#ifdef __cplusplus
} // extern "C"
#endif
