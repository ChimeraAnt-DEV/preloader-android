#pragma once

#include <jni.h>
#include <string>
#include <string_view>
#include "pl/Platform.hpp"
#include "pl/runtime/JniAttach.hpp"

namespace pl::runtime {

void SetJavaVm(JavaVM *vm);
JavaVM *GetJavaVm();

void SetActivity(JNIEnv *env, jobject activity);
void ClearActivity(JNIEnv *env);
void CallActivityVoidMethod(const char *methodName);
bool CallActivityStringMethod(const char *methodName, const std::string &value);
pl::platform::HttpResponse HttpGetImpl(std::string_view url, int timeoutMs);

/**
 * @brief A scoped JVM attachment: acquires a `JNIEnv` for the current thread and releases it on
 *        destruction.
 *
 * <p>Use this in every native function that calls back into Java. It attaches only when the thread
 * was detached, so a call made on the UI thread does not detach the UI thread, and it detaches in
 * its destructor — so an early return or an exception in the middle of the call can no longer leak
 * the attachment and the local references it held. {@code PushLocalFrame} bounds the local
 * reference table for the duration, which is what stopped a long-running receive loop from
 * exhausting it and silently disabling the module.
 *
 * <p>{@link #valid()} is false when no `JavaVM` was captured or the thread could not be attached;
 * callers must check it and fail closed rather than using an unattached env.
 */
class ScopedJniEnv {
public:
    ScopedJniEnv();
    ~ScopedJniEnv();

    ScopedJniEnv(const ScopedJniEnv &) = delete;
    ScopedJniEnv &operator=(const ScopedJniEnv &) = delete;

    /** The env, or nullptr when the thread could not be attached. */
    JNIEnv *env() const { return env_; }

    /** True when the env is usable. */
    bool valid() const { return env_ != nullptr; }

    /**
     * @brief Pops the local frame if one was pushed, and detaches only if this scope attached.
     *        Safe to call more than once; also runs from the destructor.
     */
    void release();

private:
    JNIEnv *env_ = nullptr;
    bool ownsAttachment_ = false;
    bool framePushed_ = false;
    bool released_ = false;
};

} // namespace pl::runtime
