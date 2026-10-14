#pragma once

/**
 * @file JniAttach.hpp
 * @brief The rule for using a `JNIEnv` from a thread that may not be attached to the JVM.
 *
 * <p>Every native entry point that calls back into Java can be reached from a thread that is not
 * attached: the render thread, the game's own tick thread, a cosmetic-sync receive loop, a mod's
 * worker. Calling a JNI function with an unattached thread is undefined behaviour, and the previous
 * hand-rolled version of this logic in each function attached, called, then detached only on the
 * happy path — an exception raised inside the call skipped the detach and leaked the attachment and
 * every local reference it held. Repeated calls then exhausted the local reference table and the
 * module silently stopped working.
 *
 * <p>The decision itself is pure (this header, no JNI types), so the invariants are host-tested:
 * a thread we attached is a thread we must detach; a thread that was already attached must not be
 * detached by us; a thread that cannot be attached must be reported as a failure rather than used
 * unattached.
 */

namespace pl::jni {

/** What to do with the current thread's JVM attachment. */
enum class AttachAction {
    /** The thread is already attached: use the env, do not detach it. */
    UseExisting,
    /** The thread was detached and we attached it: use the env, and detach it when done. */
    AttachAndDetach,
    /** The thread could not be attached (or the VM is gone): do not use the env. */
    Unavailable,
};

/**
 * @brief Maps a `JavaVM::GetEnv` result to the action to take.
 *
 * @param getEnvStatus the value `GetEnv` returned (`JNI_OK`, `JNI_EDETACHED`, `JNI_EVERSION`, …)
 * @param haveVm true when a `JavaVM*` was captured at load time
 * @return the action to take
 */
constexpr AttachAction DecideAttach(int getEnvStatus, bool haveVm) {
    // JNI_OK == 0, JNI_EDETACHED == -2, JNI_EVERSION == -3 (the JNI spec's fixed values).
    if (!haveVm) return AttachAction::Unavailable;
    if (getEnvStatus == 0) return AttachAction::UseExisting;
    if (getEnvStatus == -2) return AttachAction::AttachAndDetach;
    return AttachAction::Unavailable;
}

/** True when the action means the caller owns the attachment and must detach it. */
constexpr bool OwnsAttachment(AttachAction action) {
    return action == AttachAction::AttachAndDetach;
}

/** True when the env may be used at all. */
constexpr bool CanUseEnv(AttachAction action) {
    return action != AttachAction::Unavailable;
}

} // namespace pl::jni
