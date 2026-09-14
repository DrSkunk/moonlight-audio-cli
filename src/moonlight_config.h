#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

struct MoonlightIdentity {
    QByteArray certificatePem;
    QByteArray privateKeyPem;
    QString uniqueId;
    bool valid() const { return !certificatePem.isEmpty() && !privateKeyPem.isEmpty(); }
};

struct MoonlightHost {
    QString name;
    QString uuid;
    QString localAddress;
    quint16 localPort = 47989;
    QString remoteAddress;
    quint16 remotePort = 47989;
    QString manualAddress;
    quint16 manualPort = 47989;
    QString ipv6Address;
    quint16 ipv6Port = 47989;
    QByteArray serverCertificatePem;
    bool nvidiaServer = false;
    bool hasStoredPairing() const { return !serverCertificatePem.isEmpty(); }
};

class MoonlightConfig {
public:
    static MoonlightConfig load(QString* error);
    const MoonlightIdentity& identity() const { return m_identity; }
    const QVector<MoonlightHost>& hosts() const { return m_hosts; }
    const MoonlightHost* findHost(const QString& query) const;
    static QString nativeSettingsPath();

private:
    MoonlightIdentity m_identity;
    QVector<MoonlightHost> m_hosts;
};
