// Stub header for liblogosdelivery - mirrors the subset of the Nim-generated
// C ABI from logos-delivery (master 69fbffa3) that delivery_module_plugin.cpp
// consumes, so it compiles during unit tests without the real library. Keep in
// sync with the real, Nim-build-generated header when bumping the
// logos-delivery flake input.
//
// Upstream splits this across library/liblogosdelivery.h (event ABI) and the
// generated generated/logosdelivery.h (call surface plus the logosdelivery_ctx_*
// convenience wrapper); both are folded into this one file.
//
// Note on the event names below: they are the wire names of upstream's
// per-event listener registry. The JSON "eventType" values actually delivered
// to the event callback are the snake_case ones - "channel_message_received",
// "channel_message_sent", "channel_message_error" and friends (see node_api.nim).

#pragma once
#ifndef __liblogosdelivery__
#define __liblogosdelivery__

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The possible returned values for the functions that return int
#define NIMFFI_RET_OK 0
#define NIMFFI_RET_ERR 1
#define NIMFFI_RET_MISSING_CALLBACK 2
// Non-terminal: the request is still running. Fires every ~5s and is always
// followed by a terminal RET_OK/RET_ERR.
#define NIMFFI_RET_STALE_WARN 3
#define RET_OK NIMFFI_RET_OK
#define RET_ERR NIMFFI_RET_ERR
#define RET_MISSING_CALLBACK NIMFFI_RET_MISSING_CALLBACK
#define RET_STALE_WARN NIMFFI_RET_STALE_WARN

// `abi = c` wire structs. Strings are borrowed, NUL-terminated `const char*`
// valid only for the duration of the call they cross.
typedef struct {
    const char* configJson;
} LogosdeliveryCreateNodeCtorReq;
typedef struct {
    const char* contentTopicStr;
} LogosdeliverySubscribeReq;
typedef struct {
    const char* contentTopicStr;
} LogosdeliveryUnsubscribeReq;
typedef struct {
    // { "contentTopic": ..., "payload": <base64>, "ephemeral": <bool> }
    const char* messageJson;
} LogosdeliverySendReq;
typedef struct {
    const char* nodeInfoId;
} LogosdeliveryGetNodeInfoReq;
typedef struct {
    const char* channelIdStr;
    const char* contentTopicStr;
    const char* senderIdStr;
} LogosdeliveryChannelCreateReq;
typedef struct {
    const char* channelIdStr;
} LogosdeliveryChannelExistsReq;
typedef struct {
    const char* channelIdStr;
    // { "payload": <base64>, "ephemeral": <bool> }
    const char* messageJson;
} LogosdeliveryChannelSendReq;
typedef struct {
    const char* channelIdStr;
} LogosdeliveryChannelCloseReq;

// Argument-taking exports all share this reply shape; upstream emits one
// typedef per call, which this stub collapses into a single name.
typedef void (*LogosDeliveryReplyFn)(int err_code,
                                     const char* reply,
                                     const char* err_msg,
                                     void* user_data);

// Raw reply of a scalar-fast-path export: `msg`/`len` are bytes, not
// NUL-terminated, and valid only for the duration of the call.
typedef void (*LogosDeliveryScalarRawFn)(int caller_ret, char* msg, size_t len, void* user_data);

// Raw reply of the constructor: `ctx_addr` is the context address as decimal text.
typedef void (*LogosDeliveryCreateRawFn)(int err_code,
                                         const char* ctx_addr,
                                         const char* err_msg,
                                         void* user_data);

// NUL-terminated copy of a length-delimited byte run; NULL if it can't.
static inline char* nimffi_abi_dup_cstr_n(const char* s, size_t n) {
    if (n == SIZE_MAX) return NULL;
    char* p = (char*)malloc(n + 1);
    if (p) {
        if (n > 0) memcpy(p, s, n);
        p[n] = '\0';
    }
    return p;
}

