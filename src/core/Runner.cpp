#include "Runner.h"

#include "Config.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace RunnerManager {

QList<Runner> available()
{
    QList<Runner> out;

    QDir d(Config::instance().runnersDir());
    const QStringList names = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    for (const QString& name : names) {
        if (name.compare(QLatin1String("system"), Qt::CaseInsensitive) == 0)
            continue;  // the name of the system Wine
        const QString bin = d.filePath(name) + "/bin";
        QString wine = bin + "/wine";
        if (!QFileInfo(wine).isExecutable())
            wine = bin + "/wine64";
        if (!QFileInfo(wine).isExecutable())
            continue;
        out.append(Runner{name, bin, wine, false});
    }

    const QString sys = QStandardPaths::findExecutable(QStringLiteral("wine"));
    if (!sys.isEmpty())
        out.append(Runner{QStringLiteral("system"), QFileInfo(sys).absolutePath(), sys, true});

    return out;
}

std::optional<Runner> find(const QString& name)
{
    for (const Runner& r : available())
        if (r.name == name)
            return r;
    return std::nullopt;
}

}  // namespace RunnerManager
