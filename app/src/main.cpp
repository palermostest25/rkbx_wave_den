// Rekordbox Wave: an XDJ-AZ style deck screen and touch browser for Rekordbox.
#include "app/gig.h"
#include "app/install.h"
#include "app/session.h"
#include "app/updater.h"
#include "core/settings.h"
#include "ui/mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QImageReader>
#include <QSslSocket>
#include <QLoggingCategory>
#include <QStyleFactory>
#include <QSysInfo>
#include <QThread>
#include <QTimer>

Q_LOGGING_CATEGORY(lcMain, "wave")

int main(int argc, char **argv) {
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Rekordbox Wave"));
    QApplication::setOrganizationName(QStringLiteral("Rekordbox Wave"));
    QApplication::setApplicationVersion(wave::version());
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/ui/icon.png")));

    QCommandLineParser args;
    args.setApplicationDescription(QStringLiteral("Rekordbox Wave"));
    args.addHelpOption();
    args.addVersionOption();
    const QCommandLineOption demo("demo", "Show labelled synthetic decks instead of Rekordbox's.");
    const QCommandLineOption config("config", "Use a specific settings file.", "file");
    const QCommandLineOption smoke("smoke-test", "Draw the window into <png>, then quit.", "png");
    const QCommandLineOption setup("setup", "Open Rekordbox setup if the supported Rekordbox is missing.");
    const QCommandLineOption installRekordbox("install-rekordbox", "Install the supported Rekordbox now if it is missing.");
    const QCommandLineOption installSelf("install", "Install this Wave in Program Files (run as administrator).");
    const QCommandLineOption replace("replace", "Put this Wave in place of <exe> (an update, as administrator).", "exe");
    const QCommandLineOption uninstall("uninstall", "Remove Rekordbox Wave from this computer.");
    const QCommandLineOption uninstallNow("uninstall-now");
    const QCommandLineOption afterInstall("after-install");
    const QCommandLineOption selfCheck("self-check", "Check this build can read artwork and reach GitHub over HTTPS.");
    const QCommandLineOption page("page", "Open on <page>: settings, browser, or a settings page.", "page");
    args.addOptions({demo, config, smoke, setup, installRekordbox, installSelf, replace, uninstall, uninstallNow, afterInstall, page, selfCheck});
    args.process(app);

    if (args.isSet(selfCheck)) {
        // A static build carries only the plugins it was linked with: JPEG for artwork, a TLS backend for updates.
        const bool jpeg = QImageReader::supportedImageFormats().contains("jpg");
        const bool tls = QSslSocket::supportsSsl();
        fprintf(stdout, "jpeg %d, tls %d (%s)\n", jpeg, tls, qPrintable(QSslSocket::availableBackends().join(", ")));
        return jpeg && tls ? 0 : 1;
    }
    // The installer's halves: no window, an exit code.
    if (args.isSet(installSelf)) return wave::install::runInstall();
    if (args.isSet(replace)) return wave::install::runReplace(args.value(replace));
    if (args.isSet(uninstall)) return wave::install::runUninstall(true);
    if (args.isSet(uninstallNow)) return wave::install::runUninstall(false);

    const bool demoMode = args.isSet(demo), smokeTest = args.isSet(smoke);
    const bool logged = wave::setupLogging(wave::logFolder());
    qCInfo(lcMain) << "Rekordbox Wave" << wave::version() << "starting on" << QSysInfo::prettyProductName() << (demoMode ? "(demo)" : "(live)");
    std::unique_ptr<wave::SingleInstance> instance;
    if (!smokeTest && !demoMode) {
        instance = std::make_unique<wave::SingleInstance>();
        // A Wave that just started this one (an update, INSTALL) may still be closing: give it a few seconds.
        for (int i = 0; i < 30 && !instance->primary(); ++i) {
            QThread::msleep(200);
            instance = std::make_unique<wave::SingleInstance>();
        }
        if (!instance->primary()) {
            qCInfo(lcMain) << "Wave is already running: brought it forward";
            return 0;
        }
    }
    wave::Updater::cleanUp();
    wave::Session session(args.isSet(config) ? args.value(config) : wave::settingsFile(), demoMode);
    wave::MainWindow window(&session);
    window.show();
    if (args.isSet(page)) window.showPage(args.value(page));
    if (instance)
        instance->listen([&window] {
            if (window.isMinimized()) window.showNormal();
            window.raise();
            window.activateWindow();
        });
    if (!logged) qCWarning(lcMain) << "No log file: the settings folder is not writable";
    if (args.isSet(afterInstall))
        QTimer::singleShot(800, &window, [&window] {
            window.showBanner(QStringLiteral("Rekordbox Wave is installed: open it from the Start menu or the desktop from now on."), "info", {}, {}, 12000);
        });
    if (!demoMode && !smokeTest)
        QTimer::singleShot(1200, &window, [&window, &args, setup, installRekordbox] {
            window.firstRun(args.isSet(installRekordbox), args.isSet(setup));
        });
    if (smokeTest)
        QTimer::singleShot(900, &window, [&window, &app, path = args.value(smoke)] {
            QDir().mkpath(QFileInfo(path).absolutePath());
            const bool ok = window.isVisible() && window.grab().save(path);
            window.close();
            app.exit(ok ? 0 : 1);
        });
    return app.exec();
}