#ifdef __cplusplus
extern "C"
{
#endif

  // Raw result-delivery callback used by the event API. `msg` is a byte run of
  // `len` bytes, not NUL-terminated, valid only for the duration of the call.
  typedef void (*FFICallBack)(int callerRet, const char *msg, size_t len, void *userData);

  // Creates a new node from the given configuration JSON and reports the
  // context address through `on_created`. The configuration is a JSON object
  // with these optional keys:
  //   "mode": "Core" | "Edge"        (messaging role; defaults to "Core")
  //   "preset": "<network preset>"   (e.g. "twn")
  //   "messagingOverrides": { ... }  (per-field messaging config overrides)
  //   "channelsOverrides": { ... }   (per-field reliable-channel overrides)
  // Override keys accept the config field name or its CLI switch name (e.g.
  // "clusterId" or "cluster-id"). Unknown keys are rejected.
  // Example: {"mode":"Core","messagingOverrides":{"cluster-id":42,"log-level":"INFO"}}
  void *logosdelivery_create_node(const LogosdeliveryCreateNodeCtorReq *req,
                                  LogosDeliveryCreateRawFn on_created,
                                  void *user_data);

  // Starts the node.
  int logosdelivery_start_node(void *ctx, LogosDeliveryScalarRawFn callback, void *user_data);

  // Stops the node.
  int logosdelivery_stop_node(void *ctx, LogosDeliveryScalarRawFn callback, void *user_data);

  // Destroys a node created with logosdelivery_create_node, tearing down the
  // event listeners registered against it.
  int logosdelivery_destroy(void *ctx);

  // Subscribe to / unsubscribe from a content topic
  // (e.g. "/myapp/1/chat/proto").
  int logosdelivery_subscribe(void *ctx, LogosDeliveryReplyFn on_reply, void *user_data,
                              const LogosdeliverySubscribeReq *req);
  int logosdelivery_unsubscribe(void *ctx, LogosDeliveryReplyFn on_reply, void *user_data,
                                const LogosdeliveryUnsubscribeReq *req);

  // Send a message. Replies with a request ID that tracks its delivery.
  int logosdelivery_send(void *ctx, LogosDeliveryReplyFn on_reply, void *user_data,
                         const LogosdeliverySendReq *req);

  // --- Reliable Channels API (stable surface) ---

  // Create a reliable channel. Replies with the channel id.
  int logosdelivery_channel_create(void *ctx, LogosDeliveryReplyFn on_reply, void *user_data,
                                   const LogosdeliveryChannelCreateReq *req);

  // Replies "true" or "false"; an unknown channel id is not an error.
  int logosdelivery_channel_exists(void *ctx, LogosDeliveryReplyFn on_reply, void *user_data,
                                   const LogosdeliveryChannelExistsReq *req);

  // Send a message on a reliable channel. Replies with a request ID.
  int logosdelivery_channel_send(void *ctx, LogosDeliveryReplyFn on_reply, void *user_data,
                                 const LogosdeliveryChannelSendReq *req);

  // Close a reliable channel: stops its SDS loops; persisted state survives, so
  // re-creating the channel restores it.
  int logosdelivery_channel_close(void *ctx, LogosDeliveryReplyFn on_reply, void *user_data,
                                  const LogosdeliveryChannelCloseReq *req);

  // --- Events ---

  // Events are delivered through a per-event listener registry: one callback
  // per event name. Names: "onMessageSent", "onMessageError",
  // "onMessagePropagated", "onMessageReceived", "onConnectionStatusChange",
  // "onTopicHealthChange", "onConnectionChange", "onReceivedMessage",
  // "onChannelMessageReceived" (payload base64-encoded), "onChannelMessageSent"
  // and "onChannelMessageError".
  //
  // Returns a non-zero listener id (0 on an invalid context). The callback runs
  // on a dedicated event thread and must be fast, non-blocking and thread-safe.
  uint64_t logosdelivery_add_event_listener(void *ctx, const char *eventName,
                                            FFICallBack callback, void *userData);

  // Removes a previously registered listener. Returns 0 on success, 1 if the
  // listener id was not found or the context is invalid.
  int logosdelivery_remove_event_listener(void *ctx, uint64_t listenerId);

  // --- Node info / config introspection ---

  int logosdelivery_get_available_node_info_ids(void *ctx, LogosDeliveryScalarRawFn callback,
                                                void *user_data);
  int logosdelivery_get_node_info(void *ctx, LogosDeliveryReplyFn on_reply, void *user_data,
                                  const LogosdeliveryGetNodeInfoReq *req);
  int logosdelivery_get_available_configs(void *ctx, LogosDeliveryScalarRawFn callback,
                                          void *user_data);

  // NOTE: the low-level kernel API (waku_*) lives in the separate, advanced
  // header liblogosdelivery_kernel.h. It is intentionally not declared here so
  // this header only promises the stable Messaging / Reliable Channels surface.

#ifdef __cplusplus
}
#endif

