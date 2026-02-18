#include "delivery_module_plugin.h"
#include <QDebug>
#include <QCoreApplication>
#include <QVariantList>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <semaphore>

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
        if (!sem.try_acquire_for(std::chrono::seconds(CALLBACK_TIMEOUT_SECONDS))) {
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
    
    // Create semaphore and callback context for synchronous operation
    struct CallbackContext {
        std::binary_semaphore* sem;
        bool success;
    };
    
    std::binary_semaphore sem(0);
    CallbackContext ctx{&sem, false};
    
    // Lambda callback that will be called when start completes
    auto callback = +[](int callerRet, const char* msg, size_t len, void* userData) {
        qDebug() << "DeliveryModulePlugin::start callback called with ret:" << callerRet;
        
        CallbackContext* ctx = static_cast<CallbackContext*>(userData);
        if (!ctx) {
            qWarning() << "DeliveryModulePlugin::start callback: Invalid userData";
            return;
        }
        
        if (msg && len > 0) {
            QString message = QString::fromUtf8(msg, len);
            qDebug() << "DeliveryModulePlugin::start callback message:" << message;
        }
        
        ctx->success = callerRet == RET_OK;
        
        // Release semaphore to unblock the start method
        ctx->sem->release();
    };
    
    // Call logosdelivery_start_node with the saved context
    int result = logosdelivery_start_node(deliveryCtx, callback, &ctx);
    
    if (result != RET_OK) {
        qWarning() << "DeliveryModulePlugin: Failed to initiate start, error code:" << result;
        return false;
    }
    
    qDebug() << "DeliveryModulePlugin: Waiting for start callback...";
    
    // Wait for callback to complete with timeout
    if (!sem.try_acquire_for(std::chrono::seconds(CALLBACK_TIMEOUT_SECONDS))) {
        qWarning() << "DeliveryModulePlugin: Timeout waiting for start callback";
        return false;
    }
    
    qDebug() << "DeliveryModulePlugin: Messaging start completed with success:" << ctx.success;
    return ctx.success;
}

