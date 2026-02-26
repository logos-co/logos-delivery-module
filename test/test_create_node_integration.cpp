#include <QtTest/QtTest>
#include <QElapsedTimer>
#include <QLibrary>

#include "delivery_module_plugin.h"

class DeliveryModulePluginCreateNodeIntegrationTest : public QObject
{
    Q_OBJECT

private:
    static const QString kDefaultConfig;
    static const QString kEdgeConfig;
    static const QString kCoreConfig;

    static bool hasLibpqDependency()
    {
        const QStringList candidates = {
            QStringLiteral("libpq"),
            QStringLiteral("libpq.dylib"),
            QStringLiteral("pq")
        };

        for (const QString& candidate : candidates) {
            QLibrary library(candidate);
            if (library.load()) {
                library.unload();
                return true;
            }
        }

        return false;
    }

private slots:
    void createNode_withDefaultConfig_succeedsOrSkips();
    void createNode_withEdgeConfig_succeedsOrSkips();
    void createNode_withCoreConfig_succeedsOrSkips();
    void createNode_secondCallRejected_afterSuccessfulInit();
    void startStop_withRealBackend_succeedsOrSkips();
};

const QString DeliveryModulePluginCreateNodeIntegrationTest::kDefaultConfig = QStringLiteral("{}");

const QString DeliveryModulePluginCreateNodeIntegrationTest::kEdgeConfig = QStringLiteral(R"JSON({
    "mode": "Edge",
    "protocolsConfig": {
        "entryNodes": [],
        "clusterId": 1,
        "messageValidation": {
            "maxMessageSize": "150 KiB",
            "rlnConfig": null
        }
    },
    "networkingConfig": {
        "listenIpv4": "127.0.0.1",
        "p2pTcpPort": 61000,
        "discv5UdpPort": 61001
    },
    "ethRpcEndpoints": [],
    "p2pReliability": false,
    "logLevel": "INFO",
    "logFormat": "TEXT"
})JSON");

const QString DeliveryModulePluginCreateNodeIntegrationTest::kCoreConfig = QStringLiteral(R"JSON({
    "mode": "Core",
    "protocolsConfig": {
        "entryNodes": [],
        "clusterId": 1,
        "autoShardingConfig": {
            "numShardsInCluster": 1
        },
        "messageValidation": {
            "maxMessageSize": "150 KiB",
            "rlnConfig": null
        }
    },
    "networkingConfig": {
        "listenIpv4": "127.0.0.1",
        "p2pTcpPort": 62000,
        "discv5UdpPort": 62001
    },
    "ethRpcEndpoints": [],
    "p2pReliability": false,
    "logLevel": "INFO",
    "logFormat": "TEXT"
    })JSON");

void DeliveryModulePluginCreateNodeIntegrationTest::createNode_withDefaultConfig_succeedsOrSkips()
{
    if (!hasLibpqDependency()) {
        QSKIP("libpq dynamic library is not available; skipping logosdelivery integration test.");
    }

    DeliveryModulePlugin plugin;

    QVERIFY(plugin.createNode(kDefaultConfig));
}

void DeliveryModulePluginCreateNodeIntegrationTest::createNode_withEdgeConfig_succeedsOrSkips()
{
    if (!hasLibpqDependency()) {
        QSKIP("libpq dynamic library is not available; skipping logosdelivery integration test.");
    }

    DeliveryModulePlugin plugin;

    QVERIFY(plugin.createNode(kEdgeConfig));
}

void DeliveryModulePluginCreateNodeIntegrationTest::createNode_withCoreConfig_succeedsOrSkips()
{
    if (!hasLibpqDependency()) {
        QSKIP("libpq dynamic library is not available; skipping logosdelivery integration test.");
    }

    DeliveryModulePlugin plugin;
    QElapsedTimer timer;
    timer.start();

    QVERIFY(plugin.createNode(kCoreConfig));
}

void DeliveryModulePluginCreateNodeIntegrationTest::createNode_secondCallRejected_afterSuccessfulInit()
{
    if (!hasLibpqDependency()) {
        QSKIP("libpq dynamic library is not available; skipping logosdelivery integration test.");
    }

    DeliveryModulePlugin plugin;
    QVERIFY(plugin.createNode(kEdgeConfig));

    QVERIFY(!plugin.createNode(kEdgeConfig));
}

void DeliveryModulePluginCreateNodeIntegrationTest::startStop_withRealBackend_succeedsOrSkips()
{
    if (!hasLibpqDependency()) {
        QSKIP("libpq dynamic library is not available; skipping logosdelivery integration test.");
    }

    DeliveryModulePlugin plugin;
    QVERIFY(plugin.createNode(kEdgeConfig));

    QVERIFY(plugin.start());

    QVERIFY(plugin.stop());
}

QTEST_MAIN(DeliveryModulePluginCreateNodeIntegrationTest)
#include "test_create_node_integration.moc"
