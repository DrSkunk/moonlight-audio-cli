#include "moonlight_config.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QSettings>

namespace {
QSettings moonlightSettings() {
    // These are deliberately the exact identifiers set by Moonlight Qt before it
    // constructs QSettings. NativeFormat maps to the installed app's CFPreferences
    // domain on macOS; no setValue(), sync(), or writable setting is used here.
    QCoreApplication::setOrganizationName("Moonlight Game Streaming Project");
    QCoreApplication::setOrganizationDomain("moonlight-stream.com");
    QCoreApplication::setApplicationName("Moonlight");
    return QSettings();
}

MoonlightHost readHost(QSettings& settings) {
    MoonlightHost host;
    host.name = settings.value("hostname").toString();
    host.uuid = settings.value("uuid").toString();
    host.localAddress = settings.value("localaddress").toString();
    host.localPort = settings.value("localport", 47989).toUInt();
    host.remoteAddress = settings.value("remoteaddress").toString();
    host.remotePort = settings.value("remoteport", 47989).toUInt();
    host.manualAddress = settings.value("manualaddress").toString();
    host.manualPort = settings.value("manualport", 47989).toUInt();
    host.ipv6Address = settings.value("ipv6address").toString();
    host.ipv6Port = settings.value("ipv6port", 47989).toUInt();
    host.serverCertificatePem = settings.value("srvcert").toByteArray();
    host.nvidiaServer = settings.value("nvidiasw").toBool();
    return host;
}
}

MoonlightConfig MoonlightConfig::load(QString* error) {
    MoonlightConfig config;
    QSettings settings = moonlightSettings();
    config.m_identity.certificatePem = settings.value("certificate").toByteArray();
    config.m_identity.privateKeyPem = settings.value("key").toByteArray();
    config.m_identity.uniqueId = settings.value("uniqueid").toString();

    // ComputerManager restores hostsbackup only after an interrupted write. We
    // select it under the same condition as its constructor, but never mutate it.
    int count = settings.beginReadArray("hostsbackup");
    if (count == 0) {
        settings.endArray();
        count = settings.beginReadArray("hosts");
    }
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        config.m_hosts.append(readHost(settings));
    }
    settings.endArray();

    if (!config.m_identity.valid()) {
        *error = "Moonlight's client certificate or private key was not found.";
    }
    return config;
}

const MoonlightHost* MoonlightConfig::findHost(const QString& query) const {
    for (const MoonlightHost& host : m_hosts) {
        if (host.name.compare(query, Qt::CaseInsensitive) == 0 || host.uuid.compare(query, Qt::CaseInsensitive) == 0 ||
            host.localAddress == query || host.remoteAddress == query || host.manualAddress.compare(query, Qt::CaseInsensitive) == 0 || host.ipv6Address == query) {
            return &host;
        }
    }
    return nullptr;
}

QString MoonlightConfig::nativeSettingsPath() {
    QSettings settings = moonlightSettings();
    return settings.fileName();
}
