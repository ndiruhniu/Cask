#include "ui/MainWindow.h"
#include "ui/PrefixPickerDialog.h"

#include <QApplication>
#include <QTimer>
#include <cstdio>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QUrl>

#include "core/Config.h"
#include "core/DesktopIntegration.h"
#include "core/I18n.h"

int main(int argc, char* argv[])
{
    // Bundled Qt plugins live in lib/plugins next to the binary, so no qt.conf is needed in the root.
    // They replace the system plugin paths: plugins built for another Qt version are rejected anyway.
    // Must happen before QApplication is created. Uses /proc/self/exe because there is no app object yet.
    const QString exeDir = QFileInfo(QFileInfo(QStringLiteral("/proc/self/exe")).symLinkTarget()).absolutePath();
    if (QFileInfo(exeDir + QStringLiteral("/lib/plugins")).isDir())
        QCoreApplication::setLibraryPaths({exeDir + QStringLiteral("/lib/plugins")});

    // In a Wayland session use Wayland, with X11 (XWayland) as the fallback if the plugin cannot start.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM") && !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "wayland;xcb");

    QApplication app(argc, argv);
    app.setApplicationName("Cask");
    app.setDesktopFileName(QStringLiteral("cask"));  // the app id on Wayland: matches cask.desktop (icon, grouping)
    app.setApplicationVersion("0.1.0");

    Config& cfg = Config::instance();
    cfg.ensureLayout();
    I18n::apply(cfg.language());  // by default "system": the language is taken from the system

    // The Cask folder may have been moved: re-check .desktop and mimeinfo.cache; if integration is off, remove the leftovers.
    QString desktopErr;
    if (!DesktopIntegration::sync(cfg.desktopIntegration(), DesktopIntegration::applicationsDir(),
                                  app.applicationFilePath(), &desktopErr))
        qWarning("Desktop integration: %s", qPrintable(desktopErr));

    // Launching a file from the file manager: cask <file>. Show the prefix picker and run it; the main window is not needed.
    QString file;
    for (const QString& a : app.arguments().mid(1)) {
        if (!a.startsWith('-')) {
            file = a.startsWith(QLatin1String("file://")) ? QUrl(a).toLocalFile() : a;
            break;
        }
    }
    if (!file.isEmpty()) {
        const QFileInfo fi(file);
        if (!fi.isFile()) {
            QMessageBox::warning(nullptr, QStringLiteral("Cask"), QObject::tr("File not found: %1").arg(file));
            return 1;
        }
        PrefixManager mgr;
        PrefixPickerDialog picker(&mgr, fi.fileName());
        if (picker.exec() != QDialog::Accepted)
            return 0;
        QString err;
        if (!mgr.launch(picker.prefixName(), PrefixManager::programArgs(fi.absoluteFilePath()), fi.absolutePath(),
                        &err, true)) {
            QMessageBox::warning(nullptr, QStringLiteral("Cask"), err);
            return 1;
        }
        cfg.setLastPrefix(picker.prefixName());
        return 0;
    }

    MainWindow w;
    w.show();

    // For CI: CASK_SCREENSHOT=<file> saves a picture of the window, prints the platform in use, and quits.
    const QString shot = qEnvironmentVariable("CASK_SCREENSHOT");
    if (!shot.isEmpty()) {
        QTimer::singleShot(2000, &app, [&app, &w, shot] {
            const bool ok = w.grab().save(shot);
            std::fprintf(stderr, "platform: %s\nscreenshot: %s\n", qPrintable(QGuiApplication::platformName()),
                         ok ? "saved" : "failed");
            app.exit(ok ? 0 : 1);
        });
    }
    return app.exec();
}
