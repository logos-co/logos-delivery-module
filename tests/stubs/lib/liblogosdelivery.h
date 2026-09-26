// Stub header for liblogosdelivery - mirrors the subset of the Nim-generated
// C API from logos-delivery (1cf853e9, nim-ffi's CBOR ABI) that
// delivery_module_plugin.cpp consumes, so it compiles during unit tests without
// the real library. Keep in sync with the real, Nim-build-generated header when
// bumping the logos-delivery flake input.
//
// Upstream generates the logosdelivery_ctx_* calls as static inline wrappers
// that CBOR-encode their arguments (generated/logosdelivery.h + TinyCBOR). Here
// they are plain declarations the mock implements, so the tests need no CBOR.
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

// The possible returned values for the functions that return int
#define NIMFFI_RET_OK 0
#define NIMFFI_RET_ERR 1
#define NIMFFI_RET_MISSING_CALLBACK 2
#define RET_OK NIMFFI_RET_OK
#define RET_ERR NIMFFI_RET_ERR
#define RET_MISSING_CALLBACK NIMFFI_RET_MISSING_CALLBACK

typedef struct {
    void* ptr;
} LogosDeliveryCtx;

typedef void (*LogosDeliveryCreateFn)(int err_code, LogosDeliveryCtx* ctx,
                                      const char* err_msg, void* user_data);

// Every call replies with this shape; upstream emits one typedef per call,
// which this stub collapses into a single name. `reply` points at the result
// on success; `err_msg` carries a failure.
typedef void (*LogosDeliveryReplyFn)(int err_code, const char* const* reply,
                                     const char* err_msg, void* user_data);

#ifdef __cplusplus
extern "C"
{
#endif

  // Version and git commit hash. Needs no ctx; initializes the library.
  const char *logosdelivery_version(void);

  // Raw result-delivery callback used by the event API. `msg` is a byte run of
  // `len` bytes, not NUL-terminated, valid only for the duration of the call.
  typedef void (*FFICallBack)(int callerRet, const char *msg, size_t len, void *userData);

  // Creates a new node from the given configuration JSON and hands the
  // context to `on_created`. The configuration is a JSON object with these
  // optional keys:
  //   "mode": "Core" | "Edge"        (messaging role; defaults to "Core")
  //   "preset": "<network preset>"   (e.g. "twn")
  //   "messagingOverrides": { ... }  (per-field messaging config overrides)
  //   "channelsOverrides": { ... }   (per-field reliable-channel overrides)
  // Override keys accept the config field name or its CLI switch name (e.g.
  // "clusterId" or "cluster-id"). Unknown keys are rejected.
  // Example: {"mode":"Core","messagingOverrides":{"cluster-id":42,"log-level":"INFO"}}
  int logosdelivery_ctx_create(const char* configJson, LogosDeliveryCreateFn on_created,
                               void* user_data);

  // Destroys the node and frees the handle, tearing down the event listeners
  // registered against it.
  int logosdelivery_ctx_destroy(LogosDeliveryCtx* ctx);

  int logosdelivery_ctx_start_node(const LogosDeliveryCtx* ctx, LogosDeliveryReplyFn on_reply,
                                   void* user_data);
  int logosdelivery_ctx_stop_node(const LogosDeliveryCtx* ctx, LogosDeliveryReplyFn on_reply,
                                  void* user_data);

  // Subscribe to / unsubscribe from a content topic
  // (e.g. "/myapp/1/chat/proto").
  int logosdelivery_ctx_subscribe(const LogosDeliveryCtx* ctx, const char* contentTopicStr,
                                  LogosDeliveryReplyFn on_reply, void* user_data);
  int logosdelivery_ctx_unsubscribe(const LogosDeliveryCtx* ctx, const char* contentTopicStr,
                                    LogosDeliveryReplyFn on_reply, void* user_data);

  // Send a message ({ "contentTopic": ..., "payload": <base64>, "ephemeral":
  // <bool> }). Replies with a request ID that tracks its delivery.
  int logosdelivery_ctx_send(const LogosDeliveryCtx* ctx, const char* messageJson,
                             LogosDeliveryReplyFn on_reply, void* user_data);

  // --- Reliable Channels API (stable surface) ---

  // Create a reliable channel. Replies with the channel id. Zero cipher
  // callbacks and user data make an unencrypted channel.
  int logosdelivery_ctx_channel_create(const LogosDeliveryCtx* ctx, const char* channelIdStr,
                                       const char* contentTopicStr, const char* senderIdStr,
                                       uint64_t encryptFn, uint64_t decryptFn,
                                       uint64_t userData, LogosDeliveryReplyFn on_reply,
                                       void* user_data);

  // Replies "true" or "false"; an unknown channel id is not an error.
  int logosdelivery_ctx_channel_exists(const LogosDeliveryCtx* ctx, const char* channelIdStr,
                                       LogosDeliveryReplyFn on_reply, void* user_data);

  // Send a message ({ "payload": <base64>, "ephemeral": <bool> }) on a reliable
  // channel. Replies with a request ID.
  int logosdelivery_ctx_channel_send(const LogosDeliveryCtx* ctx, const char* channelIdStr,
                                     const char* messageJson, LogosDeliveryReplyFn on_reply,
                                     void* user_data);

  // Close a reliable channel: stops its SDS loops; persisted state survives, so
  // re-creating the channel restores it.
  int logosdelivery_ctx_channel_close(const LogosDeliveryCtx* ctx, const char* channelIdStr,
                                      LogosDeliveryReplyFn on_reply, void* user_data);

  // --- Events ---

  // Events are delivered through a per-event listener registry: one callback
  // per event name. Names: "onMessageSent", "onMessageError",
  // "onMessagePropagated", "onMessageReceived", "onConnectionStatusChange",
  // "onTopicHealthChange", "onConnectionChange", "onReceivedMessage",
  // "onChannelMessageReceived" (payload base64-encoded), "onChannelMessageSent"
  // and "onChannelMessageError".
  //
  // Takes the raw context (LogosDeliveryCtx::ptr). Returns a non-zero listener
  // id (0 on an invalid context). The callback runs on a dedicated event thread
  // and must be fast, non-blocking and thread-safe.
  uint64_t logosdelivery_add_event_listener(void *ctx, const char *eventName,
                                            FFICallBack callback, void *userData);

  // Removes a previously registered listener. Returns 0 on success, 1 if the
  // listener id was not found or the context is invalid.
  int logosdelivery_remove_event_listener(void *ctx, uint64_t listenerId);

  // --- Node info / config introspection ---

  int logosdelivery_ctx_get_available_node_info_ids(const LogosDeliveryCtx* ctx,
                                                    LogosDeliveryReplyFn on_reply,
                                                    void* user_data);
  int logosdelivery_ctx_get_node_info(const LogosDeliveryCtx* ctx, const char* nodeInfoId,
                                      LogosDeliveryReplyFn on_reply, void* user_data);
  int logosdelivery_ctx_get_connection_status(const LogosDeliveryCtx* ctx,
                                              LogosDeliveryReplyFn on_reply, void* user_data);
  int logosdelivery_ctx_get_available_configs(const LogosDeliveryCtx* ctx,
                                              LogosDeliveryReplyFn on_reply, void* user_data);

  // NOTE: the low-level kernel API (waku_*) lives in the separate, advanced
  // header liblogosdelivery_kernel.h. It is intentionally not declared here so
  // this header only promises the stable Messaging / Reliable Channels surface.

#ifdef __cplusplus
}
#endif

#endif /* __liblogosdelivery__ */
