#include "pl/runtime/JavaRuntime.h"
#include "pl/runtime/SafeJni.hpp"
#include "pl/Platform.hpp"
#include <algorithm>
#include <string_view>

namespace pl::runtime {
namespace {

JavaVM *g_vm = nullptr;
jobject g_activity = nullptr;

} // namespace

void SetJavaVm(JavaVM *vm) { g_vm = vm; }

JavaVM *GetJavaVm() { return g_vm; }

ScopedJniEnv::ScopedJniEnv() {
  if (!g_vm) return;
  JNIEnv *env = nullptr;
  const jint status = g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_4);
  const pl::jni::AttachAction action = pl::jni::DecideAttach(static_cast<int>(status), true);
  if (!pl::jni::CanUseEnv(action)) return;
  if (action == pl::jni::AttachAction::AttachAndDetach) {
#if defined(__ANDROID__)
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
#else
    if (g_vm->AttachCurrentThread(reinterpret_cast<void **>(&env), nullptr) != JNI_OK) return;
#endif
    ownsAttachment_ = true;
  }
  env_ = env;
  // Bound the local reference table for the duration. A long-running caller (a receive loop) that
  // never pushed a frame would otherwise accumulate references and eventually fail every call.
  if (env_ != nullptr && env_->PushLocalFrame(64) == 0) {
    framePushed_ = true;
  }
}

void ScopedJniEnv::release() {
  if (released_) return;
  released_ = true;
  if (env_ != nullptr && framePushed_) {
    env_->PopLocalFrame(nullptr);
  }
  if (ownsAttachment_ && g_vm != nullptr) {
    g_vm->DetachCurrentThread();
  }
}

ScopedJniEnv::~ScopedJniEnv() { release(); }

void SetActivity(JNIEnv *env, jobject activity) {
  ClearActivity(env);
  if (activity) {
    g_activity = env->NewGlobalRef(activity);
  }
}

void ClearActivity(JNIEnv *env) {
  if (g_activity) {
    env->DeleteGlobalRef(g_activity);
    g_activity = nullptr;
  }
}

void CallActivityVoidMethod(const char *methodName) {
  if (!g_activity) return;

  ScopedJniEnv scope;
  if (!scope.valid()) return;
  JNIEnv *env = scope.env();

  jclass cls = env->GetObjectClass(g_activity);
  if (cls) {
    jmethodID mid = env->GetMethodID(cls, methodName, "()V");
    if (mid) {
      env->CallVoidMethod(g_activity, mid);
    }
  }
  // The scope's destructor clears the local frame and detaches if it attached; an exception is
  // cleared before the frame pop so a pending throwable cannot make PopLocalFrame abort.
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
}

