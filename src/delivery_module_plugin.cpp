#include "delivery_module_plugin.h"
#include <QDebug>
#include <QVariantList>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <memory>
#include <mutex>
#include <semaphore>
#include <unordered_map>

#include "api_call_handler.h"
// Include the liblogosdelivery header from logos-delivery
// liblogosdelivery provides a high-level message-delivery API
extern "C" {
#include <liblogosdelivery.h>
}

DeliveryModulePlugin::DeliveryModulePlugin() : deliveryCtx(nullptr)
{
    qDebug() << "DeliveryModulePlugin: Initializing...";
    qDebug() << "DeliveryModulePlugin: Initialized successfully";
}

DeliveryModulePlugin::~DeliveryModulePlugin() 
{
    // Clean up resources, this is not done in PluginInterface destructor
    if (logosAPI) {
        delete logosAPI;
        logosAPI = nullptr;
    }
    
    // Clean up delivery context if it exists
    if (deliveryCtx) {
        logosdelivery_destroy(deliveryCtx, nullptr, nullptr);
        deliveryCtx = nullptr;
    }
}

void DeliveryModulePlugin::emitEvent(const QString& eventName, const QVariantList& data) {
    if (!logosAPI) {
        qWarning() << "DeliveryModulePlugin: LogosAPI not available, cannot emit" << eventName;
        return;
    }

    LogosAPIClient* client = logosAPI->getClient("delivery_module");
    if (!client) {
        qWarning() << "DeliveryModulePlugin: Failed to get delivery_module client for event" << eventName;
        return;
    }

    client->onEventResponse(this, eventName, data);
}

// Static callback function for liblogosdelivery events, this one is one time registered
// on initialization and will be called for all events from the Nim FFI side.
void DeliveryModulePlugin::event_callback(int callerRet, const char* msg, size_t len, void* userData)
{
    qDebug() << "DeliveryModulePlugin::event_callback called with ret:" << callerRet;

    DeliveryModulePlugin* plugin = static_cast<DeliveryModulePlugin*>(userData);
    if (!plugin) {
        qWarning() << "DeliveryModulePlugin::event_callback: Invalid userData";
        return;
    }

    if (msg && len > 0) {
        QString message = QString::fromUtf8(msg, len);
        qDebug() << "DeliveryModulePlugin::event_callback message:" << message;
        
        // Parse JSON to determine event type
        QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
        if (!doc.isObject()) {
            qWarning() << "DeliveryModulePlugin::event_callback: Invalid JSON";
            return;
        }
        
        QJsonObject jsonObj = doc.object();
        QString eventType = jsonObj["eventType"].toString();
        QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
        
        if (eventType == "message_sent") {
            // MessageSentEvent: requestId, messageHash
            QVariantList eventData;
            eventData << jsonObj["requestId"].toString();
            eventData << jsonObj["messageHash"].toString();
            eventData << timestamp;
            plugin->emitEvent("messageSent", eventData);
            
        } else if (eventType == "message_error") {
            // MessageErrorEvent: requestId, messageHash, error
            QVariantList eventData;
            eventData << jsonObj["requestId"].toString();
            eventData << jsonObj["messageHash"].toString();
            eventData << jsonObj["error"].toString();
            eventData << timestamp;
            plugin->emitEvent("messageError", eventData);
            
        } else if (eventType == "message_propagated") {
            // MessagePropagatedEvent: requestId, messageHash
            QVariantList eventData;
            eventData << jsonObj["requestId"].toString();
            eventData << jsonObj["messageHash"].toString();
            eventData << timestamp;
            plugin->emitEvent("messagePropagated", eventData);
            
        } else if (eventType == "message_received") {
            // MessageReceivedEvent: messageHash, message (WakuMessage)
            QJsonObject msgObj = jsonObj["message"].toObject();
            QVariantList eventData;
            eventData << jsonObj["messageHash"].toString();
            eventData << msgObj["contentTopic"].toString();

            QJsonValue payloadValue = msgObj["payload"];
            if (payloadValue.isArray()) {
                QJsonArray payloadArray = payloadValue.toArray();
                QByteArray payloadBytes;
                payloadBytes.reserve(payloadArray.size());
                for (const QJsonValue& val : payloadArray) {
                    payloadBytes.append(static_cast<char>(val.toInt()));
                }
                eventData << payloadBytes;
            } else {
                eventData << QByteArray::fromBase64(payloadValue.toString().toLatin1());
            }

            eventData << QString::number(msgObj["timestamp"].toDouble(), 'f', 0);
            plugin->emitEvent("messageReceived", eventData);

        } else if (eventType == "connection_status_change") {
            QVariantList eventData;
            eventData << jsonObj["connectionStatus"].toString();
            eventData << timestamp;
            plugin->emitEvent("connectionStateChanged", eventData);
            
        } else {
            qWarning() << "DeliveryModulePlugin::event_callback: Unknown event type:" << eventType;
        }
    }
}

