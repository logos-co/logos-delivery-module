#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <logos_module_context.h>
#include <logos_result.h>

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
 */
class DeliveryModuleImpl : public LogosModuleContext
{
public:
    DeliveryModuleImpl();
    ~DeliveryModuleImpl();

/**
 * @name Methods
 *
 * Every call returns as soon as its request is dispatched. Where the outcome
 * only becomes known later, it is reported through the events below.
 *
 * @{
 */

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
     * One such override is `anonymityLevel` — `"None"` (default), `"Preferred"`
     * or `"Required"`. Anything above `"None"` mounts mix and routes sends
     * through it, so it conflicts with an explicit `"mix": false`:
     * @code{.json}
     * {
     *   "mode": "Core",
     *   "preset": "logos.test",
     *   "messagingOverrides": { "anonymityLevel": "Required" }
     * }
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

    /**
     * @brief Lists the node info items this node advertises, for use with
     *        @ref getNodeInfo.
     *
     * The list comes back as a JSON array of strings:
     *
     * @code{.json}
     * ["Version", "Metrics", "MyMultiaddresses", "MyENR", "MyPeerId"]
     * @endcode
     *
     * Which items a node advertises depends on how it was built and
     * configured, so treat the set as discovered rather than fixed. An
     * advertised item may still return an empty value from @ref getNodeInfo
     * when the feature behind it is unconfigured.
     *
     * @return Success with the list above, or error details. Fails before
     *         @ref createNode has run.
     */
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
     * module: whenever it needs one, this module emits the matching
     * `rln*Request` event. Whoever handles that event answers by calling this
     * method with the same `reqId` and a `resultJson` reply envelope
     * (`{"ok": ...}` / `{"err": {"kind": ..., "message": ...}}`). The JSON
     * passes through this module verbatim — the wire schema is owned by the
     * RLN module and the delivery library, not modelled here.
     *
     * There is no response deadline to manage on this side: if no response
     * arrives in time, the delivery library synthesizes a TRANSIENT failure
     * itself. A response for a request that already timed out (or was never
     * issued) fails with an error.
     *
     * @param reqId Request id from the `rln*Request` event. Ids >= 2^63 appear
     *        negative here (int64 view of the library's uint64 id); they are
     *        passed through bit-exactly, so echo them back unchanged.
     * @param resultJson UTF-8 JSON reply envelope, passed through verbatim.
     * @return `true` when the response was accepted, or error details.
     */
    StdLogosResult rlnRespond(int64_t reqId, const std::string& resultJson);

    std::string name() const { return "delivery_module"; }

/** @} */

/**
 * @defgroup events Events
 *
 * Asynchronous notifications the module emits. Never invoked by a caller: the
 * codegen turns each declaration into an emitter. The rendered docs carry the
 * request-id and timestamp conventions that apply across all of them.
 *
 * @{
 */

logos_events:
    /**
     * @brief Emitted when the network has validated a sent message.
     *
     * The success terminal state for @ref send, usually preceded by
     * @ref messagePropagated.
     */
    void messageSent(const std::string& requestId, const std::string& messageHash, int64_t timestamp);

    /** @brief Emitted when the module could not send a message; `error` carries the reason. */
    void messageError(const std::string& requestId, const std::string& messageHash, const std::string& error, int64_t timestamp);

    /** @brief Emitted when a message has reached the network but is not yet validated. */
    void messagePropagated(const std::string& requestId, const std::string& messageHash, int64_t timestamp);

    /**
     * @brief Emitted when a message arrives on a subscribed content topic.
     *
     * `payload` is delivered as raw bytes, already decoded from the wire
     * encoding.
     */
    void messageReceived(const std::string& messageHash, const std::string& contentTopic, const std::vector<uint8_t>& payload, int64_t timestamp);

    /** @brief Emitted when the node's connectivity changes. */
    void connectionStateChanged(const std::string& connectionStatus, int64_t timestamp);

    /**
     * @brief Emitted when a message arrives on an open reliable channel.
     *
     * `senderId` is the sending participant's SDS identifier. `payload` is
     * delivered as raw bytes, already decoded from the wire encoding.
     */
    void channelMessageReceived(const std::string& channelId, const std::string& senderId, const std::vector<uint8_t>& payload, int64_t timestamp);

    /** @brief Emitted once every segment of a @ref channelSend is confirmed. */
    void channelMessageSent(const std::string& channelId, const std::string& requestId, int64_t timestamp);

    /** @brief Emitted when a @ref channelSend finalises with a failed segment. */
    void channelMessageError(const std::string& channelId, const std::string& requestId, const std::string& error, int64_t timestamp);

    /** @brief Emitted when @ref start finishes; `message` carries the reason when `success` is false. */
    void nodeStarted(bool success, const std::string& message, int64_t timestamp);

    /** @brief Emitted when @ref stop finishes; `message` carries the reason when `success` is false. */
    void nodeStopped(bool success, const std::string& message, int64_t timestamp);

    /**
     * @brief RLN request events: the delivery library requests an RLN
     * operation from the external RLN module, one event per ABI function
     * (`liblogosdelivery_rln.h`).
     *
     * Answer each via @ref rlnRespond with the same `reqId`. Event names and
     * argument shapes track the RLN module's own methods (register_membership
     * maps to the module's `register`); `configJson` / `optionsJson` /
     * `proofJson` are opaque JSON owned by the RLN module's wire schema.
     * `rateLimit` is register's positional rate limit; `epochTimestamp` is the
     * ABI's epoch/quota timestamp (Unix seconds), while the trailing
     * `timestamp` is the local emission time, as on every other event.
     */
    void rlnStartRequest(int64_t reqId, const std::string& configJson, int64_t timestamp);
    void rlnStopRequest(int64_t reqId, int64_t timestamp);
    void rlnRegisterRequest(int64_t reqId, const std::string& registryId,
                            const std::string& rlnIdentifier, int64_t rateLimit,
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

/** @} */

private:
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
                                      const char* rlnIdentifier, uint64_t rateLimit,
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
