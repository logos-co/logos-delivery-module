#pragma once

#include <QtCore/QObject>
#include "interface.h"
#include "QExpected.h"

class DeliveryModuleInterface : public PluginInterface
{
public:
    virtual ~DeliveryModuleInterface() {}
    Q_INVOKABLE virtual bool createNode(const QString &cfg) = 0;
    Q_INVOKABLE virtual bool start() = 0;
    Q_INVOKABLE virtual bool stop() = 0;
    Q_INVOKABLE virtual QExpected<QString> send(const QString &contentTopic, const QString &payload) = 0;
    Q_INVOKABLE virtual bool subscribe(const QString &contentTopic) = 0;
    Q_INVOKABLE virtual bool unsubscribe(const QString &contentTopic) = 0;

signals:
    // for now this is required for events, later it might not be necessary if using a proxy
    void eventResponse(const QString& eventName, const QVariantList& data);
};

#define DeliveryModuleInterface_iid "org.logos.DeliveryModuleInterface"
Q_DECLARE_INTERFACE(DeliveryModuleInterface, DeliveryModuleInterface_iid)
