// Stub header for liblogosdelivery_kernel - mirrors the subset of the
// generated waku_* surface from logos-delivery (1cf853e9) that
// delivery_module_plugin.cpp actually consumes, so unit tests compile without
// the real library. Keep in sync with the real header when bumping the
// logos-delivery flake input.
//
// The real kernel header declares the full unstable waku_* surface; only the
// functions used by the module are stubbed here on purpose, so an accidental
// new kernel dependency fails loudly at compile time.

#pragma once
#ifndef __liblogosdelivery_kernel__
#define __liblogosdelivery_kernel__

// Shared reply typedefs and RET_* return codes live in the stable header.
#include "liblogosdelivery.h"

#ifdef __cplusplus
extern "C"
{
#endif

  int logosdelivery_ctx_waku_store_query(const LogosDeliveryCtx* ctx,
                                         const char* jsonQuery,
                                         const char* peerAddr,
                                         int32_t timeoutMs,
                                         LogosDeliveryReplyFn on_reply,
                                         void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* __liblogosdelivery_kernel__ */
