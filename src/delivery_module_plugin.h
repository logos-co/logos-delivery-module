#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <logos_module_context.h>
#include <logos_result.h>

class RlnBridge;

/**
 * @brief Pure C++ implementation of the delivery messaging module.
 *
 * This class adapts the universal module API to liblogosdelivery C-FFI calls
 * and forwards asynchronous events back to the host through typed events
 * declared in the `logos_events:` section.
 *
 * Lifecycle contract:
 * - call @ref createNode exactly once per context
 * - call @ref start before message operations
 * - use @ref subscribe / @ref send / @ref unsubscribe as needed
 * - call @ref stop before shutdown
 *
 * @ref createNode is synchronous. @ref start and @ref stop return once the
 * request is dispatched; completion is reported via the `nodeStarted` /
 * `nodeStopped` events.
 *
 * Asynchronous events are emitted via typed `logos_events:` declarations.
 * The codegen generates method bodies that route through
 * LogosModuleContext::emitEventImpl_.
 *
 * The raw FFI `eventType` values mapped into these typed events are:
 * - `message_sent` -> `messageSent`
 * - `message_error` -> `messageError`
 * - `message_propagated` -> `messagePropagated`
 * - `message_received` -> `messageReceived`
 * - `connection_status_change` -> `connectionStateChanged`
 * - `channel_message_received` -> `channelMessageReceived`
 * - `channel_message_sent` -> `channelMessageSent`
 * - `channel_message_error` -> `channelMessageError`
 *
 * As a general concept consider using proper content_topic format for your purpose.
 * --> https://lip.logos.co/messaging/informational/23/topics.html#content-topics
 */
class DeliveryModuleImpl : public LogosModuleContext
{
public:
    DeliveryModuleImpl();
    ~DeliveryModuleImpl();

    /**
     * @brief Creates a liblogosdelivery node from a JSON configuration.
     *
     * The JSON passes through to logos-delivery verbatim; `parseLogosDeliveryConf`
     * (https://github.com/logos-messaging/logos-delivery) owns the grammar.
     * `entryLayer` selects how much of the stack is mounted:
     * - `"kernel"` — transport node only
     * - `"messaging"` — kernel + messaging client
     * - `"channels"` — kernel + messaging + reliable channels (default)
     *
     * Three typical shapes:
     *
     * **App developer** — full stack (default `entryLayer`). `preset` picks the
     * network (`"logos.test"`, `"logos.dev"`, `"twn"`), `mode` picks the protocol
     * flags (`"Core"` = relay node, `"Edge"` = light node). Optional
     * `messagingOverrides` / `channelsOverrides` objects override per-layer
     * defaults:
     * @code{.json}
     * { "mode": "Core", "preset": "logos.test" }
     * @endcode
     *
     * **Node operator** — kernel-only service node on a public network. `mode`
     * is not applied on this layer, so protocol flags are set explicitly in
     * `kernelConf`:
     * @code{.json}
     * {
     *   "entryLayer": "kernel",
     *   "kernelConf": { "preset": "logos.test", "relay": true }
     * }
     * @endcode
     *
     * **Network hoster** — kernel-only node on a self-hosted network;
     * `kernelConf` is a raw `WakuNodeConf` used as-is:
     * @code{.json}
     * {
     *   "entryLayer": "kernel",
     *   "kernelConf": { "clusterId": 42, "relay": true, "entryNodes": ["/dns4/…"] }
     * }
     * @endcode
     *
     * On kernel-only nodes `send` / `subscribe` / `channel*` fail with "node has
     * no messaging client" / "no reliable channel manager"; `getNodeInfo`,
     * `storeQuery` and metrics keep working.
     *
     * The pre-layered flat shape (bare `WakuNodeConf` keys at top level) still
     * parses and boots the full stack.
     *
     * @param cfg UTF-8 JSON payload string.
     * @return `true` if context creation succeeds and callback returns `RET_OK`,
     *         otherwise `false`.
     */
    StdLogosResult createNode(const std::string& cfg);

    /**
     * @brief Starts the delivery node.
     * @return `true` once dispatched; completion is reported via `nodeStarted`.
     */
    StdLogosResult start();

    /**
     * @brief Stops the delivery node.
     * @return `true` once dispatched; completion is reported via `nodeStopped`.
     */
    StdLogosResult stop();

