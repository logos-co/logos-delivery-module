#pragma once

#include <QtCore/QObject>
#include "interface.h"
#include "logos_types.h"

class DeliveryModuleInterface : public PluginInterface
{
public:
    virtual ~DeliveryModuleInterface() {}
    Q_INVOKABLE virtual LogosResult createNode(const QString &cfg) = 0;
    Q_INVOKABLE virtual LogosResult start() = 0;
    Q_INVOKABLE virtual LogosResult stop() = 0;
    Q_INVOKABLE virtual LogosResult send(const QString &contentTopic, const QByteArray &payload) = 0;
    Q_INVOKABLE virtual LogosResult subscribe(const QString &contentTopic) = 0;
    Q_INVOKABLE virtual LogosResult unsubscribe(const QString &contentTopic) = 0;
    Q_INVOKABLE virtual LogosResult getAvailableNodeInfoIDs() = 0;
    Q_INVOKABLE virtual LogosResult getNodeInfo(const QString &nodeInfoId) = 0;
    Q_INVOKABLE virtual LogosResult getAvailableConfigs() = 0;

signals:
    void eventResponse(const QString& eventName, const QVariantList& data);
};

#define DeliveryModuleInterface_iid "org.logos.DeliveryModuleInterface"
Q_DECLARE_INTERFACE(DeliveryModuleInterface, DeliveryModuleInterface_iid)
