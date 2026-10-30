#include "pl/runtime/InputBridge.h"

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pl/Input.hpp"
#include "pl/runtime/CallbackSnapshot.hpp"
#include "pl/runtime/JavaRuntime.h"

namespace pl::runtime {
namespace {

// The four dispatchers run inside the game's own input path, so their callback lists are published
// as immutable snapshots read with one atomic load and iterated by const reference. Registration is
// rare, so the copy-and-swap it costs does not matter.
CallbackSnapshot<PreloaderInput_OnTouch_Fn> g_touchCallbacks;
CallbackSnapshot<PreloaderInput_OnKeyEvent_Fn> g_keyEventCallbacks;
CallbackSnapshot<PreloaderInput_OnTextInput_Fn> g_textInputCallbacks;
CallbackSnapshot<PreloaderInput_OnMouse_Fn> g_mouseCallbacks;
CallbackSnapshot<pl::input::TouchCallback> g_cppTouchCallbacks;
CallbackSnapshot<pl::input::KeyCallback> g_cppKeyCallbacks;
CallbackSnapshot<pl::input::TextInputCallback> g_cppTextInputCallbacks;
CallbackSnapshot<pl::input::MouseCallback> g_cppMouseCallbacks;

// The document callback is a single slot with take-once semantics, not a list, so it keeps a small
// mutex; it is not on the per-event path.
pl::input::DocumentCallback g_documentCallback;
std::mutex g_callbackMutex;

void RegisterLegacyTouchCallback(PreloaderInput_OnTouch_Fn callback) {
  g_touchCallbacks.add(callback);
}

void RegisterLegacyKeyEventCallback(PreloaderInput_OnKeyEvent_Fn callback) {
  g_keyEventCallbacks.add(callback);
}

void RegisterLegacyTextInputCallback(PreloaderInput_OnTextInput_Fn callback) {
  g_textInputCallbacks.add(callback);
}

void RegisterLegacyMouseCallback(PreloaderInput_OnMouse_Fn callback) {
  g_mouseCallbacks.add(callback);
}

void ShowKeyboardImpl() { CallActivityVoidMethod("showSoftKeyboard"); }

void HideKeyboardImpl() { CallActivityVoidMethod("hideSoftKeyboard"); }

// A callback runs inside the game's own input dispatch. An exception escaping one of them
// unwinds through game frames (and, on the JNI paths, across the boundary) and takes the
// process down; a mod that throws on one key press must not kill the session. Treat a throw
// as "not consumed" so the game still receives the input.
template <typename Fn>
bool InvokeSafely(Fn &&fn) {
  try {
    return fn();
  } catch (...) {
    return false;
  }
}

PreloaderInput_Interface g_inputInterface = {
    .RegisterTouchCallback = RegisterLegacyTouchCallback,
    .RegisterKeyEventCallback = RegisterLegacyKeyEventCallback,
    .ShowKeyboard = ShowKeyboardImpl,
    .HideKeyboard = HideKeyboardImpl,
    .RegisterMouseCallback = RegisterLegacyMouseCallback,
    .RegisterTextInputCallback = RegisterLegacyTextInputCallback,
};

} // namespace

PreloaderInput_Interface *GetInputInterface() { return &g_inputInterface; }

bool DispatchTouch(int action, int pointerId, float x, float y) {
  bool consumed = false;
  g_touchCallbacks.forEach([&](const PreloaderInput_OnTouch_Fn &callback) {
    if (callback) {
      consumed |= InvokeSafely([&] { return callback(action, pointerId, x, y); });
    }
  });
  const pl::input::TouchEvent event{
      .action = action,
      .pointerId = pointerId,
      .x = x,
      .y = y,
  };
  g_cppTouchCallbacks.forEach([&](const pl::input::TouchCallback &callback) {
    if (callback) {
      consumed |= InvokeSafely([&] { return callback(event); });
    }
  });
  return consumed;
}

bool DispatchKeyEvent(int keyCode, unsigned int unicodeChar, bool isKeyDown) {
  bool consumed = false;
  g_keyEventCallbacks.forEach([&](const PreloaderInput_OnKeyEvent_Fn &callback) {
    if (callback) {
      consumed |= InvokeSafely([&] { return callback(keyCode, unicodeChar, isKeyDown); });
    }
  });
  const pl::input::KeyEvent event{
      .keyCode = keyCode,
      .unicodeChar = unicodeChar,
      .isKeyDown = isKeyDown,
  };
  g_cppKeyCallbacks.forEach([&](const pl::input::KeyCallback &callback) {
    if (callback) {
      consumed |= InvokeSafely([&] { return callback(event); });
    }
  });
  return consumed;
}

bool DispatchTextInput(std::string_view text) {
  bool consumed = false;
  g_textInputCallbacks.forEach([&](const PreloaderInput_OnTextInput_Fn &callback) {
    if (callback) {
      consumed |= InvokeSafely([&] { return callback(text.data(), text.size()); });
    }
  });
  // The C++ callback API takes the event by value, so this is the one copy the text path needs;
  // the view itself is not copied on the way in.
  const pl::input::TextInputEvent event{.text = std::string(text)};
  g_cppTextInputCallbacks.forEach([&](const pl::input::TextInputCallback &callback) {
    if (callback) {
      consumed |= InvokeSafely([&] { return callback(event); });
    }
  });
  return consumed;
}

bool DispatchMouse(int button, bool isDown) {
  bool consumed = false;
  g_mouseCallbacks.forEach([&](const PreloaderInput_OnMouse_Fn &callback) {
    if (callback) {
      consumed |= InvokeSafely([&] { return callback(button, isDown); });
    }
  });
  const pl::input::MouseEvent event{.button = button, .isDown = isDown};
  g_cppMouseCallbacks.forEach([&](const pl::input::MouseCallback &callback) {
    if (callback) {
      consumed |= InvokeSafely([&] { return callback(event); });
    }
  });
  return consumed;
}

bool RequestDocument(std::string mimeType,
                     pl::input::DocumentCallback callback) {
  if (!callback) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    if (g_documentCallback) {
      return false;
    }
    g_documentCallback = std::move(callback);
  }
  if (mimeType.empty()) {
    mimeType = "*/*";
  }
  if (CallActivityStringMethod("openPreloaderDocumentPicker", mimeType)) {
    return true;
  }
  std::lock_guard<std::mutex> lock(g_callbackMutex);
  g_documentCallback = {};
  return false;
}

