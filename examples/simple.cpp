#include <QCoreApplication>
#include <QPluginLoader>
#include <QDebug>
#include "delivery_module_interface.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QPluginLoader loader("delivery_plugin");
    QObject *plugin = loader.instance();

    if (!plugin) {
        qDebug() << "Failed to load plugin:" << loader.errorString();
        return -1;
    }

    auto delivery = qobject_cast<DeliveryModuleInterface *>(plugin);
    if (!delivery) {
        qDebug() << "Invalid plugin type";
        return -1;
    }

    qDebug() << "Loaded plugin:" << delivery->name();

    delivery->createNode("config.json");
    delivery->start();

    auto result = delivery->send("topic1", "Hello World");

    if (result.has_value())
        qDebug() << "Send result:" << result.value();

    delivery->stop();

    return 0;
}