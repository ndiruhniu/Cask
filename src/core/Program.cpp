#include "Program.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <cstring>

namespace Programs {

namespace {

// DIR/NAME, where NAME is matched ignoring case (Windows paths do not care about it, the file system does).
QString findIgnoringCase(const QString& dir, const QString& name)
{
    const QString exact = QDir(dir).filePath(name);
    if (QFileInfo::exists(exact))
        return exact;
    const QStringList entries = QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QString& e : entries)
        if (e.compare(name, Qt::CaseInsensitive) == 0)
            return QDir(dir).filePath(e);
    return {};
}

bool readU32(const QByteArray& d, qsizetype off, quint32* v)
{
    if (off < 0 || off + 4 > d.size())
        return false;
    *v = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(d.constData() + off));
    return true;
}

QString ansiAt(const QByteArray& d, quint32 off)
{
    if (off == 0 || qsizetype(off) >= d.size())
        return {};
    qsizetype len = 0;
    while (qsizetype(off) + len < d.size() && d[qsizetype(off) + len] != '\0')
        ++len;
    return QString::fromLocal8Bit(d.constData() + off, len);
}

QString wideAt(const QByteArray& d, quint32 off)
{
    if (off == 0 || qsizetype(off) >= d.size())
        return {};
    QString out;
    for (qsizetype i = qsizetype(off); i + 1 < d.size(); i += 2) {
        const quint16 c = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(d.constData() + i));
        if (c == 0)
            break;
        out.append(QChar(c));
    }
    return out;
}

// Folders that Wine and Windows fill with their own shortcuts: not the user's programs.
bool isSystemPath(const QString& prefixPath, const QString& file)
{
    const QString rel = QDir(prefixPath + "/drive_c").relativeFilePath(file).toLower();
    if (rel.startsWith(QLatin1String("windows/")))
        return true;
    static const QStringList skip{
        "program files/internet explorer/", "program files/windows nt/", "program files/windows media player/",
        "program files/common files/",      "program files (x86)/internet explorer/",
        "program files (x86)/windows nt/",  "program files (x86)/windows media player/",
        "program files (x86)/common files/",
    };
    return std::any_of(skip.begin(), skip.end(), [&](const QString& s) { return rel.startsWith(s); });
}

bool isUninstaller(const QString& file)
{
    const QString n = QFileInfo(file).fileName().toLower();
    return n.startsWith(QLatin1String("unins")) || n.contains(QLatin1String("uninstall"));
}

}  // namespace

QString resolve(const QString& prefixPath, const QString& stored)
{
    return QDir::isAbsolutePath(stored) ? QDir::cleanPath(stored) : QDir::cleanPath(prefixPath + "/" + stored);
}

QString store(const QString& prefixPath, const QString& absolute)
{
    const QString prefix = QDir::cleanPath(prefixPath);
    const QString file = QDir::cleanPath(absolute);
    if (file.startsWith(prefix + "/"))
        return file.mid(prefix.size() + 1);
    return file;
}

QString imageName(const QString& path)
{
    return QFileInfo(path).fileName().toLower();
}

QString lnkTarget(const QByteArray& d)
{
    quint32 headerSize = 0, flags = 0;
    if (!readU32(d, 0, &headerSize) || headerSize != 0x4C || !readU32(d, 0x14, &flags))
        return {};

    qsizetype pos = 0x4C;
    if (flags & 0x1) {  // HasLinkTargetIDList: skip it
        if (pos + 2 > d.size())
            return {};
        pos += 2 + qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(d.constData() + pos));
    }
    if (!(flags & 0x2))  // HasLinkInfo
        return {};

    quint32 size = 0, infoHeader = 0, infoFlags = 0, basePath = 0, suffix = 0;
    if (!readU32(d, pos, &size) || !readU32(d, pos + 4, &infoHeader) || !readU32(d, pos + 8, &infoFlags)
        || !readU32(d, pos + 16, &basePath) || !readU32(d, pos + 24, &suffix))
        return {};
    if (!(infoFlags & 0x1) || size < 28 || pos + qsizetype(size) > d.size())  // VolumeIDAndLocalBasePath
        return {};
    const QByteArray info = d.mid(pos, qsizetype(size));

    quint32 basePathW = 0, suffixW = 0;
    if (infoHeader >= 0x24) {
        readU32(info, 28, &basePathW);
        readU32(info, 32, &suffixW);
    }
    const QString base = basePathW ? wideAt(info, basePathW) : ansiAt(info, basePath);
    const QString tail = basePathW ? wideAt(info, suffixW) : ansiAt(info, suffix);
    return base.isEmpty() ? QString() : base + tail;
}

QString windowsToUnix(const QString& prefixPath, const QString& windowsPath)
{
    if (windowsPath.size() < 3 || windowsPath[1] != QLatin1Char(':')
        || (windowsPath[2] != QLatin1Char('\\') && windowsPath[2] != QLatin1Char('/')))
        return {};
    const QChar letter = windowsPath[0].toLower();
    QString cur = prefixPath + "/dosdevices/" + letter + QLatin1Char(':');
    if (!QFileInfo::exists(cur)) {
        if (letter != QLatin1Char('c'))
            return {};
        cur = prefixPath + "/drive_c";
    }
    static const QRegularExpression sep(QStringLiteral("[\\\\/]+"));
    for (const QString& part : windowsPath.mid(3).split(sep, Qt::SkipEmptyParts)) {
        cur = findIgnoringCase(cur, part);
        if (cur.isEmpty())
            return {};
    }
    return QDir::cleanPath(cur);
}

QStringList discover(const QString& prefixPath)
{
    // Where the installed programs put their shortcuts: the common and the users' Start Menus.
    QStringList menus{prefixPath + "/drive_c/ProgramData/Microsoft/Windows/Start Menu"};
    const QDir users(prefixPath + "/drive_c/users");
    for (const QString& u : users.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString base = users.filePath(u);
        menus << base + "/Start Menu" << base + "/AppData/Roaming/Microsoft/Windows/Start Menu"
              << base + "/Application Data/Microsoft/Windows/Start Menu";
    }

    QStringList out;
    QSet<QString> seen;
    for (const QString& menu : std::as_const(menus)) {
        if (!QFileInfo(menu).isDir())
            continue;
        QDirIterator it(menu, QDir::Files, QDirIterator::Subdirectories);  // does not follow links
        while (it.hasNext()) {
            const QString lnk = it.next();
            if (!lnk.endsWith(QLatin1String(".lnk"), Qt::CaseInsensitive))
                continue;
            QFile f(lnk);
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const QString target = lnkTarget(f.read(1 << 20));
            if (!target.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive))
                continue;
            const QString file = windowsToUnix(prefixPath, target);
            if (file.isEmpty() || !QFileInfo(file).isFile() || isSystemPath(prefixPath, file) || isUninstaller(file))
                continue;
            if (seen.contains(file.toLower()))
                continue;
            seen.insert(file.toLower());
            out << file;
        }
    }
    std::sort(out.begin(), out.end(), [](const QString& a, const QString& b) {
        return QFileInfo(a).fileName().compare(QFileInfo(b).fileName(), Qt::CaseInsensitive) < 0;
    });
    return out;
}

}  // namespace Programs