void DeliveryModulePlugin::initLogos(LogosAPI* logosAPIInstance) {
    if (logosAPI) {
        delete logosAPI;
    }
    logosAPI = logosAPIInstance;
}

LogosResult DeliveryModulePlugin::createNode(const QString &cfg)
{
    std::lock_guard<std::mutex> createNodeLock(createNodeMutex);

    if (deliveryCtx != nullptr) {
        qWarning() << "DeliveryModulePlugin: createNode rejected - context already initialized";
        return {false, QVariant(), QStringLiteral("Context not initialized")};
    }

    qDebug() << "DeliveryModulePlugin::createNode called with cfg:" << cfg;
    
    // Convert QString to UTF-8 byte array
    QByteArray cfgUtf8 = cfg.toUtf8();
    
    // Create callback context for synchronous createNode result.
    // The context is kept in a pending map so late callbacks can be safely ignored.
    struct CallbackContext {
        std::binary_semaphore sem{0};
        int callerRet{RET_ERR};
        QString message;
    };

    static std::mutex pendingMutex;
    static std::unordered_map<void*, std::shared_ptr<CallbackContext>> pendingContexts;

    auto callbackCtx = std::make_shared<CallbackContext>();
    void* callbackKey = static_cast<void*>(callbackCtx.get());

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts[callbackKey] = callbackCtx;
    }
    
    // Callback is expected in both success and error cases.
    auto callback = +[](int callerRet, const char* msg, size_t len, void* userData) {
        qDebug() << "DeliveryModulePlugin::createNode callback called with ret:" << callerRet;

        std::shared_ptr<CallbackContext> callbackCtx;
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            auto it = pendingContexts.find(userData);
            if (it == pendingContexts.end()) {
                return;
            }
            callbackCtx = it->second;
            pendingContexts.erase(it);
        }

        if (!callbackCtx) {
            return;
        }

        callbackCtx->callerRet = callerRet;
        if (msg && len > 0) {
            callbackCtx->message = QString::fromUtf8(msg, len);
            qDebug() << "DeliveryModulePlugin::createNode callback message:" << callbackCtx->message;
        }

        // Release semaphore to unblock the createNode method
        callbackCtx->sem.release();
    };
    
    // Call logosdelivery_create_node with the configuration
    // Important: Keep deliveryCtx assignment from the call,
    // creating of the context is immediate and not depends on the callback.
    deliveryCtx = logosdelivery_create_node(cfgUtf8.constData(), callback, callbackKey);

    qDebug() << "DeliveryModulePlugin: Waiting for createNode callback...";

    // Wait for callback result regardless of immediate pointer value.
    // Callback ensures that the underlying node object is properly created.
    if (!callbackCtx->sem.try_acquire_for(CALLBACK_TIMEOUT)) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);

        deliveryCtx = nullptr;

        qWarning() << "DeliveryModulePlugin: Timeout waiting for createNode callback";
        return {false, QVariant(), QStringLiteral("Timeout waiting for createNode callback")};
    }

    // Any issue happened during node creation means the context is destroyed and must not be user.
    if (callbackCtx->callerRet != RET_OK || deliveryCtx == nullptr) {
        if (!callbackCtx->message.isEmpty()) {
            qWarning() << "DeliveryModulePlugin: createNode callback error:" << callbackCtx->message;
        }

        deliveryCtx = nullptr;

        qWarning() << "DeliveryModulePlugin: Failed to create Delivery context";
        return {false, QVariant(), QStringLiteral("Failed to create Delivery context")};
    }
    
    // Success case - deliveryCtx is valid and callback returned RET_OK.
    qDebug() << "DeliveryModulePlugin: Delivery context created successfully";
    
    // Set up event callback
    logosdelivery_set_event_callback(deliveryCtx, event_callback, this);
    return {true, {}};
}

