#include "delivery_module_plugin.h"
#include <QDebug>
#include <QVariantList>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <semaphore>

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

bool DeliveryModulePlugin::createNode(const QString &cfg)
{
    qDebug() << "DeliveryModulePlugin::createNode called with cfg:" << cfg;
    
    // Convert QString to UTF-8 byte array
    QByteArray cfgUtf8 = cfg.toUtf8();
    
    // Create semaphore and callback context for synchronous operation
    // Callback is only called in failure case
    struct CallbackContext {
        std::binary_semaphore* sem;
        bool callbackInvoked;
    };
    
    std::binary_semaphore sem(0);
    CallbackContext ctx{&sem, false};
    
    // Lambda callback that will be called only on failure (when deliveryCtx is nullptr)
    auto callback = +[](int callerRet, const char* msg, size_t len, void* userData) {
        qDebug() << "DeliveryModulePlugin::createNode callback called with ret:" << callerRet;
        
        CallbackContext* ctx = static_cast<CallbackContext*>(userData);
        if (!ctx) {
            qWarning() << "DeliveryModulePlugin::createNode callback: Invalid userData";
            return;
        }
        
        if (msg && len > 0) {
            QString message = QString::fromUtf8(msg, len);
            qDebug() << "DeliveryModulePlugin::createNode callback message:" << message;
        }
        
        ctx->callbackInvoked = true;
        
        // Release semaphore to unblock the createNode method
        ctx->sem->release();
    };
    
    // Call logosdelivery_create_node with the configuration
    // Important: Keep deliveryCtx assignment from the call
    deliveryCtx = logosdelivery_create_node(cfgUtf8.constData(), callback, &ctx);
    
    // If deliveryCtx is nullptr, callback will be invoked with error details
    if (!deliveryCtx) {
        qDebug() << "DeliveryModulePlugin: Waiting for createNode error callback...";
        
        // Wait for callback to complete with timeout
        if (!sem.try_acquire_for(CALLBACK_TIMEOUT)) {
            qWarning() << "DeliveryModulePlugin: Timeout waiting for createNode callback";
            return false;
        }
        
        qWarning() << "DeliveryModulePlugin: Failed to create Messaging context";
        return false;
    }
    
    // Success case - deliveryCtx is valid, callback won't be called
    qDebug() << "DeliveryModulePlugin: Messaging context created successfully";
    
    // Set up event callback
    logosdelivery_set_event_callback(deliveryCtx, event_callback, this);
    return true;
}

bool DeliveryModulePlugin::start()
{
    qDebug() << "DeliveryModulePlugin::start called";
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot start Messaging - context not initialized. Call createNode first.";
        return false;
    }
    
    auto outcome = callApiRetVoid(
        "start",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_start_node, deliveryCtx));

    if (outcome.isErr()) {
        qWarning() << "DeliveryModulePlugin: Start failed:" << outcome.error();
        return false;
    }

    qDebug() << "DeliveryModulePlugin: Messaging start completed with success: true";
    return true;
}

bool DeliveryModulePlugin::stop()
{
    qDebug() << "DeliveryModulePlugin::stop called";
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot stop Messaging - context not initialized.";
        return false;
    }
    
    auto outcome = callApiRetVoid(
        "stop",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_stop_node, deliveryCtx));

    if (outcome.isErr()) {
        qWarning() << "DeliveryModulePlugin: Stop failed:" << outcome.error();
        return false;
    }

    qDebug() << "DeliveryModulePlugin: Messaging stop completed with success: true";
    return true;
}
QExpected<QString> DeliveryModulePlugin::send(const QString &contentTopic, const QString &payload)
{
    qDebug() << "DeliveryModulePlugin::send called with contentTopic:" << contentTopic;
    qDebug() << "DeliveryModulePlugin::send payload:" << payload;
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot send message - context not initialized. Call createNode first.";
        return QExpected<QString>::err("Context not initialized");
    }
    
    // Construct JSON message according to logosdelivery_send API
    // The payload should be base64-encoded as per the API spec
    QJsonObject messageObj;
    messageObj["contentTopic"] = contentTopic;
    messageObj["payload"] = QString::fromUtf8(payload.toUtf8().toBase64());
    messageObj["ephemeral"] = false;
    
    QJsonDocument doc(messageObj);
    QByteArray messageJson = doc.toJson(QJsonDocument::Compact);
    
    auto outcome = callApiRetValue<QString>(
        "send",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_send, deliveryCtx, messageJson.constData()));

    if (outcome.isErr()) {
        qWarning() << "DeliveryModulePlugin: Send failed for topic:" << contentTopic << ", reason:" << outcome.error();
        return QExpected<QString>::err(outcome.error());
    }

    const QString responseMessage = outcome.value();
    qDebug() << "DeliveryModulePlugin: Send initiated for topic:" << contentTopic << ", with success: true";
    return QExpected<QString>::ok(responseMessage);
}

bool DeliveryModulePlugin::subscribe(const QString &contentTopic)
{
    qDebug() << "DeliveryModulePlugin::subscribe called with contentTopic:" << contentTopic;
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot subscribe - context not initialized. Call createNode first.";
        return false;
    }
    
    // Convert QString to UTF-8 byte array
    QByteArray topicUtf8 = contentTopic.toUtf8();
    
    auto outcome = callApiRetVoid(
        "subscribe",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_subscribe, deliveryCtx, topicUtf8.constData()));

    if (outcome.isErr()) {
        qWarning() << "DeliveryModulePlugin: Subscribe failed for topic:" << contentTopic << ", reason:" << outcome.error();
        return false;
    }

    qDebug() << "DeliveryModulePlugin: Subscribe completed for topic:" << contentTopic << " with success: true";
    return true;
}

bool DeliveryModulePlugin::unsubscribe(const QString &contentTopic)
{
    qDebug() << "DeliveryModulePlugin::unsubscribe called with contentTopic:" << contentTopic;
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot unsubscribe - context not initialized.";
        return false;
    }
    
    // Convert QString to UTF-8 byte array
    QByteArray topicUtf8 = contentTopic.toUtf8();
    
    auto outcome = callApiRetVoid(
        "unsubscribe",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_unsubscribe, deliveryCtx, topicUtf8.constData()));

    if (outcome.isErr()) {
        qWarning() << "DeliveryModulePlugin: Unsubscribe failed for topic:" << contentTopic << ", reason:" << outcome.error();
        return false;
    }

    qDebug() << "DeliveryModulePlugin: Unsubscribe completed for topic:" << contentTopic << " with success: true";
    return true;
}
