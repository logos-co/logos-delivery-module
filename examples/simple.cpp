#include <QCoreApplication>
#include <QPluginLoader>
#include <QDebug>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QCommandLineParser>
#include <QString>
#include <iostream>
#include "../delivery_module_interface.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("DeliveryModuleLoader");
    QCoreApplication::setApplicationVersion("1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Loads a delivery module plugin and runs it.");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption pluginOption(
        QStringList() << "m" << "module",
        "Path to the delivery module plugin (.so/.dll).",
        "plugin_file"
    );
    parser.addOption(pluginOption);

    QCommandLineOption configOption(
        QStringList() << "c" << "config",
        "Path to JSON config file for the plugin.",
        "config_file",
        "config.json"
    );
    parser.addOption(configOption);

    parser.process(app);

    if (!parser.isSet(pluginOption)) {
        qDebug() << "Error: plugin module path is required.";
        parser.showHelp(); // exits automatically
    }

    QString pluginPath = parser.value(pluginOption);
    QString configPath = parser.value(configOption);

    QFileInfo pluginFile(pluginPath);
    if (!pluginFile.exists() || !pluginFile.isFile()) {
        qDebug() << "Plugin file does not exist:" << pluginPath;
        return -1;
    }

    QFileInfo configFile(configPath);
    if (!configFile.exists() || !configFile.isFile()) {
        qDebug() << "Config file does not exist:" << configPath;
        return -1;
    }

    QFile jsonFile(configFile.absoluteFilePath());
    if (!jsonFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "Failed to open config file:" << configFile.absoluteFilePath();
        return -1;
    }

    QByteArray jsonData = jsonFile.readAll();
    jsonFile.close();

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(jsonData, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qDebug() << "Failed to parse JSON:" << parseError.errorString();
        return -1;
    }

    QPluginLoader loader(pluginFile.absoluteFilePath());
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

    delivery->createNode(QString::fromUtf8(jsonData));
    delivery->start();

    qDebug() << "Plugin loaded. Type a message to send, or 'exit' to quit.";

    QString contentTopicOfInterest = "/simple-example/2/delivery/proto";
    if (!delivery->subscribe(contentTopicOfInterest)) {
        qDebug() << "Failed to subscribe to topic:" << contentTopicOfInterest;
        return -1;
    }

    // ------------------------
    // Interactive loop
    // ------------------------
    std::string input;
    while (true) {
        std::cout << "> ";
        std::getline(std::cin, input);

        if (input == "exit") {
            break;
        } else if (!input.empty()) {
            auto result = delivery->send(contentTopicOfInterest, QString::fromStdString(input));
            if (!result.isErr())
                qDebug() << "Send result:" << result.value();
            else
                qDebug() << "Send failed";
        }
    }

    delivery->stop();

    return 0;
}