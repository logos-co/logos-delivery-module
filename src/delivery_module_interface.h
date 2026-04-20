#pragma once

#include <QtCore/QObject>
#include <QtCore/QVariant>
#include "interface.h"

class DeliveryModuleInterface : public PluginInterface
{
public:
    virtual ~DeliveryModuleInterface() {}
    Q_INVOKABLE virtual QVariant createNode(const QString &cfg) = 0;
    Q_INVOKABLE virtual QVariant start() = 0;
    Q_INVOKABLE virtual QVariant stop() = 0;
    Q_INVOKABLE virtual QVariant send(const QString &contentTopic, const QString &payload) = 0;
    Q_INVOKABLE virtual QVariant subscribe(const QString &contentTopic) = 0;
    Q_INVOKABLE virtual QVariant unsubscribe(const QString &contentTopic) = 0;
    Q_INVOKABLE virtual QVariant getAvailableNodeInfoIDs() = 0;
    Q_INVOKABLE virtual QVariant getNodeInfo(const QString &nodeInfoId) = 0;
    Q_INVOKABLE virtual QVariant getAvailableConfigs() = 0;

signals:
    void eventResponse(const QString& eventName, const QVariantList& data);
};

#define DeliveryModuleInterface_iid "org.logos.DeliveryModuleInterface"
Q_DECLARE_INTERFACE(DeliveryModuleInterface, DeliveryModuleInterface_iid)
