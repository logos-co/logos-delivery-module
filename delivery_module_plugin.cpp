#include "delivery_module_plugin.h"
#include <QDebug>
#include <QCoreApplication>
#include <QVariantList>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

// Include the liblogosdelivery header from logos-messaging-nim
// liblogosdelivery provides a high-level message-delivery API
extern "C" {
#include <liblogosdelivery.h>
}

// Return codes (similar to libwaku)
#define RET_OK 0
#define RET_ERR 1
#define RET_MISSING_CALLBACK 2

DeliveryModulePlugin::DeliveryModulePlugin() : deliveryCtx(nullptr)
{
    qDebug() << "DeliveryModulePlugin: Initializing...";
    qDebug() << "DeliveryModulePlugin: Initialized successfully";
}

DeliveryModulePlugin::~DeliveryModulePlugin() 
{
    // Clean up resources
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

void DeliveryModulePlugin::init_callback(int callerRet, const char* msg, size_t len, void* userData)
{
    qDebug() << "DeliveryModulePlugin::init_callback called with ret:" << callerRet;
    if (msg && len > 0) {
        QString message = QString::fromUtf8(msg, len);
        qDebug() << "DeliveryModulePlugin::init_callback message:" << message;
    }
}

void DeliveryModulePlugin::start_callback(int callerRet, const char* msg, size_t len, void* userData)
{
    qDebug() << "DeliveryModulePlugin::start_callback called with ret:" << callerRet;
    if (msg && len > 0) {
        QString message = QString::fromUtf8(msg, len);
        qDebug() << "DeliveryModulePlugin::start_callback message:" << message;
    }
}

void DeliveryModulePlugin::stop_callback(int callerRet, const char* msg, size_t len, void* userData)
{
    qDebug() << "DeliveryModulePlugin::stop_callback called with ret:" << callerRet;
    if (msg && len > 0) {
        QString message = QString::fromUtf8(msg, len);
        qDebug() << "DeliveryModulePlugin::stop_callback message:" << message;
    }
}

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
        
        // Create event data with the message
        QVariantList eventData;
        eventData << message;
        eventData << QDateTime::currentDateTime().toString(Qt::ISODate);

        // Trigger event using emitEvent helper
        plugin->emitEvent("messageReceived", eventData);
    }
}

void DeliveryModulePlugin::send_message_callback(int callerRet, const char* msg, size_t len, void* userData)
{
    qDebug() << "DeliveryModulePlugin::send_message_callback called with ret:" << callerRet;

    DeliveryModulePlugin* plugin = static_cast<DeliveryModulePlugin*>(userData);
    if (!plugin) {
        qWarning() << "DeliveryModulePlugin::send_message_callback: Invalid userData";
        return;
    }

    if (msg && len > 0) {
        QString message = QString::fromUtf8(msg, len);
        qDebug() << "DeliveryModulePlugin::send_message_callback message:" << message;

        // Create event data with the send result
        QVariantList eventData;
        eventData << message;
        eventData << QDateTime::currentDateTime().toString(Qt::ISODate);

        if (callerRet == RET_OK) {
            plugin->emitEvent("messageSent", eventData);
        } else {
            plugin->emitEvent("messageError", eventData);
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
    
    // Call logosdelivery_create_node with the configuration
    deliveryCtx = logosdelivery_create_node(cfgUtf8.constData(), init_callback, this);
    
    if (deliveryCtx) {
        qDebug() << "DeliveryModulePlugin: Messaging context created successfully";
        
        // Set up event callback
        logosdelivery_set_event_callback(deliveryCtx, event_callback, this);
        
        // Emit initialization event
        QVariantList eventData;
        eventData << "success";
        eventData << QDateTime::currentDateTime().toString(Qt::ISODate);
        emitEvent("deliveryInitialized", eventData);
        
        return true;
    } else {
        qWarning() << "DeliveryModulePlugin: Failed to create Messaging context";
        
        // Emit error event
        QVariantList eventData;
        eventData << "error";
        eventData << QDateTime::currentDateTime().toString(Qt::ISODate);
        emitEvent("deliveryInitialized", eventData);
        
        return false;
    }
}

bool DeliveryModulePlugin::start()
{
    qDebug() << "DeliveryModulePlugin::start called";
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot start Messaging - context not initialized. Call createNode first.";
        return false;
    }
    
    // Call logosdelivery_start_node with the saved context
    int result = logosdelivery_start_node(deliveryCtx, start_callback, this);
    
    if (result == RET_OK) {
        qDebug() << "DeliveryModulePlugin: Messaging start initiated successfully";
        
        // Emit start event
        QVariantList eventData;
        eventData << "success";
        eventData << QDateTime::currentDateTime().toString(Qt::ISODate);
        emitEvent("deliveryStarted", eventData);
        
        return true;
    } else {
        qWarning() << "DeliveryModulePlugin: Failed to start Messaging, error code:" << result;
        return false;
    }
}

bool DeliveryModulePlugin::stop()
{
    qDebug() << "DeliveryModulePlugin::stop called";
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot stop Messaging - context not initialized.";
        return false;
    }
    
    // Call logosdelivery_stop_node
    int result = logosdelivery_stop_node(deliveryCtx, stop_callback, this);
    
    if (result == RET_OK) {
        qDebug() << "DeliveryModulePlugin: Messaging stop initiated successfully";
        return true;
    } else {
        qWarning() << "DeliveryModulePlugin: Failed to stop Messaging, error code:" << result;
        return false;
    }
}

