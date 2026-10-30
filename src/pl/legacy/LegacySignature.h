#pragma once

/**
 * @file LegacySignature.h
 * @brief The C signature entry point (`pl_resolve_signature`).
 *
 * <p><b>Kept for binary compatibility, not for new code.</b> A new mod should use
 * {@link pl::memory::resolveSignature} (C++, in `pl/memory/Signature.hpp`), which also offers the
 * batch form `resolveSignatures`; the C entry point forwards to the same implementation.
 *
 * <p>Nothing in this repository calls it.
 */

#include <stdint.h>

#include "pl/legacy/LegacyMacro.h"

#ifdef __cplusplus
extern "C" {
#endif

PL_LEGACY_EXPORT uintptr_t pl_resolve_signature(const char *signature,
                                                const char *moduleName);

#ifdef __cplusplus
} // extern "C"
#endif
