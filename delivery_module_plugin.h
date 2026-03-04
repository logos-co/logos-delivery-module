#pragma once

#include <QtCore/QObject>
#include <chrono>
#include "delivery_module_interface.h"
#include "logos_api.h"
#include "logos_api_client.h"

/**
 * @brief Concrete Qt plugin implementing the delivery messaging module.
 *
 * This class adapts the host plugin API to liblogosdelivery C-FFI calls and
 * forwards asynchronous events back to the host through Logos API clients.
 *
 * Lifecycle contract:
 * - call @ref createNode exactly once per context
 * - call @ref start before message operations
 * - use @ref subscribe / @ref send / @ref unsubscribe as needed
 * - call @ref stop before shutdown
 * Notice all of these calls are synchronous.
 * 
 * Asynchronous events are emitted off thread as Logos Plugin events.
 * Emitted plugin event contracts (name + `QVariantList data` indices):
 * - `messageSent` (see `send` method)
 *   - `data[0]` (`QString`): request id
 *   - `data[1]` (`QString`): message hash
 *   - `data[2]` (`QString`): local timestamp (ISO-8601)
 * - `messageError` (see `send` method)
 *   - `data[0]` (`QString`): request id
 *   - `data[1]` (`QString`): message hash
 *   - `data[2]` (`QString`): error message
 *   - `data[3]` (`QString`): local timestamp (ISO-8601)
 * - `messagePropagated` (see `send` method)
 *   - `data[0]` (`QString`): request id
 *   - `data[1]` (`QString`): message hash
 *   - `data[2]` (`QString`): local timestamp (ISO-8601)
 * - `messageReceived` (emitted when a message arrives on a subscribed topic)
 *   - `data[0]` (`QString`): message hash
 *   - `data[1]` (`QString`): content topic
 *   - `data[2]` (`QString`): payload (base64-encoded)
 *   - `data[3]` (`QString`): timestamp (nanoseconds since epoch)
 * - `connectionStateChanged`
 *   - `data[0]` (`QString`): connection status
 *   - `data[1]` (`QString`): local timestamp (ISO-8601)
 *
 * The raw FFI `eventType` values mapped into these plugin events are:
 * - `message_sent` -> `messageSent`
 * - `message_error` -> `messageError`
 * - `message_propagated` -> `messagePropagated`
 * - `message_received` -> `messageReceived`
 * - `connection_status_change` -> `connectionStateChanged`
 * 
 * As a general concept consider using proper content_topic format for your purpose.
 * --> https://lip.logos.co/messaging/informational/23/topics.html#content-topics
 */
class DeliveryModulePlugin : public QObject, public DeliveryModuleInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID DeliveryModuleInterface_iid FILE "metadata.json")
    Q_INTERFACES(DeliveryModuleInterface PluginInterface)

public:
    /**
     * @brief Constructs the plugin with no active delivery context.
     */
    DeliveryModulePlugin();

    /**
     * @brief Destroys the plugin and releases owned resources.
     *
     * If present, the owned `LogosAPI` instance is deleted and the underlying
     * liblogosdelivery context is destroyed.
     */
    virtual ~DeliveryModulePlugin();

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
     * @param cfg UTF-16 Qt string containing a UTF-8 serializable JSON payload.
     * @return `true` if context creation succeeds and callback returns `RET_OK`,
     *         otherwise `false`.
     */
    Q_INVOKABLE bool createNode(const QString &cfg) override;

    /**
     * @brief Starts the delivery node.
     * @return `true` on success; `false` when no context exists or start fails.
     */
    Q_INVOKABLE bool start() override;

    /**
     * @brief Stops the delivery node.
     * @return `true` on success; `false` when no context exists or stop fails.
     */
    Q_INVOKABLE bool stop() override;

    /**
     * @brief Sends a message over the active node.
     *
     * This method builds a JSON envelope expected by `logosdelivery_send`:
     * `{ "contentTopic": string, "payload": base64, "ephemeral": (bool, default: false) }`.
     *
     * `send` call validates the input and returns with an associated requestId.
     * After all the exact send operation is done async and user can expect Message events in response
     * The requestId helps keep track of send operation results.
     * - `messageError` emitted in case module can't sent the message
     * - `messagePropagated` emitted if message has hit the network, you can expect delivery but 
     *                       module could not validate it yet.
     * - `messageSent` emitted after the sent message is validated by the network.
     * 
     * @param contentTopic Destination content topic.
     * @param payload Raw message bytes represented as QString; converted to UTF-8
     *                bytes and base64-encoded before crossing the FFI boundary.
     * @return Success with request id, or error details.
     */
    Q_INVOKABLE QExpected<QString> send(const QString &contentTopic, const QString &payload) override;

    /**
     * @brief Subscribes to the supplied content topic.
     * @param contentTopic Topic identifier.
     * @return `true` when subscribed successfully, otherwise `false`.
     */
    Q_INVOKABLE bool subscribe(const QString &contentTopic) override;

    /**
     * @brief Unsubscribes from the supplied content topic.
     * @param contentTopic Topic identifier.
     * @return `true` when unsubscribed successfully, otherwise `false`.
     */
    Q_INVOKABLE bool unsubscribe(const QString &contentTopic) override;
    Q_INVOKABLE QString getAvailableNodeInfoIDs() override;

    /**
     * @brief Semantic version of this plugin implementation.
     * @param nodeInfoId Identifier for the requested node info item.
     * @return UTF-16 string containing UTF-8 serializable JSON data, or an empty string on error.
     */
    Q_INVOKABLE QString getNodeInfo(const QString &nodeInfoId) override;

    /**
     * @brief Information about the available configuration parameters to be used in `createNode`.
     */
    Q_INVOKABLE QString getAvailableConfigs() override;

    QString name() const override { return "delivery_module"; }

    QString version() const;

    /**
     * @brief Injects/replaces the Logos API bridge used for event forwarding.
     *
     * Ownership is transferred to this plugin instance.
     *
     * @param logosAPIInstance Heap-allocated API object or `nullptr`.
     */
    Q_INVOKABLE void initLogos(LogosAPI* logosAPIInstance);

signals:
    /**
     * @brief Module event signal (currently retained for compatibility).
     * @param eventName Event identifier.
     * @param data Event payload as positional values.
     */
    void eventResponse(const QString& eventName, const QVariantList& data);

private:
    /**
     * @brief Opaque liblogosdelivery context pointer.
     */
    void* deliveryCtx;

    /**
     * @brief Serializes node creation to a single in-flight operation.
     */
    std::mutex createNodeMutex;
    
    /**
     * @brief Common timeout for FFI operations that complete via callback.
     */
    static constexpr std::chrono::seconds CALLBACK_TIMEOUT{30};
    
    /**
     * @brief Forwards normalized events to the registered Logos API client.
     * @param eventName Canonical event name.
     * @param data Event payload list.
     */
    void emitEvent(const QString& eventName, const QVariantList& data);
    
    /**
     * @brief Global C callback used by liblogosdelivery to report async events.
     *
     * Expected event payload format is a JSON document containing an `eventType`
     * discriminator and event-specific fields.
     *
     * @param callerRet FFI return code associated with callback dispatch.
     * @param msg UTF-8 JSON event payload buffer.
     * @param len Message length in bytes.
     * @param userData Opaque pointer expected to be `DeliveryModulePlugin*`.
     */
    static void event_callback(int callerRet, const char* msg, size_t len, void* userData);
};