// --- High-level context wrapper ---
//
// Mirrors the inline wrapper upstream generates alongside the raw ABI. Only the
// constructor and destructor are reproduced: those are the two the module uses,
// because they are what turns the decimal context address into a handle.

typedef struct {
    void* ptr;
} LogosDeliveryCtx;

typedef void (*LogosDeliveryCreateFn)(int err_code, LogosDeliveryCtx* ctx,
                                      const char* err_msg, void* user_data);
typedef struct { LogosDeliveryCreateFn fn; void* user_data; } LogosDeliveryCreateBox;

static inline void logosdelivery_create_trampoline(int ret, const char* ctx_addr,
                                                   const char* err_msg, void* ud) {
    LogosDeliveryCreateBox* box = (LogosDeliveryCreateBox*)ud;
    if (!box) return;
    if (ret == NIMFFI_RET_STALE_WARN) return;
    if (!box->fn) { free(box); return; }
    if (ret != 0) {
        box->fn(ret, NULL, err_msg ? err_msg : "FFI create failed", box->user_data);
        free(box);
        return;
    }
    char* endp = NULL;
    unsigned long long a = ctx_addr ? strtoull(ctx_addr, &endp, 10) : 0;
    int ok = ctx_addr && *ctx_addr && endp && *endp == '\0';
    if (!ok) {
        box->fn(-1, NULL, "FFI create returned non-numeric address", box->user_data);
        free(box);
        return;
    }
    LogosDeliveryCtx* ctx = (LogosDeliveryCtx*)calloc(1, sizeof(LogosDeliveryCtx));
    if (!ctx) {
        box->fn(-1, NULL, "out of memory", box->user_data);
        free(box);
        return;
    }
    ctx->ptr = (void*)(uintptr_t)a;
    box->fn(NIMFFI_RET_OK, ctx, NULL, box->user_data);
    free(box);
}

static inline int logosdelivery_ctx_create(const char* configJson,
                                           LogosDeliveryCreateFn on_created, void* user_data) {
    LogosdeliveryCreateNodeCtorReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.configJson = configJson;
    LogosDeliveryCreateBox* box = (LogosDeliveryCreateBox*)malloc(sizeof(LogosDeliveryCreateBox));
    if (!box) {
        if (on_created) on_created(-1, NULL, "out of memory", user_data);
        return -1;
    }
    box->fn = on_created;
    box->user_data = user_data;
    (void)logosdelivery_create_node(&ffi_req, logosdelivery_create_trampoline, box);
    return 0;
}

static inline int logosdelivery_ctx_destroy(LogosDeliveryCtx* ctx) {
    if (!ctx) return NIMFFI_RET_OK;
    int rc = NIMFFI_RET_OK;
    if (ctx->ptr) { rc = logosdelivery_destroy(ctx->ptr); ctx->ptr = NULL; }
    free(ctx);
    return rc;
}

#endif /* __liblogosdelivery__ */
