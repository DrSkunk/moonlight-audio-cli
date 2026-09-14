#include "moonlight_client.h"

#include <QEventLoop>
#include <QCryptographicHash>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslKey>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>
#include <QtEndian>

#include <openssl/pem.h>

#include <cstring>

namespace {
constexpr int kRequestTimeoutMs = 5000;
constexpr int kLaunchTimeoutMs = 120000;

QByteArray traditionalPrivateKey(const QByteArray& pem) {
    BIO* input = BIO_new_mem_buf(pem.constData(), pem.size());
    if (!input) return {};
    EVP_PKEY* key = PEM_read_bio_PrivateKey(input, nullptr, nullptr, nullptr);
    BIO_free(input);
    if (!key) return {};
    BIO* output = BIO_new(BIO_s_mem());
    if (!output) { EVP_PKEY_free(key); return {}; }
    const int ok = PEM_write_bio_PrivateKey_traditional(output, key, nullptr, nullptr, 0, nullptr, nullptr);
    EVP_PKEY_free(key);
    BUF_MEM* bytes = nullptr;
    BIO_get_mem_ptr(output, &bytes);
    QByteArray result = ok && bytes ? QByteArray(bytes->data, bytes->length) : QByteArray();
    BIO_free(output);
    return result;
}
}

MoonlightClient::MoonlightClient(const MoonlightIdentity& identity, const MoonlightHost& host, bool verbose, QString addressOverride)
    : m_identity(identity), m_host(host), m_verbose(verbose), m_addressOverride(std::move(addressOverride)) {}

QString MoonlightClient::chooseAddress() const {
    if (!m_activeAddress.isEmpty()) return m_activeAddress;
    if (!m_addressOverride.isEmpty()) return m_addressOverride;
    if (!m_host.localAddress.isEmpty()) return m_host.localAddress;
    if (!m_host.manualAddress.isEmpty()) return m_host.manualAddress;
    if (!m_host.remoteAddress.isEmpty()) return m_host.remoteAddress;
    return m_host.ipv6Address;
}

QStringList MoonlightClient::candidateAddresses() const {
    QStringList addresses;
    const auto add = [&addresses](const QString& address) {
        if (!address.isEmpty() && !addresses.contains(address, Qt::CaseInsensitive)) addresses.append(address);
    };
    if (!m_addressOverride.isEmpty()) add(m_addressOverride);
    add(m_host.localAddress);
    add(m_host.manualAddress);
    add(m_host.remoteAddress);
    add(m_host.ipv6Address);
    return addresses;
}

QString MoonlightClient::activeAddress() const { return chooseAddress(); }

QString MoonlightClient::xmlValue(const QString& xml, const QString& tag) {
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        if (reader.readNext() == QXmlStreamReader::StartElement && reader.name() == tag) return reader.readElementText();
    }
    return {};
}

bool MoonlightClient::responseOk(const QString& xml, QString* error) {
    QXmlStreamReader reader(xml);
    while (reader.readNextStartElement()) {
        if (reader.name() != u"root") continue;
        const QString code = reader.attributes().value("status_code").toString();
        if (code == "200") return true;
        *error = QString("Sunshine returned %1: %2").arg(code.isEmpty() ? "an invalid response" : "HTTP/GameStream status " + code, reader.attributes().value("status_message").toString());
        return false;
    }
    *error = "Sunshine returned malformed XML (missing root status).";
    return false;
}

QString MoonlightClient::request(const QString& command, const QString& arguments, int timeoutMs, QString* error) {
    const QStringList addresses = candidateAddresses();
    if (addresses.isEmpty()) { *error = "The stored Moonlight host record has no usable address."; return {}; }
    QString lastError;
    for (const QString& address : addresses) {
        QUrl url;
        url.setScheme("https");
        url.setHost(address);
        url.setPort(m_serverInfo.httpsPort ? m_serverInfo.httpsPort : 47984);
        url.setPath('/' + command);
        QUrlQuery query;
        // Older Moonlight Qt installations may not have initialized the lazily
        // generated uniqueid setting yet. Pairing itself is certificate-based; use
        // a deterministic in-memory ID derived from that same certificate without
        // mutating the user's preferences file.
        const QString clientId = m_identity.uniqueId.isEmpty()
            ? QString::fromLatin1(QCryptographicHash::hash(m_identity.certificatePem, QCryptographicHash::Sha256).toHex().left(16))
            : m_identity.uniqueId;
        query.addQueryItem("uniqueid", m_host.nvidiaServer ? "0123456789ABCDEF" : clientId);
        query.addQueryItem("uuid", QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (!arguments.isEmpty()) {
            QUrlQuery extra(arguments);
            for (const auto& item : extra.queryItems(QUrl::FullyDecoded)) query.addQueryItem(item.first, item.second);
        }
        url.setQuery(query);
        if (m_verbose) qInfo().noquote() << "GameStream request:" << url.toDisplayString(QUrl::RemoveQuery);

        QNetworkAccessManager manager;
        manager.setProxy(QNetworkProxy::NoProxy);
        QNetworkRequest request(url);
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setLocalCertificate(QSslCertificate(m_identity.certificatePem));
        // This is the same conversion Moonlight Qt's IdentityManager::getSslKey()
        // performs under Q_OS_DARWIN for SecureTransport compatibility.
        ssl.setPrivateKey(QSslKey(traditionalPrivateKey(m_identity.privateKeyPem), QSsl::Rsa));
        request.setSslConfiguration(ssl);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif
        const QSslCertificate pinned(m_host.serverCertificatePem);
        QObject::connect(&manager, &QNetworkAccessManager::sslErrors, &manager, [&pinned](QNetworkReply* reply, const QList<QSslError>& errors) {
            bool onlyPinned = !pinned.isNull();
            for (const QSslError& error : errors) onlyPinned = onlyPinned && error.certificate() == pinned;
            if (onlyPinned) reply->ignoreSslErrors(errors); // exact behavior used by Moonlight Qt NvHTTP
        });
        QNetworkReply* reply = manager.get(request);
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, reply, &QNetworkReply::abort);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timer.start(timeoutMs);
        loop.exec();
        if (reply->error() != QNetworkReply::NoError) {
            lastError = QString("%1: %2").arg(address, reply->errorString());
            reply->deleteLater();
            continue;
        }
        const QString body = QString::fromUtf8(reply->readAll());
        reply->deleteLater();
        m_activeAddress = address;
        if (m_verbose) qInfo().noquote() << "GameStream response:" << body;
        return body;
    }
    *error = QString("Unable to reach the paired host at any saved address (%1).").arg(lastError);
    return {};
}

