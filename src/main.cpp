#include "audio_renderer.h"
#include "moonlight_client.h"
#include "moonlight_config.h"
#include "null_video_renderer.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QRegularExpression>
#include <QTextStream>

#include <openssl/rand.h>

#include <csignal>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace {
volatile std::sig_atomic_t interrupted = 0;
std::atomic<bool> connectionStarted {false};
std::atomic<bool> connectionTerminated {false};
void onSignal(int) { interrupted = 1; LiInterruptConnection(); }
void commonLog(const char*, ...) {}

void printError(const QString& error) { QTextStream(stderr) << error << '\n'; }

CONNECTION_LISTENER_CALLBACKS connectionCallbacks() {
    CONNECTION_LISTENER_CALLBACKS callbacks;
    LiInitializeConnectionCallbacks(&callbacks);
    callbacks.stageStarting = [](int stage) { qInfo().noquote() << "Connecting:" << LiGetStageName(stage); };
    callbacks.stageFailed = [](int stage, int error) { qWarning().noquote() << "Connection failed at" << LiGetStageName(stage) << "(" << error << ")"; };
    callbacks.connectionStarted = [] { connectionStarted.store(true); qInfo() << "Connected."; };
    callbacks.connectionTerminated = [](int error) { connectionTerminated.store(true); if (!interrupted) qWarning() << "Connection ended:" << error; };
    callbacks.logMessage = commonLog;
    return callbacks;
}

MoonlightApp selectApp(const QVector<MoonlightApp>& apps, const QString& wanted, QString* error) {
    const QString requested = wanted.isEmpty() ? "Desktop" : wanted;
    for (const auto& app : apps) if (app.name.compare(requested, Qt::CaseInsensitive) == 0) return app;
    *error = QString("Application '%1' was not found on the host.").arg(requested);
    return {};
}

struct StreamOptions {
    int width = 320;
    int height = 180;
    int fps = 30;
    int bitrate = 500;
    int durationSeconds = 0;
};