bool SetClipboardTextImpl(std::string_view text) {
  if (!g_activity) return false;

  ScopedJniEnv scope;
  if (!scope.valid()) return false;
  JNIEnv *env = scope.env();

  bool success = false;
  jclass activityClass = env->GetObjectClass(g_activity);
  jstring serviceName = pl::jni::NewString(env, "clipboard");
  jobject manager = nullptr;
  if (activityClass && serviceName) {
    jmethodID getSystemService = env->GetMethodID(
        activityClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    if (getSystemService) manager = env->CallObjectMethod(g_activity, getSystemService, serviceName);
  }

  jclass clipDataClass = env->FindClass("android/content/ClipData");
  jclass clipboardClass = env->FindClass("android/content/ClipboardManager");
  jstring label = pl::jni::NewString(env, "");
  jstring content = pl::jni::NewString(env, text);
  jobject clip = nullptr;
  if (manager && clipDataClass && label && content) {
    jmethodID newPlainText = env->GetStaticMethodID(
        clipDataClass, "newPlainText",
        "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
    if (newPlainText) clip = env->CallStaticObjectMethod(clipDataClass, newPlainText, label, content);
  }
  if (manager && clipboardClass && clip) {
    jmethodID setPrimaryClip = env->GetMethodID(
        clipboardClass, "setPrimaryClip", "(Landroid/content/ClipData;)V");
    if (setPrimaryClip) {
      env->CallVoidMethod(manager, setPrimaryClip, clip);
      success = !env->ExceptionCheck();
    }
  }

  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    success = false;
  }
  // No per-reference DeleteLocalRef calls: the scope's frame pop releases them all at once, which is
  // both cheaper and correct even on the early-return paths above.
  return success;
}

bool CallActivityStringMethod(const char *methodName, const std::string &value) {
  if (!g_activity) return false;

  ScopedJniEnv scope;
  if (!scope.valid()) return false;
  JNIEnv *env = scope.env();

  bool called = false;
  jclass cls = env->GetObjectClass(g_activity);
  if (cls) {
    jmethodID mid = env->GetMethodID(cls, methodName, "(Ljava/lang/String;)V");
    if (mid) {
      jstring argument = pl::jni::NewString(env, value);
      if (argument) {
        env->CallVoidMethod(g_activity, mid, argument);
        called = !env->ExceptionCheck();
      }
    }
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    called = false;
  }
  return called;
}

pl::platform::HttpResponse HttpGetImpl(std::string_view url, int timeoutMs) {
  pl::platform::HttpResponse result;
  if (url.empty()) return result;
  if (!url.starts_with("https://") && !url.starts_with("http://")) return result;

  // The scoped attachment replaces the hand-rolled GetEnv/Attach/Detach here: it attaches only when
  // the thread was detached and detaches on every exit path (including the early returns inside the
  // lambda below), and its local frame bounds the reference table for the whole request.
  ScopedJniEnv scope;
  if (!scope.valid()) return result;
  JNIEnv *env = scope.env();

  jobject connection = nullptr;
  jobject input = nullptr;
  jmethodID close = nullptr;
  jmethodID disconnect = nullptr;
  auto findClass = [&](const char *name) -> jclass {
    return env->ExceptionCheck() ? nullptr : env->FindClass(name);
  };
  auto method = [&](jclass cls, const char *name, const char *signature) -> jmethodID {
    return !cls || env->ExceptionCheck() ? nullptr : env->GetMethodID(cls, name, signature);
  };
  auto string = [&](const char *value) -> jstring {
    return env->ExceptionCheck() ? nullptr : pl::jni::NewString(env, value);
  };

  const bool success = [&]() -> bool {
    jclass urlClass = findClass("java/net/URL");
    jclass connectionClass = findClass("java/net/HttpURLConnection");
    jclass inputClass = findClass("java/io/InputStream");
    jclass outputClass = findClass("java/io/ByteArrayOutputStream");
    if (!urlClass || !connectionClass || !inputClass || !outputClass) return false;

    close = method(inputClass, "close", "()V");
    disconnect = method(connectionClass, "disconnect", "()V");
    jmethodID urlCtor = method(urlClass, "<init>", "(Ljava/lang/String;)V");
    jmethodID openConnection = method(urlClass, "openConnection", "()Ljava/net/URLConnection;");
    jmethodID setConnectTimeout = method(connectionClass, "setConnectTimeout", "(I)V");
    jmethodID setReadTimeout = method(connectionClass, "setReadTimeout", "(I)V");
    jmethodID setRequestMethod = method(connectionClass, "setRequestMethod", "(Ljava/lang/String;)V");
    jmethodID setRequestProperty = method(connectionClass, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
    jmethodID getResponseCode = method(connectionClass, "getResponseCode", "()I");
    jmethodID getHeaderField = method(connectionClass, "getHeaderField", "(Ljava/lang/String;)Ljava/lang/String;");
    jmethodID getInputStream = method(connectionClass, "getInputStream", "()Ljava/io/InputStream;");
    jmethodID getErrorStream = method(connectionClass, "getErrorStream", "()Ljava/io/InputStream;");
    jmethodID outputCtor = method(outputClass, "<init>", "()V");
    jmethodID read = method(inputClass, "read", "([B)I");
    jmethodID write = method(outputClass, "write", "([BII)V");
    jmethodID toByteArray = method(outputClass, "toByteArray", "()[B");
    if (!close || !disconnect || !urlCtor || !openConnection || !setConnectTimeout ||
        !setReadTimeout || !setRequestMethod || !setRequestProperty || !getResponseCode ||
        !getHeaderField || !getInputStream || !getErrorStream || !outputCtor ||
        !read || !write || !toByteArray || env->ExceptionCheck()) return false;

    std::string urlString(url);
    jstring jUrl = string(urlString.c_str());
    jstring get = string("GET");
    jstring acceptKey = string("Accept");
    jstring acceptValue = string("application/json");
    jstring agentKey = string("User-Agent");
    jstring agentValue = string("ChimeraPreloader/1");
    jstring retryKey = string("Retry-After");
    if (!jUrl || !get || !acceptKey || !acceptValue || !agentKey || !agentValue || !retryKey) return false;

    jobject urlObject = env->NewObject(urlClass, urlCtor, jUrl);
    if (env->ExceptionCheck() || !urlObject) return false;
    connection = env->CallObjectMethod(urlObject, openConnection);
    if (env->ExceptionCheck() || !connection) return false;
    const int safeTimeout = std::clamp(timeoutMs <= 0 ? 5000 : timeoutMs, 1000, 30000);
    env->CallVoidMethod(connection, setConnectTimeout, safeTimeout);
    if (env->ExceptionCheck()) return false;
    env->CallVoidMethod(connection, setReadTimeout, safeTimeout);
    if (env->ExceptionCheck()) return false;
    env->CallVoidMethod(connection, setRequestMethod, get);
    if (env->ExceptionCheck()) return false;
    env->CallVoidMethod(connection, setRequestProperty, acceptKey, acceptValue);
    if (env->ExceptionCheck()) return false;
    env->CallVoidMethod(connection, setRequestProperty, agentKey, agentValue);
    if (env->ExceptionCheck()) return false;

    result.status = env->CallIntMethod(connection, getResponseCode);
    if (env->ExceptionCheck()) return false;
    jstring retry = static_cast<jstring>(env->CallObjectMethod(connection, getHeaderField, retryKey));
    if (env->ExceptionCheck()) return false;
    if (retry) {
      const char *chars = env->GetStringUTFChars(retry, nullptr);
      if (env->ExceptionCheck() || !chars) return false;
      result.retryAfter = chars;
      env->ReleaseStringUTFChars(retry, chars);
    }

    input = env->CallObjectMethod(connection, result.status >= 400 ? getErrorStream : getInputStream);
    if (env->ExceptionCheck()) return false;
    if (!input) return true; // HTTP errors can legitimately have no response body.
    jobject output = env->NewObject(outputClass, outputCtor);
    if (env->ExceptionCheck() || !output) return false;
    jbyteArray buffer = env->NewByteArray(8192);
    if (env->ExceptionCheck() || !buffer) return false;

    constexpr std::size_t maxResponseBytes = 8 * 1024 * 1024;
    std::size_t total = 0;
    for (;;) {
      jint count = env->CallIntMethod(input, read, buffer);
      if (env->ExceptionCheck()) return false;
      if (count <= 0) break;
      total += static_cast<std::size_t>(count);
      if (total > maxResponseBytes) return false;
      env->CallVoidMethod(output, write, buffer, 0, count);
      if (env->ExceptionCheck()) return false;
    }
    jbyteArray bytes = static_cast<jbyteArray>(env->CallObjectMethod(output, toByteArray));
    if (env->ExceptionCheck() || !bytes) return false;
    jsize length = env->GetArrayLength(bytes);
    if (length > 0) {
      jbyte *data = env->GetByteArrayElements(bytes, nullptr);
      if (env->ExceptionCheck() || !data) return false;
      result.body.assign(reinterpret_cast<const char *>(data), static_cast<std::size_t>(length));
      env->ReleaseByteArrayElements(bytes, data, JNI_ABORT);
    }
    return true;
  }();

  if (env->ExceptionCheck()) env->ExceptionClear();
  if (!success) result = {};
  if (input && close) {
    env->CallVoidMethod(input, close);
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  if (connection && disconnect) {
    env->CallVoidMethod(connection, disconnect);
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  // The scope pops the local frame and detaches if it attached; nothing to do here.
  return result;
}

} // namespace pl::runtime

namespace pl::platform {

bool setClipboardText(std::string_view text) {
  return pl::runtime::SetClipboardTextImpl(text);
}

HttpResponse httpGet(std::string_view url, int timeoutMs) {
  return pl::runtime::HttpGetImpl(url, timeoutMs);
}

}
