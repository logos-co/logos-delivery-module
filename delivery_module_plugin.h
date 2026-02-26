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
 * - `connectionStateChanged`
 *   - `data[0]` (`QString`): connection status
 *   - `data[1]` (`QString`): local timestamp (ISO-8601)
 *
 * The raw FFI `eventType` values mapped into these plugin events are:
 * - `message_sent` -> `messageSent`
 * - `message_error` -> `messageError`
 * - `message_propagated` -> `messagePropagated`
 * - `connection_status_change` -> `connectionStateChanged`
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
     * @brief Creates a liblogosdelivery node from a NodeConfig JSON document.
     *
     * The JSON is parsed by logos-delivery (liblogosdelivery folder) side and maps to
     * `NodeConfig` from `waku/api/api_conf.nim` (https://github.com/logos-messaging/logos-delivery).
     *
     * ## Top-level keys (`NodeConfig`)
     * - `mode` (`"Core" | "Edge"`, optional, default: `"Core"`)
     * - `protocolsConfig` (object, optional, default: network preset)
     * - `networkingConfig` (object, optional, default shown below)
     * - `ethRpcEndpoints` (array of string, optional, default: `[]`)
     * - `p2pReliability` (boolean, optional, default: `false`)
     * - `logLevel` (enum string, optional, default: `"INFO"`)
     * - `logFormat` (`"TEXT" | "JSON"`, optional, default: `"TEXT"`)
     *
     * ## `protocolsConfig` keys
     * - `entryNodes` (array of string (in formats: enrtree, multiaddress), 
     *                required when `protocolsConfig` is present)
     * - `staticStoreNodes` (array of string (in formats: enr, multiaddress), 
     *                      optional, default: `[]`)
     * - `clusterId` (number/uint16, required when `protocolsConfig` is present)
     * - `autoShardingConfig` (object, optional)
     *   - `numShardsInCluster` (number/uint16, required if object present)
     * - `messageValidation` (object, optional)
     *   - `maxMessageSize` (string, required if object present; e.g. `"150 KiB"`)
     *   - `rlnConfig` Rate Limit Nullifier configuration (object or `null`, optional, default: `null`)
     *     - `contractAddress` (string, required if object present)
     *     - `chainId` (number/uint, required if object present)
     *     - `epochSizeSec` (number/uint64, required if object present)
     *
     * ## `networkingConfig` keys
     * - `listenIpv4` (string IPv4, required if object present)
     * - `p2pTcpPort` (number/uint16, required if object present)
     * - `discv5UdpPort` (number/uint16, required if object present)
     *
     * @note Unknown keys at any level are rejected by the decoder.
     * @note Omitting `protocolsConfig` entirely is valid and uses the preset.
     * @note If `protocolsConfig` is present, both `entryNodes` and `clusterId`
     *       must be provided.
     *
     * Example:
     * @code{.json}
     * {
     *   "mode": "Core",
     *   "protocolsConfig": {
     *     "entryNodes": ["enrtree://TREE@nodes.example.com"],
     *     "staticStoreNodes": [],
     *     "clusterId": 1,
     *     "autoShardingConfig": { "numShardsInCluster": 8 },
     *     "messageValidation": {
     *       "maxMessageSize": "150 KiB",
     *       "rlnConfig": null
     *     }
     *   },
     *   "networkingConfig": {
     *     "listenIpv4": "0.0.0.0",
     *     "p2pTcpPort": 60000,
     *     "discv5UdpPort": 9000
     *   },
     *   "ethRpcEndpoints": [],
     *   "p2pReliability": false,
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

    /**
     * @brief Human-readable plugin name used by host/plugin registry.
     */
    QString name() const override { return "delivery_module"; }

    /**
     * @brief Semantic version of this plugin implementation.
     */
    QString version() const override { return "1.0.0"; }

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
