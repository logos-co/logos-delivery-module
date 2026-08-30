#ifndef NIM_FFI_LIB_LOGOSDELIVERY_C_ABI_H_INCLUDED
#define NIM_FFI_LIB_LOGOSDELIVERY_C_ABI_H_INCLUDED
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define NIMFFI_RET_OK 0
#define NIMFFI_RET_ERR 1
#define NIMFFI_RET_MISSING_CALLBACK 2
/* Non-terminal: the request is still running. Fires every ~5s with `msg`
   carrying the elapsed milliseconds as decimal text; always followed by a
   terminal RET_OK/RET_ERR. Ignore it unless you want progress. */
#define NIMFFI_RET_STALE_WARN 3
#ifndef RET_OK
#define RET_OK NIMFFI_RET_OK
#endif
#ifndef RET_ERR
#define RET_ERR NIMFFI_RET_ERR
#endif
#ifndef RET_MISSING_CALLBACK
#define RET_MISSING_CALLBACK NIMFFI_RET_MISSING_CALLBACK
#endif
#ifndef RET_STALE_WARN
#define RET_STALE_WARN NIMFFI_RET_STALE_WARN
#endif

/* `abi = c` wire structs — the C ABI. Strings are borrowed, NUL-terminated
   `const char*` valid only for the duration of the call they cross. */
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
    const char* messageJson;
} LogosdeliverySendReq;
typedef struct {
    const char* nodeInfoId;
} LogosdeliveryGetNodeInfoReq;
typedef struct {
    const char* peerMultiAddr;
    uint32_t timeoutMs;
} WakuConnectReq;
typedef struct {
    const char* peerId;
} WakuDisconnectPeerByIdReq;
typedef struct {
    const char* peerMultiAddr;
    const char* protocol;
    uint32_t timeoutMs;
} WakuDialPeerReq;
typedef struct {
    const char* peerId;
    const char* protocol;
    uint32_t timeoutMs;
} WakuDialPeerByIdReq;
typedef struct {
    const char* protocol;
} WakuGetPeeridsByProtocolReq;
typedef struct {
    const char* bootnodes;
} WakuDiscv5UpdateBootnodesReq;
typedef struct {
    const char* enrTreeUrl;
    const char* nameDnsServer;
    int32_t timeoutMs;
} WakuDnsDiscoveryReq;
typedef struct {
    const char* peerAddr;
    uint32_t timeoutMs;
} WakuPingPeerReq;
typedef struct {
    const char* pubSubTopic;
} WakuRelayGetPeersInMeshReq;
typedef struct {
    const char* pubSubTopic;
} WakuRelayGetNumPeersInMeshReq;
typedef struct {
    const char* pubSubTopic;
} WakuRelayGetConnectedPeersReq;
typedef struct {
    const char* pubSubTopic;
} WakuRelayGetNumConnectedPeersReq;
typedef struct {
    uint16_t clusterId;
    uint16_t shardId;
    const char* publicKey;
} WakuRelayAddProtectedShardReq;
typedef struct {
    const char* pubSubTopic;
} WakuRelaySubscribeReq;
typedef struct {
    const char* pubSubTopic;
} WakuRelayUnsubscribeReq;
typedef struct {
    const char* pubSubTopic;
    const char* jsonWakuMessage;
    uint32_t timeoutMs;
} WakuRelayPublishReq;
typedef struct {
    const char* appName;
    uint32_t appVersion;
    const char* contentTopicName;
    const char* encoding;
} WakuContentTopicReq;
typedef struct {
    const char* topicName;
} WakuPubsubTopicReq;
typedef struct {
    const char* jsonQuery;
    const char* peerAddr;
    int32_t timeoutMs;
} WakuStoreQueryReq;
typedef struct {
    const char* pubSubTopic;
    const char* jsonWakuMessage;
} WakuLightpushPublishReq;
typedef struct {
    const char* pubSubTopic;
    const char* contentTopics;
} WakuFilterSubscribeReq;
typedef struct {
    const char* pubSubTopic;
    const char* contentTopics;
} WakuFilterUnsubscribeReq;
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
    const char* messageJson;
} LogosdeliveryChannelSendReq;
typedef struct {
    const char* channelIdStr;
} LogosdeliveryChannelCloseReq;

typedef void (*LogosDeliveryStartNodeReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryStopNodeReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliverySubscribeReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryUnsubscribeReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliverySendReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryGetAvailableNodeInfoIdsReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryGetNodeInfoReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryGetAvailableConfigsReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuGetPeeridsFromPeerstoreReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuConnectReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuDisconnectPeerByIdReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuDisconnectAllPeersReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuDialPeerReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuDialPeerByIdReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuGetConnectedPeersInfoReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuGetConnectedPeersReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuGetPeeridsByProtocolReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuDiscv5UpdateBootnodesReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuDnsDiscoveryReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuStartDiscv5ReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuStopDiscv5ReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuPeerExchangeRequestReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuVersionReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuListenAddressesReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuGetMyEnrReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuGetMyPeeridReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuGetMetricsReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuIsOnlineReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuPingPeerReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuRelayGetPeersInMeshReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuRelayGetNumPeersInMeshReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuRelayGetConnectedPeersReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuRelayGetNumConnectedPeersReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuRelayAddProtectedShardReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuRelaySubscribeReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuRelayUnsubscribeReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuRelayPublishReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuDefaultPubsubTopicReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuContentTopicReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuPubsubTopicReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuStoreQueryReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuLightpushPublishReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuFilterSubscribeReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuFilterUnsubscribeReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryWakuFilterUnsubscribeAllReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryChannelCreateReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryChannelExistsReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryChannelSendReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);
typedef void (*LogosDeliveryChannelCloseReplyFn)(int err_code, const char* reply, const char* err_msg, void* user_data);

typedef void (*LogosDeliveryCreateRawFn)(int err_code, const char* ctx_addr, const char* err_msg, void* user_data);
/* Raw reply of a scalar-fast-path export: `msg`/`len` are bytes (a string
   return's UTF-8, or the 8-byte native-endian scalar image), not
   NUL-terminated and valid only for the duration of the call. */