    /**
     * @brief Sends a message over the active node.
     *
     * Builds a JSON envelope expected by `logosdelivery_send`:
     * `{ "contentTopic": string, "payload": base64, "ephemeral": false }`.
     *
     * Returns a requestId on success. Async results come via typed events:
     * - `messageError` emitted if the module can't send the message
     * - `messagePropagated` emitted if the message has hit the network
     * - `messageSent` emitted after the message is validated by the network
     *
     * @param contentTopic Destination content topic.
     * @param payload Raw message bytes; base64-encoded before crossing the FFI boundary.
     * @return Success with request id, or error details.
     */
    StdLogosResult send(const std::string& contentTopic, const std::vector<uint8_t>& payload);

    /**
     * @brief Subscribes to the supplied content topic.
     * @param contentTopic Topic identifier.
     * @return `true` when subscribed successfully, otherwise `false`.
     */
    StdLogosResult subscribe(const std::string& contentTopic);

    /**
     * @brief Unsubscribes from the supplied content topic.
     * @param contentTopic Topic identifier.
     * @return `true` when unsubscribed successfully, otherwise `false`.
     */
    StdLogosResult unsubscribe(const std::string& contentTopic);

    /**
     * @brief Runs a Store (historical message) query against a specific store
     *        service peer.
     *
     * ⚠️ USE AT YOUR OWN RISK: backed by the kernel API (`waku_store_query`,
     * `liblogosdelivery_kernel.h`), which is subject to change at any point
     * without a deprecation cycle. This method's JSON contract follows it.
     *
     * The query JSON maps to logos-delivery's `StoreQueryRequest`
     * (`library/kernel_api/protocols/store_api.nim`):
     * | Key                 | Type            | Required | Description                                        |
     * |---------------------|-----------------|----------|----------------------------------------------------|
     * | `requestId`         | string          | yes      | Caller-chosen id, echoed in the response           |
     * | `includeData`       | boolean         | yes      | `true` returns full messages, `false` hashes only  |
     * | `paginationForward` | boolean         | yes      | Paging direction                                   |
     * | `pubsubTopic`       | string          | no       | Pubsub topic filter                                |
     * | `contentTopics`     | array of string | no       | Content topic filters                              |
     * | `timeStart`         | number/string   | no       | Range start, nanoseconds since Unix epoch          |
     * | `timeEnd`           | number/string   | no       | Range end, nanoseconds since Unix epoch            |
     * | `messageHashes`     | array of string | no       | Hex message hashes for lookup-by-hash queries      |
     * | `paginationCursor`  | string          | no       | Hex cursor from a previous response                |
     * | `paginationLimit`   | number          | no       | Max messages per page                              |
     *
     * On success the result value is the response JSON (`StoreQueryResponseHex`):
     * `{ "requestId", "statusCode", "statusDesc", "messages": [ { "messageHash",
     * "message", "pubsubTopic" } ], "paginationCursor" }` with hashes 0x-hex
     * encoded.
     *
     * @param jsonQuery UTF-8 JSON query document, see above.
     * @param peerAddr Multiaddress of the store service peer to query
     *        (e.g. `/ip4/127.0.0.1/tcp/60000/p2p/16Uiu2...`).
     * @param timeoutMs Query timeout in milliseconds.
     * @return Success with the response JSON, or error details.
     */
    StdLogosResult storeQuery(const std::string& jsonQuery,
                              const std::string& peerAddr,
                              int64_t timeoutMs);

    /**
     * @brief Creates (or re-opens) a reliable channel.
     *
     * Persisted channel state survives @ref channelClose, so re-creating a
     * channel with the same id restores it.
     *
     * @param channelId Application-chosen channel identifier.
     * @param contentTopic Content topic the channel communicates on.
     * @param senderId This participant's SDS (Scalable Data Sync) sender identifier.
     * @return Success with the channel id, or error details.
     */
    StdLogosResult channelCreate(const std::string& channelId,
                                 const std::string& contentTopic,
                                 const std::string& senderId);

    /**
     * @brief Checks whether a reliable channel is currently open.
     *
     * An unknown channel id is not an error.
     *
     * @param channelId Channel identifier.
     * @return Success with `"true"` or `"false"` (verbatim FFI string), or error details.
     */
    StdLogosResult channelExists(const std::string& channelId);

    /**
     * @brief Sends a message on a reliable channel.
     *
     * Builds the JSON envelope expected by `logosdelivery_channel_send`:
     * `{ "payload": base64, "ephemeral": false }`.
     *
     * Returns a requestId on success. Async results come via typed events:
     * - `channelMessageSent` once every segment of the send is confirmed
     * - `channelMessageError` if the send finalises with a failed segment
     *
     * @param channelId Channel identifier.
     * @param payload Raw message bytes; base64-encoded before crossing the FFI boundary.
     * @return Success with request id, or error details.
     */
    StdLogosResult channelSend(const std::string& channelId, const std::vector<uint8_t>& payload);