void DispatchDocumentResult(pl::input::DocumentResult result) {
  pl::input::DocumentCallback callback;
  {
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    callback = std::move(g_documentCallback);
    g_documentCallback = {};
  }
  if (callback) {
    try {
      callback(result);
    } catch (...) {
    }
  }
}

void ClearDocumentRequest() {
  std::lock_guard<std::mutex> lock(g_callbackMutex);
  g_documentCallback = {};
}

void RegisterCppTouchCallback(pl::input::TouchCallback callback) {
  g_cppTouchCallbacks.add(std::move(callback));
}

void RegisterCppKeyCallback(pl::input::KeyCallback callback) {
  g_cppKeyCallbacks.add(std::move(callback));
}

void RegisterCppTextInputCallback(pl::input::TextInputCallback callback) {
  g_cppTextInputCallbacks.add(std::move(callback));
}

void RegisterCppMouseCallback(pl::input::MouseCallback callback) {
  g_cppMouseCallbacks.add(std::move(callback));
}

void ShowKeyboard() { ShowKeyboardImpl(); }

void HideKeyboard() { HideKeyboardImpl(); }

} // namespace pl::runtime

namespace pl::input {

void registerTouchCallback(TouchCallback callback) {
  pl::runtime::RegisterCppTouchCallback(std::move(callback));
}

void registerKeyCallback(KeyCallback callback) {
  pl::runtime::RegisterCppKeyCallback(std::move(callback));
}

void registerTextInputCallback(TextInputCallback callback) {
  pl::runtime::RegisterCppTextInputCallback(std::move(callback));
}

void registerMouseCallback(MouseCallback callback) {
  pl::runtime::RegisterCppMouseCallback(std::move(callback));
}

void showKeyboard() { pl::runtime::ShowKeyboard(); }

void hideKeyboard() { pl::runtime::HideKeyboard(); }

bool openDocument(std::string mimeType, DocumentCallback callback) {
  return pl::runtime::RequestDocument(std::move(mimeType),
                                      std::move(callback));
}

} // namespace pl::input

extern "C" PL_EXPORT bool PLRequestDocument(
    const char *mimeType, PLDocumentResultCallback callback, void *userData) {
  if (!callback) {
    return false;
  }
  return pl::runtime::RequestDocument(
      mimeType ? mimeType : "*/*",
      [callback, userData](const pl::input::DocumentResult &result) {
        callback(result.success, result.path.c_str(), result.displayName.c_str(),
                 result.error.c_str(), userData);
      });
}
