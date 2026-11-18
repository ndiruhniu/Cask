#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

// A program of a prefix (a shortcut): a name and the file to run.
// `path` is stored relative to the prefix folder when the file is inside it (so the prefix can be
// moved), otherwise as an absolute path.
struct Program {
    QString name;
    QString path;

    bool operator==(const Program& o) const { return name == o.name && path == o.path; }
};

namespace Programs {

// Stored form <-> absolute path of the file.
QString resolve(const QString& prefixPath, const QString& stored);
QString store(const QString& prefixPath, const QString& absolute);

// Lower-case file name: how a running Wine process of the program is recognized.
QString imageName(const QString& path);

// Programs installed in the prefix: .exe files that the Start Menu shortcuts of the prefix point to
// (Wine's own programs, uninstallers and the Windows folder are left out). Absolute paths.
QStringList discover(const QString& prefixPath);

// Target of a Windows shortcut (.lnk) as a Windows path ("C:\Program Files\..."), empty if it has none.
QString lnkTarget(const QByteArray& lnk);

// "C:\Program Files\7-Zip\7zFM.exe" -> the file in the prefix (case-insensitive), empty if there is none.
QString windowsToUnix(const QString& prefixPath, const QString& windowsPath);

}  // namespace Programs