bool DeliveryModulePlugin::stop()
{
    qDebug() << "DeliveryModulePlugin::stop called";
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot stop Messaging - context not initialized.";
        return false;
    }
    
    // Create semaphore and callback context for synchronous operation
    struct CallbackContext {
        std::binary_semaphore* sem;
        bool success;
    };
    
    std::binary_semaphore sem(0);
    CallbackContext ctx{&sem, false};
    
    // Lambda callback that will be called when stop completes
    auto callback = +[](int callerRet, const char* msg, size_t len, void* userData) {
        qDebug() << "DeliveryModulePlugin::stop callback called with ret:" << callerRet;
        
        CallbackContext* ctx = static_cast<CallbackContext*>(userData);
        if (!ctx) {
            qWarning() << "DeliveryModulePlugin::stop callback: Invalid userData";
            return;
        }
        
        if (msg && len > 0) {
            QString message = QString::fromUtf8(msg, len);
            qDebug() << "DeliveryModulePlugin::stop callback message:" << message;
        }
        
        ctx->success = callerRet == RET_OK;
        
        // Release semaphore to unblock the stop method
        ctx->sem->release();
    };
    
    // Call logosdelivery_stop_node
    int result = logosdelivery_stop_node(deliveryCtx, callback, &ctx);
    
    if (result != RET_OK) {
        qWarning() << "DeliveryModulePlugin: Failed to initiate stop, error code:" << result;
        return false;
    }
    
    qDebug() << "DeliveryModulePlugin: Waiting for stop callback...";
    
    // Wait for callback to complete with timeout
    if (!sem.try_acquire_for(std::chrono::seconds(CALLBACK_TIMEOUT_SECONDS))) {
        qWarning() << "DeliveryModulePlugin: Timeout waiting for stop callback";
        return false;
    }
    
    qDebug() << "DeliveryModulePlugin: Messaging stop completed with success:" << ctx.success;
    return ctx.success;
}
bool DeliveryModulePlugin::send(const QString &contentTopic, const QString &payload, QString &requestId)
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
    
    // Create semaphore and callback context for synchronous operation
    struct CallbackContext {
        std::binary_semaphore* sem;
        QString strResult;
        bool success;
    };
    
    std::binary_semaphore sem(0);
    CallbackContext ctx{&sem, "", false};
    
    // Lambda callback that will be called when send completes
    auto callback = +[](int callerRet, const char* msg, size_t len, void* userData) {
        qDebug() << "DeliveryModulePlugin::send callback called with ret:" << callerRet;
        
        CallbackContext* ctx = static_cast<CallbackContext*>(userData);
        if (!ctx) {
            qWarning() << "DeliveryModulePlugin::send callback: Invalid userData";
            if (ctx && ctx->sem) {
                ctx->sem->release();
            }
            return;
        }
        
        if (msg && len > 0) {
            ctx->strResult = QString::fromUtf8(msg, len);
            ctx->success = callerRet == RET_OK;
            qDebug() << "DeliveryModulePlugin::send callback message (" << ctx->success << "):" << ctx->strResult;
        }
        
        // Release semaphore to unblock the send method
        ctx->sem->release();
    };
    
    // Call logosdelivery_send with JSON message
    int result = logosdelivery_send(deliveryCtx, callback, &ctx, messageJson.constData());
    
    if (result != RET_OK) {
        qWarning() << "DeliveryModulePlugin: Failed to initiate send to topic:" << contentTopic << ", error code:" << result;
        return false;
    }
    
    qDebug() << "DeliveryModulePlugin: Waiting for send callback...";
    
    // Wait for callback to complete with timeout
    if (!sem.try_acquire_for(std::chrono::seconds(CALLBACK_TIMEOUT_SECONDS))) {
        qWarning() << "DeliveryModulePlugin: Timeout waiting for send callback";
        return false;
    }
    
    if (ctx.success) {
        requestId = ctx.strResult;
    }
    qDebug() << "DeliveryModulePlugin: Send initiated for topic:" << contentTopic << ", with success:" << ctx.success;
    return ctx.success;
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
    
    // Create semaphore and callback context for synchronous operation
    struct CallbackContext {
        std::binary_semaphore* sem;
        bool success;
    };
    
    std::binary_semaphore sem(0);
    CallbackContext ctx{&sem, false};
    
    // Lambda callback that will be called when subscribe completes
    auto callback = +[](int callerRet, const char* msg, size_t len, void* userData) {
        qDebug() << "DeliveryModulePlugin::subscribe callback called with ret:" << callerRet;
        
        CallbackContext* ctx = static_cast<CallbackContext*>(userData);
        if (!ctx) {
            qWarning() << "DeliveryModulePlugin::subscribe callback: Invalid userData";
            return;
        }
        
        if (msg && len > 0) {
            QString message = QString::fromUtf8(msg, len);
            qDebug() << "DeliveryModulePlugin::subscribe callback message:" << message;
        }
        
        ctx->success = callerRet == RET_OK;
        
        // Release semaphore to unblock the subscribe method
        ctx->sem->release();
    };
    
    // Call logosdelivery_subscribe (signature: ctx, callback, userData, contentTopic)
    int result = logosdelivery_subscribe(deliveryCtx, callback, &ctx, topicUtf8.constData());
    
    if (result != RET_OK) {
        qWarning() << "DeliveryModulePlugin: Failed to initiate subscribe to topic:" << contentTopic << ", error code:" << result;
        return false;
    }
    
    qDebug() << "DeliveryModulePlugin: Waiting for subscribe callback...";
    
    // Wait for callback to complete with timeout
    if (!sem.try_acquire_for(std::chrono::seconds(CALLBACK_TIMEOUT_SECONDS))) {
        qWarning() << "DeliveryModulePlugin: Timeout waiting for subscribe callback";
        return false;
    }
    
    qDebug() << "DeliveryModulePlugin: Subscribe completed for topic:" << contentTopic << " with success:" << ctx.success;
    return ctx.success;
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
    
    // Create semaphore and callback context for synchronous operation
    struct CallbackContext {
        std::binary_semaphore* sem;
        bool success;
    };
    
    std::binary_semaphore sem(0);
    CallbackContext ctx{&sem, false};
    
    // Lambda callback that will be called when unsubscribe completes
    auto callback = +[](int callerRet, const char* msg, size_t len, void* userData) {
        qDebug() << "DeliveryModulePlugin::unsubscribe callback called with ret:" << callerRet;
        
        CallbackContext* ctx = static_cast<CallbackContext*>(userData);
        if (!ctx) {
            qWarning() << "DeliveryModulePlugin::unsubscribe callback: Invalid userData";
            return;
        }
        
        if (msg && len > 0) {
            QString message = QString::fromUtf8(msg, len);
            qDebug() << "DeliveryModulePlugin::unsubscribe callback message:" << message;
        }
        
        ctx->success = callerRet == RET_OK;
        
        // Release semaphore to unblock the unsubscribe method
        ctx->sem->release();
    };
    
    // Call logosdelivery_unsubscribe (signature: ctx, callback, userData, contentTopic)
    int result = logosdelivery_unsubscribe(deliveryCtx, callback, &ctx, topicUtf8.constData());
    
    if (result != RET_OK) {
        qWarning() << "DeliveryModulePlugin: Failed to initiate unsubscribe from topic:" << contentTopic << ", error code:" << result;
        return false;
    }
    
    qDebug() << "DeliveryModulePlugin: Waiting for unsubscribe callback...";
    
    // Wait for callback to complete with timeout
    if (!sem.try_acquire_for(std::chrono::seconds(CALLBACK_TIMEOUT_SECONDS))) {
        qWarning() << "DeliveryModulePlugin: Timeout waiting for unsubscribe callback";
        return false;
    }
    
    qDebug() << "DeliveryModulePlugin: Unsubscribe completed for topic:" << contentTopic << " with success:" << ctx.success;
    return ctx.success;
}
