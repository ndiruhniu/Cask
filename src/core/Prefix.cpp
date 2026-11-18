#include "Prefix.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {
const char* kMetaFile = "cask.json";
}

bool Prefix::save() const
{
    QJsonObject o;
    o["runner"] = runner;
    o["arch"] = arch;
    o["created"] = created.toString(Qt::ISODate);

    QJsonObject comps;
    for (auto it = components.cbegin(); it != components.cend(); ++it) {
        QJsonObject c;
        c["version"] = it->version;
        c["files"] = QJsonArray::fromStringList(it->files);
        comps[it.key()] = c;
    }
    o["components"] = comps;

    QJsonObject s;
    s["locale"] = settings.locale;
    s["gamemode"] = settings.gamemode;
    s["mangohud"] = settings.mangohud;
    QJsonObject env;
    for (auto it = settings.env.cbegin(); it != settings.env.cend(); ++it)
        env[it.key()] = it.value();
    s["env"] = env;
    o["settings"] = s;

    QJsonArray progs;
    for (const Program& pr : programs)
        {
        QJsonObject po{{"name", pr.name}, {"path", pr.path}};
        if (pr.automatic)
            po["auto"] = true;
        progs.append(po);
    }
    o["programs"] = progs;
    o["hiddenPrograms"] = QJsonArray::fromStringList(hiddenPrograms);

    // Atomic write: a crash in the middle must not leave a half-written cask.json.
    const QByteArray data = QJsonDocument(o).toJson(QJsonDocument::Indented);
    QSaveFile f(path + "/" + kMetaFile);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit();
}

std::optional<Prefix> Prefix::load(const QString& dir)
{
    QFile f(dir + "/" + kMetaFile);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return std::nullopt;
    const QJsonObject o = doc.object();

    Prefix p;
    p.path = QDir::cleanPath(dir);
    p.name = QFileInfo(p.path).fileName();  // name = folder name
    p.runner = o.value("runner").toString("system");
    p.arch = o.value("arch").toString("win64");
    p.created = QDateTime::fromString(o.value("created").toString(), Qt::ISODate);

    const QJsonObject comps = o.value("components").toObject();
    for (auto it = comps.begin(); it != comps.end(); ++it) {
        const QJsonObject c = it.value().toObject();
        AppliedComponent a;
        a.version = c.value("version").toString();
        for (const QJsonValue& v : c.value("files").toArray())
            a.files << v.toString();
        p.components.insert(it.key(), a);
    }

    const QJsonObject s = o.value("settings").toObject();
    p.settings.locale = s.value("locale").toString();
    p.settings.gamemode = s.value("gamemode").toBool(false);
    p.settings.mangohud = s.value("mangohud").toBool(false);
    const QJsonObject env = s.value("env").toObject();
    for (auto it = env.begin(); it != env.end(); ++it) {
        // If cask.json was edited by hand: variables managed by Cask are dropped.
        if (LaunchSettings::isValidEnvName(it.key()) && !LaunchSettings::isReservedEnvName(it.key()))
            p.settings.env.insert(it.key(), it.value().toString());
    }

    for (const QJsonValue& v : o.value("programs").toArray()) {
        const QJsonObject po = v.toObject();
        const QString path = po.value("path").toString();
        if (path.isEmpty())
            continue;
        p.programs.append({po.value("name").toString(QFileInfo(path).fileName()), path, po.value("auto").toBool()});
    }
    for (const QJsonValue& v : o.value("hiddenPrograms").toArray())
        if (!v.toString().isEmpty())
            p.hiddenPrograms << v.toString();
    return p;
}
