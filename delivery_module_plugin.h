#pragma once

#include <QtCore/QObject>
#include <chrono>
#include "delivery_module_interface.h"
#include "logos_api.h"
#include "logos_api_client.h"

class DeliveryModulePlugin : public QObject, public DeliveryModuleInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID DeliveryModuleInterface_iid FILE "metadata.json")
    Q_INTERFACES(DeliveryModuleInterface PluginInterface)

public:
    DeliveryModulePlugin();
    virtual ~DeliveryModulePlugin();

    Q_INVOKABLE bool createNode(const QString &cfg) override;
    Q_INVOKABLE bool start() override;
    Q_INVOKABLE bool stop() override;
    Q_INVOKABLE QExpected<QString> send(const QString &contentTopic, const QString &payload) override;
    Q_INVOKABLE bool subscribe(const QString &contentTopic) override;
    Q_INVOKABLE bool unsubscribe(const QString &contentTopic) override;
    QString name() const override { return "delivery_module"; }
    QString version() const override { return "1.0.0"; }

    // LogosAPI initialization
    Q_INVOKABLE void initLogos(LogosAPI* logosAPIInstance);

signals:
    // for now this is required for events, later it might not be necessary if using a proxy
    void eventResponse(const QString& eventName, const QVariantList& data);

private:
    void* deliveryCtx;
    
    // Timeout for callback operations
    static constexpr std::chrono::seconds CALLBACK_TIMEOUT{30};
    
    // Helper method for emitting events
    void emitEvent(const QString& eventName, const QVariantList& data);
    
    // Static callback functions for liblogosdelivery
    static void event_callback(int callerRet, const char* msg, size_t len, void* userData);
};
