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
 *
 * The raw FFI `eventType` values mapped into these typed events are:
 * - `message_sent` -> `messageSent`
 * - `message_error` -> `messageError`
 * - `message_propagated` -> `messagePropagated`
 * - `message_received` -> `messageReceived`
 * - `connection_status_change` -> `connectionStateChanged`
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
     * @brief Creates a liblogosdelivery node from a WakuNodeConf JSON document.
     *
     * The JSON is parsed by logos-delivery (liblogosdelivery folder) side and maps to
     * `WakuNodeConf` from `tools/confutils/cli_args.nim`
     * (https://github.com/logos-messaging/logos-delivery).
     *
     * The configuration is a **flat** JSON object whose keys correspond to
     * `WakuNodeConf` Nim field names (camelCase). Unknown keys are silently
     * ignored. Every field has a built-in default, so only the values that
     * differ from defaults need to be supplied.
     *
     * ## Commonly used keys
     * | Key                  | Type             | Default    | Description                                 |
     * |----------------------|------------------|------------|---------------------------------------------|
     * | `mode`               | string           | `"noMode"` | `"Core"`, `"Edge"`, or `"noMode"`           |
     * | `preset`             | string           | `""`       | Network preset (`"twn"`, `"logos.dev"`, …)  |
     * | `clusterId`          | number (uint16)  | `0`        | Cluster identifier                          |
     * | `entryNodes`         | array of string  | `[]`       | Bootstrap peers (enrtree / multiaddress)    |
     * | `relay`              | boolean          | `false`    | Enable relay protocol                       |
     * | `rlnRelay`           | boolean          | `false`    | Enable RLN rate-limit nullifier             |
     * | `tcpPort`            | number (uint16)  | `60000`    | P2P TCP listen port                         |
     * | `numShardsInNetwork` | number (uint16)  | `1`        | Auto-sharding shard count                   |
     * | `logLevel`           | string           | `"INFO"`   | `"TRACE"`, `"DEBUG"`, `"INFO"`, `"WARN"`, … |
     * | `logFormat`          | string           | `"TEXT"`   | `"TEXT"` or `"JSON"`                        |
     * | `maxMessageSize`     | string           | `"150KiB"` | Maximum message payload size                |
     *
     * ## Presets
     * Using a `preset` populates cluster ID, entry nodes, sharding, RLN, and
     * other network-specific defaults automatically. Individual keys supplied
     * alongside a preset override the preset values.
     * - `"twn"` – The RLN-protected Waku Network (cluster 1).
     * - `"logos.dev"` – Logos Dev Network (cluster 2, mix enabled,
     *   p2pReliability on, 8 auto-shards, built-in bootstrap nodes).
     *
     * Minimal `logos.dev` example:
     * @code{.json}
     * {
     *   "logLevel": "INFO",
     *   "mode": "Core",
     *   "preset": "logos.dev"
     * }
     * @endcode
     *
     * Full override example:
     * @code{.json}
     * {
     *   "mode": "Core",
     *   "clusterId": 42,
     *   "entryNodes": ["enrtree://TREE@nodes.example.com"],
     *   "relay": true,
     *   "tcpPort": 60000,
     *   "numShardsInNetwork": 8,
     *   "maxMessageSize": "150KiB",
     *   "logLevel": "INFO",
     *   "logFormat": "TEXT"
     * }
     * @endcode
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

    std::string name() const { return "delivery_module"; }

    std::string version() const;

logos_events:
    void messageSent(const std::string& requestId, const std::string& messageHash, int64_t timestamp);
    void messageError(const std::string& requestId, const std::string& messageHash, const std::string& error, int64_t timestamp);
    void messagePropagated(const std::string& requestId, const std::string& messageHash, int64_t timestamp);
    void messageReceived(const std::string& messageHash, const std::string& contentTopic, const std::vector<uint8_t>& payload, int64_t timestamp);
    void connectionStateChanged(const std::string& connectionStatus, int64_t timestamp);

    void nodeStarted(bool success, const std::string& message, int64_t timestamp);
    void nodeStopped(bool success, const std::string& message, int64_t timestamp);

private:
    void* deliveryCtx;

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
    static void start_callback(int callerRet, const char* msg, size_t len, void* userData);
    static void stop_callback(int callerRet, const char* msg, size_t len, void* userData);
};