int stream(const MoonlightHost& host, const MoonlightIdentity& identity, const QString& appName, bool attach, bool verbose, const QString& addressOverride, const StreamOptions& options) {
    MoonlightClient client(identity, host, verbose, addressOverride);
    ServerInfo server;
    QString error;
    if (!client.verifyPaired(&server, &error)) { printError(error); return 1; }
    QVector<MoonlightApp> apps;
    if (!client.apps(&apps, &error)) { printError(error); return 1; }
    MoonlightApp app = selectApp(apps, appName, &error);
    if (!app.id) { printError(error); return 1; }
    if (server.currentGame != 0) {
        MoonlightApp running;
        for (const MoonlightApp& candidate : apps) if (candidate.id == server.currentGame) { running = candidate; break; }
        if (!running.id) { printError("Sunshine reported an active application that is absent from its application list."); return 1; }
        if (attach || appName.isEmpty()) {
            app = running;
            qInfo().noquote() << "Attaching to Sunshine's active application:" << app.name;
        }
        else if (app.id != running.id) {
            printError(QString("Sunshine is already streaming '%1'. Use --attach to receive audio from that active application, or stop it before launching '%2'.").arg(running.name, app.name));
            return 1;
        }
    }

    STREAM_CONFIGURATION config;
    LiInitializeStreamConfiguration(&config);
    // Sunshine's RTSP parser currently accepts arbitrary positive dimensions;
    // use a small even H.264 stream solely to keep its required video transport alive.
    config.width = options.width;
    config.height = options.height;
    config.fps = options.fps;
    config.bitrate = options.bitrate;
    config.packetSize = 1024;
    config.streamingRemotely = STREAM_CFG_AUTO;
    config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    config.supportedVideoFormats = VIDEO_FORMAT_H264;
    config.encryptionFlags = ENCFLG_ALL;
    RAND_bytes(reinterpret_cast<unsigned char*>(config.remoteInputAesKey), sizeof(config.remoteInputAesKey));
    RAND_bytes(reinterpret_cast<unsigned char*>(config.remoteInputAesIv), 4);

    QString rtsp;
    qInfo() << "Found paired host:" << host.name;
    qInfo() << "Using existing Moonlight pairing.";
    if (!client.launch(app, &config, &rtsp, &error)) { printError(error); return 1; }
    qInfo().noquote() << "Launching" << app.name << "with" << config.width << "x" << config.height << "H.264 video discarded.";

    AudioRenderer audio;
    auto videoCallbacks = makeNullVideoRenderer();
    auto audioCallbacks = audio.callbacks();
    auto callbacks = connectionCallbacks();
    const QByteArray address = client.activeAddress().toUtf8();
    const QByteArray appVersion = server.appVersion.toUtf8();
    const QByteArray gfeVersion = server.gfeVersion.toUtf8();
    const QByteArray rtspUrl = rtsp.toUtf8();
    SERVER_INFORMATION hostInfo;
    LiInitializeServerInformation(&hostInfo);
    hostInfo.address = address.constData();
    hostInfo.serverInfoAppVersion = appVersion.constData();
    hostInfo.serverInfoGfeVersion = gfeVersion.isEmpty() ? nullptr : gfeVersion.constData();
    hostInfo.rtspSessionUrl = rtspUrl.isEmpty() ? nullptr : rtspUrl.constData();
    hostInfo.serverCodecModeSupport = server.codecModes;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    connectionStarted.store(false);
    connectionTerminated.store(false);
    qInfo() << "Audio uses an Opus multistream decoder and a" << audio.configuredLatencyMs() << "ms CoreAudio queue (100 ms bounded PCM ring).";
    if (options.durationSeconds > 0) qInfo() << "Stopping automatically after" << options.durationSeconds << "seconds.";
    qInfo() << "Press Ctrl-C to disconnect.";
    std::atomic<bool> durationElapsed {false};
    std::jthread durationStopper;
    if (options.durationSeconds > 0) {
        durationStopper = std::jthread([seconds = options.durationSeconds, &durationElapsed](std::stop_token stop) {
            for (int tenths = seconds * 10; tenths > 0 && !stop.stop_requested(); --tenths) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!stop.stop_requested()) durationElapsed.store(true);
        });
    }
    const int result = LiStartConnection(&hostInfo, &config, &callbacks, &videoCallbacks, &audioCallbacks, nullptr, 0, &audio, 0);
    // moonlight-common-c returns after setup and streams asynchronously. Keep
    // this process alive, then stop the active connection from this same thread.
    if (result == 0 && connectionStarted.load()) {
        while (!interrupted && !durationElapsed.load() && !connectionTerminated.load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        LiStopConnection();
    }
    if (durationStopper.joinable()) durationStopper.request_stop();
    qInfo().noquote() << audio.statistics();
    qInfo() << "Reported CoreAudio device latency:" << audio.deviceLatencyMs() << "ms.";
    if (result != 0 && !interrupted) { qWarning() << "Failed to establish Moonlight stream:" << result; return 1; }
    return 0;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationVersion("0.1.0");
    QCommandLineParser parser;
    parser.setApplicationDescription("Audio-only Moonlight/GameStream client for already-paired Sunshine hosts");
    parser.addHelpOption(); parser.addVersionOption();
    QCommandLineOption appOption({"a", "app"}, "Sunshine application name (defaults to Desktop).", "name");
    QCommandLineOption attachOption("attach", "Attach to Sunshine's currently active application; ignores --app.");
    QCommandLineOption videoSizeOption("video-size", "Discarded video stream size, as WIDTHxHEIGHT (default: 320x180).", "size");
    QCommandLineOption fpsOption("fps", "Discarded video stream frames per second (default: 30).", "fps");
    QCommandLineOption bitrateOption("bitrate", "Discarded video stream bitrate in Kbps (default: 500).", "kbps");
    QCommandLineOption durationOption("duration", "Disconnect after this many seconds; useful for diagnostics.", "seconds");
    QCommandLineOption verboseOption("verbose", "Log GameStream API requests.");
    parser.addOption(appOption); parser.addOption(attachOption); parser.addOption(videoSizeOption); parser.addOption(fpsOption); parser.addOption(bitrateOption); parser.addOption(durationOption); parser.addOption(verboseOption);
    parser.addPositionalArgument("command-or-host", "hosts, apps, or a known Moonlight host name/address.");
    parser.addPositionalArgument("host", "Host for the apps command.");
    parser.process(app);
    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) parser.showHelp(2);
    QString error;
    const MoonlightConfig config = MoonlightConfig::load(&error);
    if (!error.isEmpty()) { printError(error); return 1; }
    if (args.first() == "hosts") {
        QTextStream out(stdout);
        for (const MoonlightHost& host : config.hosts()) {
            const QString address = !host.localAddress.isEmpty() ? host.localAddress : (!host.manualAddress.isEmpty() ? host.manualAddress : host.remoteAddress);
            out << host.name.leftJustified(20) << address.leftJustified(24) << (host.hasStoredPairing() ? "paired" : "unpaired") << "  " << host.uuid << '\n';
        }
        return 0;
    }
    QString hostArg;
    bool listApps = args.first() == "apps";
    if (listApps) { if (args.size() < 2) parser.showHelp(2); hostArg = args.at(1); } else hostArg = args.first();
    const MoonlightHost* host = config.findHost(hostArg);
    if (!host || !host->hasStoredPairing()) {
        printError("Host is not paired in Moonlight.\n\nOpen the normal Moonlight desktop app and pair this Mac with the host first.");
        return 1;
    }
    const bool suppliedAddress = hostArg == host->localAddress || hostArg == host->remoteAddress || hostArg == host->manualAddress || hostArg == host->ipv6Address;
    const QString addressOverride = suppliedAddress ? hostArg : QString();
    MoonlightClient client(config.identity(), *host, parser.isSet(verboseOption), addressOverride);
    ServerInfo server;
    if (!client.verifyPaired(&server, &error)) { printError(error); return 1; }
    if (listApps) {
        QVector<MoonlightApp> apps;
        if (!client.apps(&apps, &error)) { printError(error); return 1; }
        for (const auto& item : apps) if (!item.hidden) QTextStream(stdout) << item.name << '\n';
        return 0;
    }
    StreamOptions options;
    if (parser.isSet(videoSizeOption)) {
        const QRegularExpressionMatch match = QRegularExpression("^(\\d+)x(\\d+)$").match(parser.value(videoSizeOption));
        if (!match.hasMatch()) { printError("--video-size must be WIDTHxHEIGHT."); return 2; }
        options.width = match.captured(1).toInt();
        options.height = match.captured(2).toInt();
    }
    const auto positive = [&parser](const QCommandLineOption& option, int* value) {
        if (!parser.isSet(option)) return true;
        bool ok = false;
        const int parsed = parser.value(option).toInt(&ok);
        if (!ok || parsed <= 0) { printError("--" + option.names().last() + " must be a positive integer."); return false; }
        *value = parsed;
        return true;
    };
    if (options.width < 2 || options.height < 2 || options.width % 2 || options.height % 2) { printError("--video-size must contain positive even dimensions."); return 2; }
    if (!positive(fpsOption, &options.fps) || !positive(bitrateOption, &options.bitrate) || !positive(durationOption, &options.durationSeconds)) return 2;
    return stream(*host, config.identity(), parser.value(appOption), parser.isSet(attachOption), parser.isSet(verboseOption), addressOverride, options);
}