LogosResult DeliveryModulePlugin::start()
{
    qDebug() << "DeliveryModulePlugin::start called";
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot start Delivery - context not initialized. Call createNode first.";
        return {false, QVariant(), QStringLiteral("Context not initialized")};
    }
    
    auto outcome = callApiRetVoid(
        "start",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_start_node, deliveryCtx));

    if (!outcome.success) {
        qWarning() << "DeliveryModulePlugin: Start failed:" << outcome.getError();
    }

    qDebug() << "DeliveryModulePlugin: Delivery start completed with success";
    return outcome;
}

LogosResult DeliveryModulePlugin::stop()
{
    qDebug() << "DeliveryModulePlugin::stop called";
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot stop Delivery - context not initialized. Call createNode first.";
        return {false, QVariant(), QStringLiteral("Context not initialized")};
    }
    
    auto outcome = callApiRetVoid(
        "stop",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_stop_node, deliveryCtx));

    if (!outcome.success) {
        qWarning() << "DeliveryModulePlugin: Stop failed:" << outcome.getError();
    }

    qDebug() << "DeliveryModulePlugin: Delivery stop completed with success";
    return outcome;
}
LogosResult DeliveryModulePlugin::send(const QString &contentTopic, const QByteArray &payload)
{
    qDebug() << "DeliveryModulePlugin::send called with contentTopic:" << contentTopic;

    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot send message - context not initialized. Call createNode first.";
        return {false, QVariant(), QStringLiteral("Context not initialized")};
    }

    // Construct JSON message according to logosdelivery_send API
    // The payload should be base64-encoded as per the API spec
    QJsonObject messageObj;
    messageObj["contentTopic"] = contentTopic;
    messageObj["payload"] = QString::fromUtf8(payload.toBase64());
    messageObj["ephemeral"] = false;

    QJsonDocument doc(messageObj);
    QByteArray messageJson = doc.toJson(QJsonDocument::Compact);

    auto outcome = callApiRetValue(
        "send",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_send, deliveryCtx, messageJson.constData()));

    if (!outcome.success) {
        qWarning() << "DeliveryModulePlugin: Send failed for topic:" << contentTopic << ", reason:" << outcome.getError();
    }

    const QString responseMessage = outcome.getString();
    qDebug() << "DeliveryModulePlugin: Send initiated for topic:" << contentTopic << ", with success, requestId: " << responseMessage;
    return outcome;
}

LogosResult DeliveryModulePlugin::subscribe(const QString &contentTopic)
{
    qDebug() << "DeliveryModulePlugin::subscribe called with contentTopic:" << contentTopic;
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot subscribe - context not initialized. Call createNode first.";
        return {false, QVariant(), QStringLiteral("Context not initialized")};
    }
    
    // Convert QString to UTF-8 byte array
    QByteArray topicUtf8 = contentTopic.toUtf8();
    
    auto outcome = callApiRetVoid(
        "subscribe",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_subscribe, deliveryCtx, topicUtf8.constData()));

    if (!outcome.success) {
        qWarning() << "DeliveryModulePlugin: Subscribe failed for topic:" << contentTopic << ", reason:" << outcome.getError();
    }

    qDebug() << "DeliveryModulePlugin: Subscribe completed for topic:" << contentTopic << " with success";
    return outcome;
}