bool MoonlightClient::verifyPaired(ServerInfo* info, QString* error) {
    // The pinned certificate is the persistent local proof of an old pairing;
    // this request proves that Sunshine still accepts the inherited client TLS identity.
    m_serverInfo.httpsPort = 47984;
    const QString xml = request("serverinfo", {}, kRequestTimeoutMs, error);
    if (xml.isEmpty() || !responseOk(xml, error)) return false;
    m_serverInfo.httpsPort = xmlValue(xml, "HttpsPort").toUShort();
    if (!m_serverInfo.httpsPort) m_serverInfo.httpsPort = 47984;
    m_serverInfo.paired = xmlValue(xml, "PairStatus") == "1";
    m_serverInfo.appVersion = xmlValue(xml, "appversion");
    m_serverInfo.gfeVersion = xmlValue(xml, "GfeVersion");
    m_serverInfo.codecModes = xmlValue(xml, "ServerCodecModeSupport").toInt();
    if (!m_serverInfo.codecModes) m_serverInfo.codecModes = SCM_H264;
    const QString state = xmlValue(xml, "state");
    m_serverInfo.currentGame = state.endsWith("_SERVER_BUSY") ? xmlValue(xml, "currentgame").toInt() : 0;
    if (!m_serverInfo.paired) { *error = "Host is not paired in Moonlight.\n\nOpen the normal Moonlight desktop app and pair this Mac with the host first."; return false; }
    if (info) *info = m_serverInfo;
    return true;
}

bool MoonlightClient::apps(QVector<MoonlightApp>* apps, QString* error) {
    const QString xml = request("applist", {}, kRequestTimeoutMs, error);
    if (xml.isEmpty() || !responseOk(xml, error)) return false;
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        if (reader.readNext() != QXmlStreamReader::StartElement) continue;
        if (reader.name() != u"App") continue;
        MoonlightApp current;
        while (!(reader.readNext() == QXmlStreamReader::EndElement && reader.name() == u"App") && !reader.atEnd()) {
            if (reader.tokenType() != QXmlStreamReader::StartElement) continue;
            if (reader.name() == u"AppTitle") current.name = reader.readElementText();
            else if (reader.name() == u"ID") current.id = reader.readElementText().toInt();
            else if (reader.name() == u"IsHidden") current.hidden = reader.readElementText() == "1";
        }
        if (current.id && !current.name.isNull()) apps->append(current);
    }
    if (reader.hasError()) { *error = "Invalid application list XML."; return false; }
    return true;
}

bool MoonlightClient::launch(const MoonlightApp& app, STREAM_CONFIGURATION* config, QString* rtspUrl, QString* error) {
    int keyId;
    std::memcpy(&keyId, config->remoteInputAesIv, sizeof(keyId));
    keyId = qFromBigEndian(keyId);
    QUrlQuery args;
    args.addQueryItem("appid", QString::number(app.id));
    args.addQueryItem("mode", QString("%1x%2x%3").arg(config->width).arg(config->height).arg(config->fps));
    args.addQueryItem("additionalStates", "1");
    args.addQueryItem("sops", "0"); // don't ask Sunshine to change desktop mode for a discarded video stream
    args.addQueryItem("rikey", QByteArray(config->remoteInputAesKey, 16).toHex());
    args.addQueryItem("rikeyid", QString::number(keyId));
    args.addQueryItem("localAudioPlayMode", "0");
    args.addQueryItem("surroundAudioInfo", QString::number(SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(config->audioConfiguration)));
    args.addQueryItem("remoteControllersBitmap", "0");
    args.addQueryItem("gcmap", "0");
    args.addQueryItem("gcpersist", "0");
    args.addQueryItem("x-ml-video.configuredBitrateKbps", QString::number(config->bitrate));
    const QString xml = request(m_serverInfo.currentGame ? "resume" : "launch", args.toString(QUrl::FullyEncoded), kLaunchTimeoutMs, error);
    if (xml.isEmpty() || !responseOk(xml, error)) return false;
    *rtspUrl = xmlValue(xml, "sessionUrl0");
    return true;
}