bool DeliveryModulePlugin::send(const QString &contentTopic, const QString &payload)
{
    qDebug() << "DeliveryModulePlugin::send called with contentTopic:" << contentTopic;
    qDebug() << "DeliveryModulePlugin::send payload:" << payload;
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot send message - context not initialized. Call createNode first.";
        return false;
    }
    
    // Construct JSON message according to logosdelivery_send API
    // The payload should be base64-encoded as per the API spec
    QJsonObject messageObj;
    messageObj["contentTopic"] = contentTopic;
    messageObj["payload"] = QString::fromUtf8(payload.toUtf8().toBase64());
    messageObj["ephemeral"] = false;
    
    QJsonDocument doc(messageObj);
    QByteArray messageJson = doc.toJson(QJsonDocument::Compact);
    
    // Call logosdelivery_send with JSON message
    int result = logosdelivery_send(deliveryCtx, send_message_callback, this, messageJson.constData());
    
    if (result == RET_OK) {
        qDebug() << "DeliveryModulePlugin: Send message initiated successfully for topic:" << contentTopic;
        return true;
    } else {
        qWarning() << "DeliveryModulePlugin: Failed to send message to topic:" << contentTopic << ", error code:" << result;
        return false;
    }
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
    
    // Call logosdelivery_subscribe (signature: ctx, callback, userData, contentTopic)
    int result = logosdelivery_subscribe(deliveryCtx, nullptr, this, topicUtf8.constData());
    
    if (result == RET_OK) {
        qDebug() << "DeliveryModulePlugin: Subscribe initiated successfully for topic:" << contentTopic;
        return true;
    } else {
        qWarning() << "DeliveryModulePlugin: Failed to subscribe to topic:" << contentTopic << ", error code:" << result;
        return false;
    }
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
    
    // Call logosdelivery_unsubscribe (signature: ctx, callback, userData, contentTopic)
    int result = logosdelivery_unsubscribe(deliveryCtx, nullptr, this, topicUtf8.constData());
    
    if (result == RET_OK) {
        qDebug() << "DeliveryModulePlugin: Unsubscribe initiated successfully for topic:" << contentTopic;
        return true;
    } else {
        qWarning() << "DeliveryModulePlugin: Failed to unsubscribe from topic:" << contentTopic << ", error code:" << result;
        return false;
    }
}
