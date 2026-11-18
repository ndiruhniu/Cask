#include "DesktopIntegration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

namespace DesktopIntegration {

static const char kHeader[] = "[MIME Cache]";

QString applicationsDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/applications");
}

QString desktopFileName()
{
    return QStringLiteral("cask.desktop");
}

QStringList mimeTypes()
{
    return {QStringLiteral("application/x-ms-dos-executable"),
            QStringLiteral("application/vnd.microsoft.portable-executable"),
            QStringLiteral("application/x-msdownload"),
            QStringLiteral("application/x-msi"),
            QStringLiteral("application/x-ms-shortcut"),
            QStringLiteral("application/x-bat")};
}

// Exec value: path in double quotes (special characters escaped per the Desktop Entry spec),
// then backslashes are doubled once more at the file level.
static QString execQuote(const QString& path)
{
    QString s;
    for (const QChar c : path) {
        if (c == '"' || c == '`' || c == '$' || c == '\\')
            s += QLatin1Char('\\');
        s += c;
        if (c == '%')
            s += QLatin1Char('%');
    }
    s.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    return QLatin1Char('"') + s + QLatin1Char('"');
}

QByteArray desktopContent(const QString& exePath)
{
    const QString text =
        QStringLiteral("[Desktop Entry]\n"
                       "Type=Application\n"
                       "Name=Cask\n"
                       "GenericName=Wine prefix manager\n"
                       "GenericName[ru]=Менеджер префиксов Wine\n"
                       "Comment=Minimalist Wine prefix manager\n"
                       "Comment[ru]=Минималистичный менеджер префиксов Wine\n"
                       "Exec=%1 %f\n"
                       "Icon=wine\n"
                       "Terminal=false\n"
                       "StartupNotify=true\n"
                       "Categories=Utility;\n"
                       "MimeType=%2;\n")
            .arg(execQuote(exePath), mimeTypes().join(QLatin1Char(';')));
    return text.toUtf8();
}

QByteArray updateMimeCache(const QByteArray& cache, bool add)
{
    const QByteArray me = desktopFileName().toUtf8();
    QList<QByteArray> lines = cache.split('\n');
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();

    bool hasHeader = false;
    for (const QByteArray& l : lines)
        if (l.trimmed() == kHeader) {
            hasHeader = true;
            break;
        }
    if (!hasHeader)
        lines.prepend(kHeader);

    for (const QString& mimeStr : mimeTypes()) {
        const QByteArray mime = mimeStr.toUtf8();
        int idx = -1;
        for (int i = 0; i < lines.size(); ++i)
            if (lines[i].startsWith(mime + '=')) {
                idx = i;
                break;
            }

        QList<QByteArray> entries;
        if (idx >= 0)
            for (const QByteArray& e : lines[idx].mid(mime.size() + 1).split(';'))
                if (!e.isEmpty() && e != me)
                    entries << e;
        if (add)
            entries.prepend(me);

        QByteArray line;
        if (!entries.isEmpty()) {
            line = mime + '=';
            for (const QByteArray& e : entries)
                line += e + ';';
        }
        if (idx >= 0) {
            if (line.isEmpty())
                lines.removeAt(idx);
            else
                lines[idx] = line;
        } else if (!line.isEmpty()) {
            lines << line;
        }
    }

    QByteArray out;
    for (const QByteArray& l : lines)
        out += l + '\n';
    return out;
}

static bool readFile(const QString& path, QByteArray* out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    *out = f.readAll();
    return true;
}

static bool writeIfChanged(const QString& path, const QByteArray& data, QString* err)
{
    QByteArray cur;
    if (readFile(path, &cur) && cur == data)
        return true;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
        if (err)
            *err = QCoreApplication::translate("DesktopIntegration", "Could not write %1").arg(path);
        return false;
    }
    return true;
}

bool isInstalled(const QString& appsDir)
{
    return QFileInfo::exists(appsDir + QLatin1Char('/') + desktopFileName());
}

bool install(const QString& appsDir, const QString& exePath, QString* err)
{
    if (!QDir().mkpath(appsDir)) {
        if (err)
            *err = QCoreApplication::translate("DesktopIntegration", "Could not create folder %1").arg(appsDir);
        return false;
    }
    if (!writeIfChanged(appsDir + QLatin1Char('/') + desktopFileName(), desktopContent(exePath), err))
        return false;

    const QString cachePath = appsDir + QStringLiteral("/mimeinfo.cache");
    QByteArray cur;
    if (!readFile(cachePath, &cur))
        return true;  // no cache: the desktop reads the .desktop files itself; do not create a cache of our own
    return writeIfChanged(cachePath, updateMimeCache(cur, true), err);
}

bool remove(const QString& appsDir, QString* err)
{
    const QString desktop = appsDir + QLatin1Char('/') + desktopFileName();
    if (QFileInfo::exists(desktop) && !QFile::remove(desktop)) {
        if (err)
            *err = QCoreApplication::translate("DesktopIntegration", "Could not write %1").arg(desktop);
        return false;
    }

    // We neither create nor delete mimeinfo.cache: we only remove our own entries if the file exists.
    const QString cachePath = appsDir + QStringLiteral("/mimeinfo.cache");
    QByteArray cur;
    if (!readFile(cachePath, &cur))
        return true;
    return writeIfChanged(cachePath, updateMimeCache(cur, false), err);
}

bool sync(bool enabled, const QString& appsDir, const QString& exePath, QString* err)
{
    return enabled ? install(appsDir, exePath, err) : remove(appsDir, err);
}

}  // namespace DesktopIntegration
