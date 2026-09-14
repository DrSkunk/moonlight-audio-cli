#pragma once

#include "moonlight_config.h"

#include <QString>
#include <QStringList>
#include <QVector>

extern "C" {
#include <Limelight.h>
}

struct MoonlightApp { QString name; int id = 0; bool hidden = false; };
struct ServerInfo { QString appVersion; QString gfeVersion; int codecModes = SCM_H264; bool paired = false; int currentGame = 0; quint16 httpsPort = 47984; };

class MoonlightClient {
public:
    MoonlightClient(const MoonlightIdentity& identity, const MoonlightHost& host, bool verbose, QString addressOverride = {});
    bool verifyPaired(ServerInfo* info, QString* error);
    bool apps(QVector<MoonlightApp>* apps, QString* error);
    bool launch(const MoonlightApp& app, STREAM_CONFIGURATION* streamConfig, QString* rtspUrl, QString* error);
    QString activeAddress() const;
    const ServerInfo& serverInfo() const { return m_serverInfo; }

private:
    QString request(const QString& command, const QString& arguments, int timeoutMs, QString* error);
    static QString xmlValue(const QString& xml, const QString& tag);
    static bool responseOk(const QString& xml, QString* error);
    QString chooseAddress() const;
    QStringList candidateAddresses() const;

    MoonlightIdentity m_identity;
    MoonlightHost m_host;
    ServerInfo m_serverInfo;
    bool m_verbose;
    QString m_addressOverride;
    QString m_activeAddress;
};