LogosResult DeliveryModulePlugin::unsubscribe(const QString &contentTopic)
{
    qDebug() << "DeliveryModulePlugin::unsubscribe called with contentTopic:" << contentTopic;
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot unsubscribe - context not initialized.";
        return {false, QVariant(), QStringLiteral("Context not initialized")};
    }
    
    // Convert QString to UTF-8 byte array
    QByteArray topicUtf8 = contentTopic.toUtf8();
    
    auto outcome = callApiRetVoid(
        "unsubscribe",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_unsubscribe, deliveryCtx, topicUtf8.constData()));

    if (!outcome.success) {
        qWarning() << "DeliveryModulePlugin: Unsubscribe failed for topic:" << contentTopic << ", reason:" << outcome.getError();
    }

    qDebug() << "DeliveryModulePlugin: Unsubscribe completed for topic:" << contentTopic << " with success";
    return outcome;
}

QString DeliveryModulePlugin::version() const {
    QString moduleVersion = "1.1.0";
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot subscribe - context not initialized. Call createNode first.";
        return moduleVersion + " (liblogosdelivery version unknown, context not initialized)";
    }

    auto attributeName = "Version";
    auto liblogosDeliveryVersion = callApiRetValue(
        "get_node_info",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_node_info, deliveryCtx, attributeName));

    if (!liblogosDeliveryVersion.success) {
        qWarning() << "DeliveryModulePlugin: Get node info failed getting version, reason:" <<
            liblogosDeliveryVersion.getError();
        return moduleVersion + " (liblogosdelivery version unknown)";
    }

    const QString version = liblogosDeliveryVersion.getString();
    qDebug() << "DeliveryModulePlugin: Get node info completed for attribute:" <<
        attributeName << ", with success: " << version;

    return moduleVersion + " (liblogosdelivery version: " + version + ")";
}

LogosResult DeliveryModulePlugin::getAvailableNodeInfoIDs() {
    
    qDebug() << "DeliveryModulePlugin::getAvailableNodeInfoIDs called";

    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot get available node info IDs - context not initialized. Call createNode first.";
        return {false, QVariant(), QStringLiteral("Context not initialized")};
    }
    auto outcome = callApiRetValue(
        "get_available_node_info_ids",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_available_node_info_ids, deliveryCtx));

    if (!outcome.success) {
        qWarning() << "DeliveryModulePlugin: Get available node info IDs failed, reason:" << outcome.getError();
    }
    return outcome;
}

LogosResult DeliveryModulePlugin::getNodeInfo(const QString &nodeInfoId) {
    qDebug() << "DeliveryModulePlugin::getNodeInfo called with nodeInfoId:" << nodeInfoId;

    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot get node info - context not initialized. Call createNode first.";
        return {false, QVariant(), QStringLiteral("Context not initialized")};
    }
    auto outcome = callApiRetValue(
        "get_node_info",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_node_info, deliveryCtx, nodeInfoId.toUtf8().constData()));

    if (!outcome.success) {
        qWarning() << "DeliveryModulePlugin: Get node info failed for ID:" << nodeInfoId <<
            ", reason:" << outcome.getError();
    }

    return outcome;
}

LogosResult DeliveryModulePlugin::getAvailableConfigs() {
    qDebug() << "DeliveryModulePlugin::getAvailableConfigs called";

    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot get available configs - context not initialized. Call createNode first.";
        return {false, QVariant(), QStringLiteral("Context not initialized")};
    }
    auto outcome = callApiRetValue(
        "get_available_configs",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_available_configs, deliveryCtx));

    if (!outcome.success) {
        qWarning() << "DeliveryModulePlugin: Get available configs failed, reason:" << outcome.getError();
    }

    return outcome;
}