    /**
     * @brief Closes a reliable channel: stops its SDS loops.
     *
     * Persisted state survives, so @ref channelCreate with the same id
     * restores the channel.
     *
     * @param channelId Channel identifier.
     * @return `true` when closed successfully, otherwise `false`.
     */
    StdLogosResult channelClose(const std::string& channelId);

    StdLogosResult getAvailableNodeInfoIDs();

    /**
     * @brief Returns information for the given node info item.
     * @param nodeInfoId Identifier for the requested node info item.
     * @return JSON data string on success, or error details.
     */
    StdLogosResult getNodeInfo(const std::string& nodeInfoId);

    /**
     * @brief Information about the available configuration parameters for `createNode`.
     */
    StdLogosResult getAvailableConfigs();

    /**
     * @brief Returns the node's metrics as an OpenMetrics/Prometheus text
     *        document, so the `openmetrics` module can scrape this module.
     *
     * liblogosdelivery already aggregates Prometheus metrics in its global
     * registry and renders them as exposition text behind the `"Metrics"`
     * node-info attribute. This method just hands that text back verbatim — no
     * reshaping — which satisfies the openmetrics `metrics_source` interface's
     * `collectOpenMetricsText()` convention. The openmetrics scraper parses the
     * text, injects a `module="delivery_module"` label on every series, and
     * merges it with other modules. Select this method per-module in the
     * openmetrics `start` config with `{"name":"delivery_module","format":"text"}`.
     *
     * Returns an empty string before a node has been created, or when the
     * underlying read fails, so a scrape never errors out on this module.
     *
     * @return OpenMetrics/Prometheus exposition text (possibly empty).
     */
    std::string collectOpenMetricsText();

    /**
     * @brief Completes an outstanding RLN request (see the `rln*Request` events).
     *
     * The delivery library outsources RLN operations to an external RLN
     * module; this module facilitates that message passing. When the delivery
     * library makes an RLN request, this module emits the matching
     * `rln*Request` event. This method takes the reqId of the original
     * request along with the response and passes it on to the library
     * verbatim — the wire schema is owned by the RLN module and the delivery
     * library, not modelled here.
     *
     * On a node running lez RLN the in-process bridge (see
     * @ref rlnBridgeEnable) answers each request itself; only the first
     * response per reqId is accepted, so a second caller of this method is
     * rejected as a duplicate.
     *
     * There is no response deadline to manage on this side: if no response
     * arrives in time, the delivery library synthesizes a TRANSIENT failure
     * itself. A response for a request that already timed out (or was never
     * issued) fails with an error.
     *
     * @param reqId Request id from the `rln*Request` event. Ids >= 2^63 appear
     *        negative here (int64 view of the library's uint64 id); they are
     *        passed through bit-exactly, so echo them back unchanged.
     */
    StdLogosResult rlnRespond(int64_t reqId, const std::string& resultJson);

    /**
     * @brief Enables the in-process RLN bridge.
     *
     * Once enabled, each `rln*Request` is answered inside this module: the
     * bridge's worker threads call the co-loaded `liblogos_rln_module` and
     * pass its reply back unchanged. The events keep emitting for
     * observability, but an external responder must not also answer an
     * enabled node: its second response per reqId is rejected. Idempotent;
     * call any time before @ref start. @ref createNode does this
     * automatically when the config's `rln-relay-lez` is true. Calling it
     * directly is mainly for test purposes.
     */
    StdLogosResult rlnBridgeEnable();

    std::string name() const { return "delivery_module"; }

logos_events:
    void messageSent(const std::string& requestId, const std::string& messageHash, int64_t timestamp);
    void messageError(const std::string& requestId, const std::string& messageHash, const std::string& error, int64_t timestamp);
    void messagePropagated(const std::string& requestId, const std::string& messageHash, int64_t timestamp);
    void messageReceived(const std::string& messageHash, const std::string& contentTopic, const std::vector<uint8_t>& payload, int64_t timestamp);
    void connectionStateChanged(const std::string& connectionStatus, int64_t timestamp);

    void channelMessageReceived(const std::string& channelId, const std::string& senderId, const std::vector<uint8_t>& payload, int64_t timestamp);
    void channelMessageSent(const std::string& channelId, const std::string& requestId, int64_t timestamp);
    void channelMessageError(const std::string& channelId, const std::string& requestId, const std::string& error, int64_t timestamp);

    void nodeStarted(bool success, const std::string& message, int64_t timestamp);
    void nodeStopped(bool success, const std::string& message, int64_t timestamp);