typedef void (*LogosDeliveryScalarRawFn)(int caller_ret, char* msg, size_t len, void* user_data);
#ifndef NIMFFI_ABI_DUP_CSTR_N
#define NIMFFI_ABI_DUP_CSTR_N
/* NUL-terminated copy of a length-delimited byte run; NULL if it can't. */
static inline char* nimffi_abi_dup_cstr_n(const char* s, size_t n) {
    if (n == SIZE_MAX) return NULL;
    char* p = (char*)malloc(n + 1);
    if (p) {
        if (n > 0) memcpy(p, s, n);
        p[n] = '\0';
    }
    return p;
}
#endif
#ifndef NIMFFI_FFICALLBACK_DEFINED
#define NIMFFI_FFICALLBACK_DEFINED
typedef void (*FFICallback)(int ret, const char* msg, size_t len, void* user_data);
#endif
#ifdef __cplusplus
extern "C" {
#endif

void* logosdelivery_create_node(const LogosdeliveryCreateNodeCtorReq* req, LogosDeliveryCreateRawFn on_created, void* user_data);
int logosdelivery_start_node(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
int logosdelivery_stop_node(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
/**
 * Safety net for a host that skips `stop_node` (#4108): nim-ffi recycles the
 * worker rather than joining it, so an unstopped node keeps running.
 * The forwarders registered at create live until here, with the node's
 * broker scope; `teardownFFIEventScope` is the other end of create.
 */
int logosdelivery_destroy(void* ctx);
int logosdelivery_subscribe(void* ctx, LogosDeliverySubscribeReplyFn on_reply, void* user_data, const LogosdeliverySubscribeReq* req);
int logosdelivery_unsubscribe(void* ctx, LogosDeliveryUnsubscribeReplyFn on_reply, void* user_data, const LogosdeliveryUnsubscribeReq* req);
int logosdelivery_send(void* ctx, LogosDeliverySendReplyFn on_reply, void* user_data, const LogosdeliverySendReq* req);
/**
 * Returns, as a JSON array of strings, all available node info item ids that
 * can be queried with `get_node_info`.
 */
int logosdelivery_get_available_node_info_ids(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
/**
 * Returns the content of the node info item with the given id if it exists.
 * The content is a plain string, not JSON: a peer id, an ENR URI, a
 * comma-separated multiaddress list or the Prometheus metrics text.
 */
int logosdelivery_get_node_info(void* ctx, LogosDeliveryGetNodeInfoReplyFn on_reply, void* user_data, const LogosdeliveryGetNodeInfoReq* req);
/** Returns information about the accepted config items. */
int logosdelivery_get_available_configs(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
/** returns a comma-separated string of peerIDs */
int waku_get_peerids_from_peerstore(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
int waku_connect(void* ctx, LogosDeliveryWakuConnectReplyFn on_reply, void* user_data, const WakuConnectReq* req);
int waku_disconnect_peer_by_id(void* ctx, LogosDeliveryWakuDisconnectPeerByIdReplyFn on_reply, void* user_data, const WakuDisconnectPeerByIdReq* req);
int waku_disconnect_all_peers(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
int waku_dial_peer(void* ctx, LogosDeliveryWakuDialPeerReplyFn on_reply, void* user_data, const WakuDialPeerReq* req);
int waku_dial_peer_by_id(void* ctx, LogosDeliveryWakuDialPeerByIdReplyFn on_reply, void* user_data, const WakuDialPeerByIdReq* req);
/** returns a JSON string mapping peerIDs to objects with protocols and addresses */
int waku_get_connected_peers_info(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
/** returns a comma-separated string of peerIDs */
int waku_get_connected_peers(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
/** returns a comma-separated string of peerIDs that mount the given protocol */
int waku_get_peerids_by_protocol(void* ctx, LogosDeliveryWakuGetPeeridsByProtocolReplyFn on_reply, void* user_data, const WakuGetPeeridsByProtocolReq* req);
/**
 * Updates the bootnode list used for discovering new peers via DiscoveryV5
 * bootnodes - JSON array containing the bootnode ENRs i.e. `["enr:...", "enr:..."]`
 */
int waku_discv5_update_bootnodes(void* ctx, LogosDeliveryWakuDiscv5UpdateBootnodesReplyFn on_reply, void* user_data, const WakuDiscv5UpdateBootnodesReq* req);
/** returns a comma-separated string of bootstrap nodes' multiaddresses */
int waku_dns_discovery(void* ctx, LogosDeliveryWakuDnsDiscoveryReplyFn on_reply, void* user_data, const WakuDnsDiscoveryReq* req);
int waku_start_discv5(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
int waku_stop_discv5(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
int waku_peer_exchange_request(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data, uint64_t numPeers);
int waku_version(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
/** returns a comma-separated string of the listen addresses */
int waku_listen_addresses(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
int waku_get_my_enr(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
int waku_get_my_peerid(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
int waku_get_metrics(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
int waku_is_online(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
int waku_ping_peer(void* ctx, LogosDeliveryWakuPingPeerReplyFn on_reply, void* user_data, const WakuPingPeerReq* req);
/** returns a comma-separated string of peerIDs */
int waku_relay_get_peers_in_mesh(void* ctx, LogosDeliveryWakuRelayGetPeersInMeshReplyFn on_reply, void* user_data, const WakuRelayGetPeersInMeshReq* req);
int waku_relay_get_num_peers_in_mesh(void* ctx, LogosDeliveryWakuRelayGetNumPeersInMeshReplyFn on_reply, void* user_data, const WakuRelayGetNumPeersInMeshReq* req);
/** Returns the list of all connected peers to an specific pubsub topic */
int waku_relay_get_connected_peers(void* ctx, LogosDeliveryWakuRelayGetConnectedPeersReplyFn on_reply, void* user_data, const WakuRelayGetConnectedPeersReq* req);
int waku_relay_get_num_connected_peers(void* ctx, LogosDeliveryWakuRelayGetNumConnectedPeersReplyFn on_reply, void* user_data, const WakuRelayGetNumConnectedPeersReq* req);
/** Protects a shard with a public key */
int waku_relay_add_protected_shard(void* ctx, LogosDeliveryWakuRelayAddProtectedShardReplyFn on_reply, void* user_data, const WakuRelayAddProtectedShardReq* req);
int waku_relay_subscribe(void* ctx, LogosDeliveryWakuRelaySubscribeReplyFn on_reply, void* user_data, const WakuRelaySubscribeReq* req);
int waku_relay_unsubscribe(void* ctx, LogosDeliveryWakuRelayUnsubscribeReplyFn on_reply, void* user_data, const WakuRelayUnsubscribeReq* req);
int waku_relay_publish(void* ctx, LogosDeliveryWakuRelayPublishReplyFn on_reply, void* user_data, const WakuRelayPublishReq* req);
int waku_default_pubsub_topic(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
int waku_content_topic(void* ctx, LogosDeliveryWakuContentTopicReplyFn on_reply, void* user_data, const WakuContentTopicReq* req);
int waku_pubsub_topic(void* ctx, LogosDeliveryWakuPubsubTopicReplyFn on_reply, void* user_data, const WakuPubsubTopicReq* req);
int waku_store_query(void* ctx, LogosDeliveryWakuStoreQueryReplyFn on_reply, void* user_data, const WakuStoreQueryReq* req);
int waku_lightpush_publish(void* ctx, LogosDeliveryWakuLightpushPublishReplyFn on_reply, void* user_data, const WakuLightpushPublishReq* req);
int waku_filter_subscribe(void* ctx, LogosDeliveryWakuFilterSubscribeReplyFn on_reply, void* user_data, const WakuFilterSubscribeReq* req);
int waku_filter_unsubscribe(void* ctx, LogosDeliveryWakuFilterUnsubscribeReplyFn on_reply, void* user_data, const WakuFilterUnsubscribeReq* req);
int waku_filter_unsubscribe_all(void* ctx, LogosDeliveryScalarRawFn callback, void* user_data);
int logosdelivery_channel_create(void* ctx, LogosDeliveryChannelCreateReplyFn on_reply, void* user_data, const LogosdeliveryChannelCreateReq* req);
/** Returns `"true"` or `"false"`; a missing channel is not an error. */
int logosdelivery_channel_exists(void* ctx, LogosDeliveryChannelExistsReplyFn on_reply, void* user_data, const LogosdeliveryChannelExistsReq* req);
/** `messageJson` carries `{ "payload": <base64>, "ephemeral": <bool> }`. */
int logosdelivery_channel_send(void* ctx, LogosDeliveryChannelSendReplyFn on_reply, void* user_data, const LogosdeliveryChannelSendReq* req);
int logosdelivery_channel_close(void* ctx, LogosDeliveryChannelCloseReplyFn on_reply, void* user_data, const LogosdeliveryChannelCloseReq* req);
uint64_t logosdelivery_add_event_listener(void* ctx, const char* event_name, FFICallback callback, void* user_data);
int logosdelivery_remove_event_listener(void* ctx, uint64_t listener_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* High-level context wrapper */
typedef struct {
    void* ptr;
} LogosDeliveryCtx;

typedef void (*LogosDeliveryCreateFn)(int err_code, LogosDeliveryCtx* ctx, const char* err_msg, void* user_data);
typedef struct { LogosDeliveryCreateFn fn; void* user_data; } LogosDeliveryCreateBox;
static void logosdelivery_create_trampoline(int ret, const char* ctx_addr, const char* err_msg, void* ud) {
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
    bool ok = ctx_addr && *ctx_addr && endp && *endp == '\0';
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

static inline int logosdelivery_ctx_create(const char* configJson, LogosDeliveryCreateFn on_created, void* user_data) {
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

/**
 * Safety net for a host that skips `stop_node` (#4108): nim-ffi recycles the
 * worker rather than joining it, so an unstopped node keeps running.
 * The forwarders registered at create live until here, with the node's
 * broker scope; `teardownFFIEventScope` is the other end of create.
 */
static inline int logosdelivery_ctx_destroy(LogosDeliveryCtx* ctx) {
    if (!ctx) return NIMFFI_RET_OK;
    int rc = NIMFFI_RET_OK;
    if (ctx->ptr) { rc = logosdelivery_destroy(ctx->ptr); ctx->ptr = NULL; }
    free(ctx);
    return rc;
}

typedef struct { LogosDeliveryStartNodeReplyFn fn; void* user_data; } LogosDeliveryStartNodeScalarBox;
static void logosdelivery_start_node_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryStartNodeScalarBox* box = (LogosDeliveryStartNodeScalarBox*)ud;
    if (!box) return;
    LogosDeliveryStartNodeReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

static inline int logosdelivery_ctx_start_node(const LogosDeliveryCtx* ctx, LogosDeliveryStartNodeReplyFn on_reply, void* user_data) {
    LogosDeliveryStartNodeScalarBox* box = (LogosDeliveryStartNodeScalarBox*)malloc(sizeof(LogosDeliveryStartNodeScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return logosdelivery_start_node(ctx->ptr, logosdelivery_start_node_scalar_reply, box);
}

typedef struct { LogosDeliveryStopNodeReplyFn fn; void* user_data; } LogosDeliveryStopNodeScalarBox;
static void logosdelivery_stop_node_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryStopNodeScalarBox* box = (LogosDeliveryStopNodeScalarBox*)ud;
    if (!box) return;
    LogosDeliveryStopNodeReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

static inline int logosdelivery_ctx_stop_node(const LogosDeliveryCtx* ctx, LogosDeliveryStopNodeReplyFn on_reply, void* user_data) {
    LogosDeliveryStopNodeScalarBox* box = (LogosDeliveryStopNodeScalarBox*)malloc(sizeof(LogosDeliveryStopNodeScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return logosdelivery_stop_node(ctx->ptr, logosdelivery_stop_node_scalar_reply, box);
}

static inline int logosdelivery_ctx_subscribe(const LogosDeliveryCtx* ctx, const char* contentTopicStr, LogosDeliverySubscribeReplyFn on_reply, void* user_data) {
    LogosdeliverySubscribeReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.contentTopicStr = contentTopicStr;
    return logosdelivery_subscribe(ctx->ptr, on_reply, user_data, &ffi_req);
}

static inline int logosdelivery_ctx_unsubscribe(const LogosDeliveryCtx* ctx, const char* contentTopicStr, LogosDeliveryUnsubscribeReplyFn on_reply, void* user_data) {
    LogosdeliveryUnsubscribeReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.contentTopicStr = contentTopicStr;
    return logosdelivery_unsubscribe(ctx->ptr, on_reply, user_data, &ffi_req);
}

static inline int logosdelivery_ctx_send(const LogosDeliveryCtx* ctx, const char* messageJson, LogosDeliverySendReplyFn on_reply, void* user_data) {
    LogosdeliverySendReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.messageJson = messageJson;
    return logosdelivery_send(ctx->ptr, on_reply, user_data, &ffi_req);
}

typedef struct { LogosDeliveryGetAvailableNodeInfoIdsReplyFn fn; void* user_data; } LogosDeliveryGetAvailableNodeInfoIdsScalarBox;
static void logosdelivery_get_available_node_info_ids_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryGetAvailableNodeInfoIdsScalarBox* box = (LogosDeliveryGetAvailableNodeInfoIdsScalarBox*)ud;
    if (!box) return;
    LogosDeliveryGetAvailableNodeInfoIdsReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

/**
 * Returns, as a JSON array of strings, all available node info item ids that
 * can be queried with `get_node_info`.
 */
static inline int logosdelivery_ctx_get_available_node_info_ids(const LogosDeliveryCtx* ctx, LogosDeliveryGetAvailableNodeInfoIdsReplyFn on_reply, void* user_data) {
    LogosDeliveryGetAvailableNodeInfoIdsScalarBox* box = (LogosDeliveryGetAvailableNodeInfoIdsScalarBox*)malloc(sizeof(LogosDeliveryGetAvailableNodeInfoIdsScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return logosdelivery_get_available_node_info_ids(ctx->ptr, logosdelivery_get_available_node_info_ids_scalar_reply, box);
}

/**
 * Returns the content of the node info item with the given id if it exists.
 * The content is a plain string, not JSON: a peer id, an ENR URI, a
 * comma-separated multiaddress list or the Prometheus metrics text.
 */
static inline int logosdelivery_ctx_get_node_info(const LogosDeliveryCtx* ctx, const char* nodeInfoId, LogosDeliveryGetNodeInfoReplyFn on_reply, void* user_data) {
    LogosdeliveryGetNodeInfoReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.nodeInfoId = nodeInfoId;
    return logosdelivery_get_node_info(ctx->ptr, on_reply, user_data, &ffi_req);
}

typedef struct { LogosDeliveryGetAvailableConfigsReplyFn fn; void* user_data; } LogosDeliveryGetAvailableConfigsScalarBox;
static void logosdelivery_get_available_configs_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryGetAvailableConfigsScalarBox* box = (LogosDeliveryGetAvailableConfigsScalarBox*)ud;
    if (!box) return;
    LogosDeliveryGetAvailableConfigsReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

/** Returns information about the accepted config items. */
static inline int logosdelivery_ctx_get_available_configs(const LogosDeliveryCtx* ctx, LogosDeliveryGetAvailableConfigsReplyFn on_reply, void* user_data) {
    LogosDeliveryGetAvailableConfigsScalarBox* box = (LogosDeliveryGetAvailableConfigsScalarBox*)malloc(sizeof(LogosDeliveryGetAvailableConfigsScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return logosdelivery_get_available_configs(ctx->ptr, logosdelivery_get_available_configs_scalar_reply, box);
}

typedef struct { LogosDeliveryWakuGetPeeridsFromPeerstoreReplyFn fn; void* user_data; } LogosDeliveryWakuGetPeeridsFromPeerstoreScalarBox;
static void waku_get_peerids_from_peerstore_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryWakuGetPeeridsFromPeerstoreScalarBox* box = (LogosDeliveryWakuGetPeeridsFromPeerstoreScalarBox*)ud;
    if (!box) return;
    LogosDeliveryWakuGetPeeridsFromPeerstoreReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

/** returns a comma-separated string of peerIDs */
static inline int logosdelivery_ctx_waku_get_peerids_from_peerstore(const LogosDeliveryCtx* ctx, LogosDeliveryWakuGetPeeridsFromPeerstoreReplyFn on_reply, void* user_data) {
    LogosDeliveryWakuGetPeeridsFromPeerstoreScalarBox* box = (LogosDeliveryWakuGetPeeridsFromPeerstoreScalarBox*)malloc(sizeof(LogosDeliveryWakuGetPeeridsFromPeerstoreScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return waku_get_peerids_from_peerstore(ctx->ptr, waku_get_peerids_from_peerstore_scalar_reply, box);
}

static inline int logosdelivery_ctx_waku_connect(const LogosDeliveryCtx* ctx, const char* peerMultiAddr, uint32_t timeoutMs, LogosDeliveryWakuConnectReplyFn on_reply, void* user_data) {
    WakuConnectReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.peerMultiAddr = peerMultiAddr;
    ffi_req.timeoutMs = timeoutMs;
    return waku_connect(ctx->ptr, on_reply, user_data, &ffi_req);
}

static inline int logosdelivery_ctx_waku_disconnect_peer_by_id(const LogosDeliveryCtx* ctx, const char* peerId, LogosDeliveryWakuDisconnectPeerByIdReplyFn on_reply, void* user_data) {
    WakuDisconnectPeerByIdReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.peerId = peerId;
    return waku_disconnect_peer_by_id(ctx->ptr, on_reply, user_data, &ffi_req);
}

typedef struct { LogosDeliveryWakuDisconnectAllPeersReplyFn fn; void* user_data; } LogosDeliveryWakuDisconnectAllPeersScalarBox;
static void waku_disconnect_all_peers_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryWakuDisconnectAllPeersScalarBox* box = (LogosDeliveryWakuDisconnectAllPeersScalarBox*)ud;
    if (!box) return;
    LogosDeliveryWakuDisconnectAllPeersReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

static inline int logosdelivery_ctx_waku_disconnect_all_peers(const LogosDeliveryCtx* ctx, LogosDeliveryWakuDisconnectAllPeersReplyFn on_reply, void* user_data) {
    LogosDeliveryWakuDisconnectAllPeersScalarBox* box = (LogosDeliveryWakuDisconnectAllPeersScalarBox*)malloc(sizeof(LogosDeliveryWakuDisconnectAllPeersScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return waku_disconnect_all_peers(ctx->ptr, waku_disconnect_all_peers_scalar_reply, box);
}

static inline int logosdelivery_ctx_waku_dial_peer(const LogosDeliveryCtx* ctx, const char* peerMultiAddr, const char* protocol, uint32_t timeoutMs, LogosDeliveryWakuDialPeerReplyFn on_reply, void* user_data) {
    WakuDialPeerReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.peerMultiAddr = peerMultiAddr;
    ffi_req.protocol = protocol;
    ffi_req.timeoutMs = timeoutMs;
    return waku_dial_peer(ctx->ptr, on_reply, user_data, &ffi_req);
}

static inline int logosdelivery_ctx_waku_dial_peer_by_id(const LogosDeliveryCtx* ctx, const char* peerId, const char* protocol, uint32_t timeoutMs, LogosDeliveryWakuDialPeerByIdReplyFn on_reply, void* user_data) {
    WakuDialPeerByIdReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.peerId = peerId;
    ffi_req.protocol = protocol;
    ffi_req.timeoutMs = timeoutMs;
    return waku_dial_peer_by_id(ctx->ptr, on_reply, user_data, &ffi_req);
}

typedef struct { LogosDeliveryWakuGetConnectedPeersInfoReplyFn fn; void* user_data; } LogosDeliveryWakuGetConnectedPeersInfoScalarBox;
static void waku_get_connected_peers_info_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryWakuGetConnectedPeersInfoScalarBox* box = (LogosDeliveryWakuGetConnectedPeersInfoScalarBox*)ud;
    if (!box) return;
    LogosDeliveryWakuGetConnectedPeersInfoReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

/** returns a JSON string mapping peerIDs to objects with protocols and addresses */
static inline int logosdelivery_ctx_waku_get_connected_peers_info(const LogosDeliveryCtx* ctx, LogosDeliveryWakuGetConnectedPeersInfoReplyFn on_reply, void* user_data) {
    LogosDeliveryWakuGetConnectedPeersInfoScalarBox* box = (LogosDeliveryWakuGetConnectedPeersInfoScalarBox*)malloc(sizeof(LogosDeliveryWakuGetConnectedPeersInfoScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return waku_get_connected_peers_info(ctx->ptr, waku_get_connected_peers_info_scalar_reply, box);
}

typedef struct { LogosDeliveryWakuGetConnectedPeersReplyFn fn; void* user_data; } LogosDeliveryWakuGetConnectedPeersScalarBox;
static void waku_get_connected_peers_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryWakuGetConnectedPeersScalarBox* box = (LogosDeliveryWakuGetConnectedPeersScalarBox*)ud;
    if (!box) return;
    LogosDeliveryWakuGetConnectedPeersReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

/** returns a comma-separated string of peerIDs */
static inline int logosdelivery_ctx_waku_get_connected_peers(const LogosDeliveryCtx* ctx, LogosDeliveryWakuGetConnectedPeersReplyFn on_reply, void* user_data) {
    LogosDeliveryWakuGetConnectedPeersScalarBox* box = (LogosDeliveryWakuGetConnectedPeersScalarBox*)malloc(sizeof(LogosDeliveryWakuGetConnectedPeersScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return waku_get_connected_peers(ctx->ptr, waku_get_connected_peers_scalar_reply, box);
}

/** returns a comma-separated string of peerIDs that mount the given protocol */
static inline int logosdelivery_ctx_waku_get_peerids_by_protocol(const LogosDeliveryCtx* ctx, const char* protocol, LogosDeliveryWakuGetPeeridsByProtocolReplyFn on_reply, void* user_data) {
    WakuGetPeeridsByProtocolReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.protocol = protocol;
    return waku_get_peerids_by_protocol(ctx->ptr, on_reply, user_data, &ffi_req);
}

/**
 * Updates the bootnode list used for discovering new peers via DiscoveryV5
 * bootnodes - JSON array containing the bootnode ENRs i.e. `["enr:...", "enr:..."]`
 */
static inline int logosdelivery_ctx_waku_discv5_update_bootnodes(const LogosDeliveryCtx* ctx, const char* bootnodes, LogosDeliveryWakuDiscv5UpdateBootnodesReplyFn on_reply, void* user_data) {
    WakuDiscv5UpdateBootnodesReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.bootnodes = bootnodes;
    return waku_discv5_update_bootnodes(ctx->ptr, on_reply, user_data, &ffi_req);
}

/** returns a comma-separated string of bootstrap nodes' multiaddresses */
static inline int logosdelivery_ctx_waku_dns_discovery(const LogosDeliveryCtx* ctx, const char* enrTreeUrl, const char* nameDnsServer, int32_t timeoutMs, LogosDeliveryWakuDnsDiscoveryReplyFn on_reply, void* user_data) {
    WakuDnsDiscoveryReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.enrTreeUrl = enrTreeUrl;
    ffi_req.nameDnsServer = nameDnsServer;
    ffi_req.timeoutMs = timeoutMs;
    return waku_dns_discovery(ctx->ptr, on_reply, user_data, &ffi_req);
}

typedef struct { LogosDeliveryWakuStartDiscv5ReplyFn fn; void* user_data; } LogosDeliveryWakuStartDiscv5ScalarBox;
static void waku_start_discv5_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryWakuStartDiscv5ScalarBox* box = (LogosDeliveryWakuStartDiscv5ScalarBox*)ud;
    if (!box) return;
    LogosDeliveryWakuStartDiscv5ReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

static inline int logosdelivery_ctx_waku_start_discv5(const LogosDeliveryCtx* ctx, LogosDeliveryWakuStartDiscv5ReplyFn on_reply, void* user_data) {
    LogosDeliveryWakuStartDiscv5ScalarBox* box = (LogosDeliveryWakuStartDiscv5ScalarBox*)malloc(sizeof(LogosDeliveryWakuStartDiscv5ScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return waku_start_discv5(ctx->ptr, waku_start_discv5_scalar_reply, box);
}

typedef struct { LogosDeliveryWakuStopDiscv5ReplyFn fn; void* user_data; } LogosDeliveryWakuStopDiscv5ScalarBox;
static void waku_stop_discv5_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryWakuStopDiscv5ScalarBox* box = (LogosDeliveryWakuStopDiscv5ScalarBox*)ud;
    if (!box) return;
    LogosDeliveryWakuStopDiscv5ReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

static inline int logosdelivery_ctx_waku_stop_discv5(const LogosDeliveryCtx* ctx, LogosDeliveryWakuStopDiscv5ReplyFn on_reply, void* user_data) {
    LogosDeliveryWakuStopDiscv5ScalarBox* box = (LogosDeliveryWakuStopDiscv5ScalarBox*)malloc(sizeof(LogosDeliveryWakuStopDiscv5ScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return waku_stop_discv5(ctx->ptr, waku_stop_discv5_scalar_reply, box);
}

typedef struct { LogosDeliveryWakuPeerExchangeRequestReplyFn fn; void* user_data; } LogosDeliveryWakuPeerExchangeRequestScalarBox;
static void waku_peer_exchange_request_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryWakuPeerExchangeRequestScalarBox* box = (LogosDeliveryWakuPeerExchangeRequestScalarBox*)ud;
    if (!box) return;
    LogosDeliveryWakuPeerExchangeRequestReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

static inline int logosdelivery_ctx_waku_peer_exchange_request(const LogosDeliveryCtx* ctx, uint64_t numPeers, LogosDeliveryWakuPeerExchangeRequestReplyFn on_reply, void* user_data) {
    LogosDeliveryWakuPeerExchangeRequestScalarBox* box = (LogosDeliveryWakuPeerExchangeRequestScalarBox*)malloc(sizeof(LogosDeliveryWakuPeerExchangeRequestScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return waku_peer_exchange_request(ctx->ptr, waku_peer_exchange_request_scalar_reply, box, numPeers);
}

typedef struct { LogosDeliveryWakuVersionReplyFn fn; void* user_data; } LogosDeliveryWakuVersionScalarBox;
static void waku_version_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryWakuVersionScalarBox* box = (LogosDeliveryWakuVersionScalarBox*)ud;
    if (!box) return;
    LogosDeliveryWakuVersionReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

static inline int logosdelivery_ctx_waku_version(const LogosDeliveryCtx* ctx, LogosDeliveryWakuVersionReplyFn on_reply, void* user_data) {
    LogosDeliveryWakuVersionScalarBox* box = (LogosDeliveryWakuVersionScalarBox*)malloc(sizeof(LogosDeliveryWakuVersionScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return waku_version(ctx->ptr, waku_version_scalar_reply, box);
}

typedef struct { LogosDeliveryWakuListenAddressesReplyFn fn; void* user_data; } LogosDeliveryWakuListenAddressesScalarBox;
static void waku_listen_addresses_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryWakuListenAddressesScalarBox* box = (LogosDeliveryWakuListenAddressesScalarBox*)ud;
    if (!box) return;
    LogosDeliveryWakuListenAddressesReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

/** returns a comma-separated string of the listen addresses */
static inline int logosdelivery_ctx_waku_listen_addresses(const LogosDeliveryCtx* ctx, LogosDeliveryWakuListenAddressesReplyFn on_reply, void* user_data) {
    LogosDeliveryWakuListenAddressesScalarBox* box = (LogosDeliveryWakuListenAddressesScalarBox*)malloc(sizeof(LogosDeliveryWakuListenAddressesScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return waku_listen_addresses(ctx->ptr, waku_listen_addresses_scalar_reply, box);
}

typedef struct { LogosDeliveryWakuGetMyEnrReplyFn fn; void* user_data; } LogosDeliveryWakuGetMyEnrScalarBox;
static void waku_get_my_enr_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryWakuGetMyEnrScalarBox* box = (LogosDeliveryWakuGetMyEnrScalarBox*)ud;
    if (!box) return;
    LogosDeliveryWakuGetMyEnrReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

static inline int logosdelivery_ctx_waku_get_my_enr(const LogosDeliveryCtx* ctx, LogosDeliveryWakuGetMyEnrReplyFn on_reply, void* user_data) {
    LogosDeliveryWakuGetMyEnrScalarBox* box = (LogosDeliveryWakuGetMyEnrScalarBox*)malloc(sizeof(LogosDeliveryWakuGetMyEnrScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return waku_get_my_enr(ctx->ptr, waku_get_my_enr_scalar_reply, box);
}

typedef struct { LogosDeliveryWakuGetMyPeeridReplyFn fn; void* user_data; } LogosDeliveryWakuGetMyPeeridScalarBox;
static void waku_get_my_peerid_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryWakuGetMyPeeridScalarBox* box = (LogosDeliveryWakuGetMyPeeridScalarBox*)ud;
    if (!box) return;
    LogosDeliveryWakuGetMyPeeridReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

static inline int logosdelivery_ctx_waku_get_my_peerid(const LogosDeliveryCtx* ctx, LogosDeliveryWakuGetMyPeeridReplyFn on_reply, void* user_data) {
    LogosDeliveryWakuGetMyPeeridScalarBox* box = (LogosDeliveryWakuGetMyPeeridScalarBox*)malloc(sizeof(LogosDeliveryWakuGetMyPeeridScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return waku_get_my_peerid(ctx->ptr, waku_get_my_peerid_scalar_reply, box);
}

typedef struct { LogosDeliveryWakuGetMetricsReplyFn fn; void* user_data; } LogosDeliveryWakuGetMetricsScalarBox;
static void waku_get_metrics_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryWakuGetMetricsScalarBox* box = (LogosDeliveryWakuGetMetricsScalarBox*)ud;
    if (!box) return;
    LogosDeliveryWakuGetMetricsReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

static inline int logosdelivery_ctx_waku_get_metrics(const LogosDeliveryCtx* ctx, LogosDeliveryWakuGetMetricsReplyFn on_reply, void* user_data) {
    LogosDeliveryWakuGetMetricsScalarBox* box = (LogosDeliveryWakuGetMetricsScalarBox*)malloc(sizeof(LogosDeliveryWakuGetMetricsScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return waku_get_metrics(ctx->ptr, waku_get_metrics_scalar_reply, box);
}

typedef struct { LogosDeliveryWakuIsOnlineReplyFn fn; void* user_data; } LogosDeliveryWakuIsOnlineScalarBox;
static void waku_is_online_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryWakuIsOnlineScalarBox* box = (LogosDeliveryWakuIsOnlineScalarBox*)ud;
    if (!box) return;
    LogosDeliveryWakuIsOnlineReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

static inline int logosdelivery_ctx_waku_is_online(const LogosDeliveryCtx* ctx, LogosDeliveryWakuIsOnlineReplyFn on_reply, void* user_data) {
    LogosDeliveryWakuIsOnlineScalarBox* box = (LogosDeliveryWakuIsOnlineScalarBox*)malloc(sizeof(LogosDeliveryWakuIsOnlineScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return waku_is_online(ctx->ptr, waku_is_online_scalar_reply, box);
}

static inline int logosdelivery_ctx_waku_ping_peer(const LogosDeliveryCtx* ctx, const char* peerAddr, uint32_t timeoutMs, LogosDeliveryWakuPingPeerReplyFn on_reply, void* user_data) {
    WakuPingPeerReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.peerAddr = peerAddr;
    ffi_req.timeoutMs = timeoutMs;
    return waku_ping_peer(ctx->ptr, on_reply, user_data, &ffi_req);
}

/** returns a comma-separated string of peerIDs */
static inline int logosdelivery_ctx_waku_relay_get_peers_in_mesh(const LogosDeliveryCtx* ctx, const char* pubSubTopic, LogosDeliveryWakuRelayGetPeersInMeshReplyFn on_reply, void* user_data) {
    WakuRelayGetPeersInMeshReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.pubSubTopic = pubSubTopic;
    return waku_relay_get_peers_in_mesh(ctx->ptr, on_reply, user_data, &ffi_req);
}

static inline int logosdelivery_ctx_waku_relay_get_num_peers_in_mesh(const LogosDeliveryCtx* ctx, const char* pubSubTopic, LogosDeliveryWakuRelayGetNumPeersInMeshReplyFn on_reply, void* user_data) {
    WakuRelayGetNumPeersInMeshReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.pubSubTopic = pubSubTopic;
    return waku_relay_get_num_peers_in_mesh(ctx->ptr, on_reply, user_data, &ffi_req);
}

/** Returns the list of all connected peers to an specific pubsub topic */
static inline int logosdelivery_ctx_waku_relay_get_connected_peers(const LogosDeliveryCtx* ctx, const char* pubSubTopic, LogosDeliveryWakuRelayGetConnectedPeersReplyFn on_reply, void* user_data) {
    WakuRelayGetConnectedPeersReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.pubSubTopic = pubSubTopic;
    return waku_relay_get_connected_peers(ctx->ptr, on_reply, user_data, &ffi_req);
}

static inline int logosdelivery_ctx_waku_relay_get_num_connected_peers(const LogosDeliveryCtx* ctx, const char* pubSubTopic, LogosDeliveryWakuRelayGetNumConnectedPeersReplyFn on_reply, void* user_data) {
    WakuRelayGetNumConnectedPeersReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.pubSubTopic = pubSubTopic;
    return waku_relay_get_num_connected_peers(ctx->ptr, on_reply, user_data, &ffi_req);
}

/** Protects a shard with a public key */
static inline int logosdelivery_ctx_waku_relay_add_protected_shard(const LogosDeliveryCtx* ctx, uint16_t clusterId, uint16_t shardId, const char* publicKey, LogosDeliveryWakuRelayAddProtectedShardReplyFn on_reply, void* user_data) {
    WakuRelayAddProtectedShardReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.clusterId = clusterId;
    ffi_req.shardId = shardId;
    ffi_req.publicKey = publicKey;
    return waku_relay_add_protected_shard(ctx->ptr, on_reply, user_data, &ffi_req);
}

static inline int logosdelivery_ctx_waku_relay_subscribe(const LogosDeliveryCtx* ctx, const char* pubSubTopic, LogosDeliveryWakuRelaySubscribeReplyFn on_reply, void* user_data) {
    WakuRelaySubscribeReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.pubSubTopic = pubSubTopic;
    return waku_relay_subscribe(ctx->ptr, on_reply, user_data, &ffi_req);
}

static inline int logosdelivery_ctx_waku_relay_unsubscribe(const LogosDeliveryCtx* ctx, const char* pubSubTopic, LogosDeliveryWakuRelayUnsubscribeReplyFn on_reply, void* user_data) {
    WakuRelayUnsubscribeReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.pubSubTopic = pubSubTopic;
    return waku_relay_unsubscribe(ctx->ptr, on_reply, user_data, &ffi_req);
}

static inline int logosdelivery_ctx_waku_relay_publish(const LogosDeliveryCtx* ctx, const char* pubSubTopic, const char* jsonWakuMessage, uint32_t timeoutMs, LogosDeliveryWakuRelayPublishReplyFn on_reply, void* user_data) {
    WakuRelayPublishReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.pubSubTopic = pubSubTopic;
    ffi_req.jsonWakuMessage = jsonWakuMessage;
    ffi_req.timeoutMs = timeoutMs;
    return waku_relay_publish(ctx->ptr, on_reply, user_data, &ffi_req);
}

typedef struct { LogosDeliveryWakuDefaultPubsubTopicReplyFn fn; void* user_data; } LogosDeliveryWakuDefaultPubsubTopicScalarBox;
static void waku_default_pubsub_topic_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryWakuDefaultPubsubTopicScalarBox* box = (LogosDeliveryWakuDefaultPubsubTopicScalarBox*)ud;
    if (!box) return;
    LogosDeliveryWakuDefaultPubsubTopicReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

static inline int logosdelivery_ctx_waku_default_pubsub_topic(const LogosDeliveryCtx* ctx, LogosDeliveryWakuDefaultPubsubTopicReplyFn on_reply, void* user_data) {
    LogosDeliveryWakuDefaultPubsubTopicScalarBox* box = (LogosDeliveryWakuDefaultPubsubTopicScalarBox*)malloc(sizeof(LogosDeliveryWakuDefaultPubsubTopicScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return waku_default_pubsub_topic(ctx->ptr, waku_default_pubsub_topic_scalar_reply, box);
}

static inline int logosdelivery_ctx_waku_content_topic(const LogosDeliveryCtx* ctx, const char* appName, uint32_t appVersion, const char* contentTopicName, const char* encoding, LogosDeliveryWakuContentTopicReplyFn on_reply, void* user_data) {
    WakuContentTopicReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.appName = appName;
    ffi_req.appVersion = appVersion;
    ffi_req.contentTopicName = contentTopicName;
    ffi_req.encoding = encoding;
    return waku_content_topic(ctx->ptr, on_reply, user_data, &ffi_req);
}

static inline int logosdelivery_ctx_waku_pubsub_topic(const LogosDeliveryCtx* ctx, const char* topicName, LogosDeliveryWakuPubsubTopicReplyFn on_reply, void* user_data) {
    WakuPubsubTopicReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.topicName = topicName;
    return waku_pubsub_topic(ctx->ptr, on_reply, user_data, &ffi_req);
}

static inline int logosdelivery_ctx_waku_store_query(const LogosDeliveryCtx* ctx, const char* jsonQuery, const char* peerAddr, int32_t timeoutMs, LogosDeliveryWakuStoreQueryReplyFn on_reply, void* user_data) {
    WakuStoreQueryReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.jsonQuery = jsonQuery;
    ffi_req.peerAddr = peerAddr;
    ffi_req.timeoutMs = timeoutMs;
    return waku_store_query(ctx->ptr, on_reply, user_data, &ffi_req);
}

static inline int logosdelivery_ctx_waku_lightpush_publish(const LogosDeliveryCtx* ctx, const char* pubSubTopic, const char* jsonWakuMessage, LogosDeliveryWakuLightpushPublishReplyFn on_reply, void* user_data) {
    WakuLightpushPublishReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.pubSubTopic = pubSubTopic;
    ffi_req.jsonWakuMessage = jsonWakuMessage;
    return waku_lightpush_publish(ctx->ptr, on_reply, user_data, &ffi_req);
}

static inline int logosdelivery_ctx_waku_filter_subscribe(const LogosDeliveryCtx* ctx, const char* pubSubTopic, const char* contentTopics, LogosDeliveryWakuFilterSubscribeReplyFn on_reply, void* user_data) {
    WakuFilterSubscribeReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.pubSubTopic = pubSubTopic;
    ffi_req.contentTopics = contentTopics;
    return waku_filter_subscribe(ctx->ptr, on_reply, user_data, &ffi_req);
}

static inline int logosdelivery_ctx_waku_filter_unsubscribe(const LogosDeliveryCtx* ctx, const char* pubSubTopic, const char* contentTopics, LogosDeliveryWakuFilterUnsubscribeReplyFn on_reply, void* user_data) {
    WakuFilterUnsubscribeReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.pubSubTopic = pubSubTopic;
    ffi_req.contentTopics = contentTopics;
    return waku_filter_unsubscribe(ctx->ptr, on_reply, user_data, &ffi_req);
}

typedef struct { LogosDeliveryWakuFilterUnsubscribeAllReplyFn fn; void* user_data; } LogosDeliveryWakuFilterUnsubscribeAllScalarBox;
static void waku_filter_unsubscribe_all_scalar_reply(int caller_ret, char* msg, size_t len, void* ud) {
    LogosDeliveryWakuFilterUnsubscribeAllScalarBox* box = (LogosDeliveryWakuFilterUnsubscribeAllScalarBox*)ud;
    if (!box) return;
    LogosDeliveryWakuFilterUnsubscribeAllReplyFn fn = box->fn;
    void* user_data = box->user_data;
    free(box);
    if (!fn) return;
    if (caller_ret != NIMFFI_RET_OK) {
        char* em = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
        fn(caller_ret, "", em ? em : "FFI call failed", user_data);
        free(em);
        return;
    }
    char* reply = nimffi_abi_dup_cstr_n(msg ? msg : "", msg ? len : 0);
    if (!reply) {
        fn(NIMFFI_RET_ERR, "", "out of memory", user_data);
        return;
    }
    fn(NIMFFI_RET_OK, reply, "", user_data);
    free(reply);
}

static inline int logosdelivery_ctx_waku_filter_unsubscribe_all(const LogosDeliveryCtx* ctx, LogosDeliveryWakuFilterUnsubscribeAllReplyFn on_reply, void* user_data) {
    LogosDeliveryWakuFilterUnsubscribeAllScalarBox* box = (LogosDeliveryWakuFilterUnsubscribeAllScalarBox*)malloc(sizeof(LogosDeliveryWakuFilterUnsubscribeAllScalarBox));
    if (!box) {
        if (on_reply) on_reply(-1, "", "out of memory", user_data);
        return -1;
    }
    box->fn = on_reply;
    box->user_data = user_data;
    return waku_filter_unsubscribe_all(ctx->ptr, waku_filter_unsubscribe_all_scalar_reply, box);
}

static inline int logosdelivery_ctx_channel_create(const LogosDeliveryCtx* ctx, const char* channelIdStr, const char* contentTopicStr, const char* senderIdStr, LogosDeliveryChannelCreateReplyFn on_reply, void* user_data) {
    LogosdeliveryChannelCreateReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.channelIdStr = channelIdStr;
    ffi_req.contentTopicStr = contentTopicStr;
    ffi_req.senderIdStr = senderIdStr;
    return logosdelivery_channel_create(ctx->ptr, on_reply, user_data, &ffi_req);
}

/** Returns `"true"` or `"false"`; a missing channel is not an error. */
static inline int logosdelivery_ctx_channel_exists(const LogosDeliveryCtx* ctx, const char* channelIdStr, LogosDeliveryChannelExistsReplyFn on_reply, void* user_data) {
    LogosdeliveryChannelExistsReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.channelIdStr = channelIdStr;
    return logosdelivery_channel_exists(ctx->ptr, on_reply, user_data, &ffi_req);
}

/** `messageJson` carries `{ "payload": <base64>, "ephemeral": <bool> }`. */
static inline int logosdelivery_ctx_channel_send(const LogosDeliveryCtx* ctx, const char* channelIdStr, const char* messageJson, LogosDeliveryChannelSendReplyFn on_reply, void* user_data) {
    LogosdeliveryChannelSendReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.channelIdStr = channelIdStr;
    ffi_req.messageJson = messageJson;
    return logosdelivery_channel_send(ctx->ptr, on_reply, user_data, &ffi_req);
}

static inline int logosdelivery_ctx_channel_close(const LogosDeliveryCtx* ctx, const char* channelIdStr, LogosDeliveryChannelCloseReplyFn on_reply, void* user_data) {
    LogosdeliveryChannelCloseReq ffi_req;
    memset(&ffi_req, 0, sizeof(ffi_req));
    ffi_req.channelIdStr = channelIdStr;
    return logosdelivery_channel_close(ctx->ptr, on_reply, user_data, &ffi_req);
}

#endif /* NIM_FFI_LIB_LOGOSDELIVERY_C_ABI_H_INCLUDED */