    /**
     * RLN request events, one per ABI function (`liblogosdelivery_rln.h`).
     * Answer each via @ref rlnRespond with the same `reqId`. The JSON args are
     * opaque to this module (RLN module wire schema). `epochTimestamp` is the
     * Unix-seconds epoch/quota timestamp; the trailing `timestamp` is the
     * local emission time, as on every other event.
     */
    void rlnStartRequest(int64_t reqId, const std::string& configJson, int64_t timestamp);
    void rlnStopRequest(int64_t reqId, int64_t timestamp);
    void rlnRegisterRequest(int64_t reqId, const std::string& registryId,
                            const std::string& rlnIdentifier,
                            const std::string& optionsJson, int64_t timestamp);
    void rlnGetMembershipStateRequest(int64_t reqId, const std::string& registryId,
                                      const std::string& rlnIdentifier, int64_t timestamp);
    void rlnGetEpochQuotaRequest(int64_t reqId, const std::string& registryId,
                                 const std::string& rlnIdentifier,
                                 int64_t epochTimestamp, int64_t timestamp);
    void rlnGenerateProofRequest(int64_t reqId, const std::string& registryId,
                                 const std::string& rlnIdentifier, const std::string& signalHex,
                                 int64_t epochTimestamp, int64_t timestamp);
    void rlnValidateProofRequest(int64_t reqId, const std::string& registryId,
                                 const std::string& rlnIdentifier, const std::string& signalHex,
                                 int64_t epochTimestamp, const std::string& proofJson,
                                 int64_t timestamp);

private:
    // Wires the bridge to the co-loaded RLN module on first use — modules()
    // is only valid once the framework has handed the context over — then
    // starts it. Both enable doors (rlnBridgeEnable, the rln-relay-lez config
    // path) funnel through here. Returns an error string, or empty.
    std::string enableRlnBridge();

    // In-process RLN responder (src/rln_bridge.h). Constructed empty; wired
    // and started by enableRlnBridge().
    std::unique_ptr<RlnBridge> rlnBridge;

    // Raw FFI context: what every call and the event registry take.
    void* deliveryCtx;
    // Owning handle from logosdelivery_ctx_create (a LogosDeliveryCtx*), held
    // as void* so the C ABI header stays out of this header's includers.
    // Released with logosdelivery_ctx_destroy.
    void* deliveryCtxHandle;

    std::mutex createNodeMutex;

    static constexpr std::chrono::seconds CALLBACK_TIMEOUT{30};

    /**
     * @brief Global C callback used by liblogosdelivery to report async events.
     * @param callerRet FFI return code associated with callback dispatch.
     * @param msg UTF-8 JSON event payload buffer.
     * @param len Message length in bytes.
     * @param userData Opaque pointer expected to be `DeliveryModuleImpl*`.
     */
    static void event_callback(int callerRet, const char* msg, size_t len, void* userData);

    // Completion callbacks for start()/stop(); emit nodeStarted / nodeStopped.
    // userData is the DeliveryModuleImpl*.
    // Both take the scalar-fast-path reply shape and ignore RET_STALE_WARN,
    // the non-terminal progress tick a long start/stop emits.
    static void start_callback(int callerRet, char* msg, size_t len, void* userData);
    static void stop_callback(int callerRet, char* msg, size_t len, void* userData);

    // RLN callback slots registered in createNode, one per ABI function
    // (liblogosdelivery_rln.h); each emits its rln*Request event. Fired by
    // liblogosdelivery, possibly on a foreign thread. All strings are borrowed
    // for the duration of the call. userData is the DeliveryModuleImpl*.
    static void rln_start_callback(uint64_t reqId, const char* configJson, void* userData);
    static void rln_stop_callback(uint64_t reqId, void* userData);
    static void rln_register_callback(uint64_t reqId, const char* registryId,
                                      const char* rlnIdentifier,
                                      const char* optionsJson, void* userData);
    static void rln_get_membership_state_callback(uint64_t reqId, const char* registryId,
                                                  const char* rlnIdentifier, void* userData);
    static void rln_get_epoch_quota_callback(uint64_t reqId, const char* registryId,
                                             const char* rlnIdentifier,
                                             uint64_t timestamp, void* userData);
    static void rln_generate_proof_callback(uint64_t reqId, const char* registryId,
                                            const char* rlnIdentifier, const char* signalHex,
                                            uint64_t timestamp, void* userData);
    static void rln_validate_proof_callback(uint64_t reqId, const char* registryId,
                                            const char* rlnIdentifier, const char* signalHex,
                                            uint64_t timestamp, const char* proofJson,
                                            void* userData);
};
