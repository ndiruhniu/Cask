#include "PrefixManager.h"

#include "Component.h"
#include "Config.h"
#include "Program.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <climits>
#include <csignal>
#include <sys/types.h>

namespace {

const char* kBak = ".cask-bak";

void setErr(QString* err, const QString& msg)
{
    if (err)
        *err = msg;
}

QStringList overrideNames(const QStringList& files)
{
    QSet<QString> s;
    for (const QString& f : files)
        s.insert(QFileInfo(f).completeBaseName().toLower());
    QStringList l = s.values();
    l.sort();
    return l;
}

// Recursive removal that does NOT follow symlinks
// (in a prefix, dosdevices/z: points to "/").
bool removeTree(const QString& path)
{
    const QFileInfo fi(path);
    if (fi.isSymLink() || !fi.isDir())
        return QFile::remove(path);
    bool ok = true;
    // Games sometimes leave read-only directories behind; they cannot be emptied otherwise.
    QFile::setPermissions(path, QFile::permissions(path) | QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QDir d(path);
    const auto entries = d.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo& e : entries)
        ok = removeTree(e.absoluteFilePath()) && ok;
    return QDir().rmdir(path) && ok;
}

// Adds a DLL override to WINEDLLOVERRIDES, keeping what is already set there.
void addDllOverride(QProcessEnvironment& e, const QString& entry)
{
    const QString cur = e.value(QStringLiteral("WINEDLLOVERRIDES"));
    if (cur.split(QLatin1Char(';')).contains(entry))
        return;
    e.insert(QStringLiteral("WINEDLLOVERRIDES"), cur.isEmpty() ? entry : cur + QLatin1Char(';') + entry);
}

// A prefix folder: cask.json is removed last, so a prefix that could not be deleted completely stays in
// the list (it is recognized by cask.json) and the deletion can be repeated.
bool removePrefixDir(const QString& path)
{
    bool ok = true;
    const auto entries =
        QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo& e : entries)
        if (e.fileName() != QLatin1String("cask.json"))
            ok = removeTree(e.absoluteFilePath()) && ok;
    if (!ok)
        return false;
    return removeTree(path);
}

}  // namespace

PrefixManager::PrefixManager(QObject* parent) : QObject(parent) {}

QList<Prefix> PrefixManager::list() const
{
    QList<Prefix> out;
    const QDir d(Config::instance().prefixesDir());
    const QStringList names = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    for (const QString& n : names)
        if (auto p = Prefix::load(d.filePath(n)))
            out.append(*p);

    // User order; prefixes not in it come after, alphabetically.
    const QStringList ord = order();
    auto pos = [&ord](const Prefix& p) {
        const int i = ord.indexOf(p.name);
        return i < 0 ? INT_MAX : i;
    };
    std::stable_sort(out.begin(), out.end(), [&](const Prefix& a, const Prefix& b) { return pos(a) < pos(b); });
    return out;
}

QStringList PrefixManager::order() const
{
    QFile f(Config::instance().prefixesDir() + "/.cask-order.json");
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QStringList out;
    for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).array())
        if (v.isString())
            out << v.toString();
    return out;
}

bool PrefixManager::setOrder(const QStringList& names)
{
    const QByteArray data = QJsonDocument(QJsonArray::fromStringList(names)).toJson(QJsonDocument::Indented);
    QSaveFile f(Config::instance().prefixesDir() + "/.cask-order.json");
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit();
}

int PrefixManager::unlinkUserDirs(const QString& prefixPath)
{
    const QDir users(prefixPath + "/drive_c/users");
    if (!users.exists())
        return -1;

    const QString root = QDir::cleanPath(prefixPath);
    const QString canon = QFileInfo(prefixPath).canonicalFilePath();
    auto inside = [&](const QString& target) {
        const QString t = QDir::cleanPath(target);
        return t.startsWith(root + "/") || (!canon.isEmpty() && t.startsWith(canon + "/"));
    };

    int replaced = 0;
    const auto profiles = users.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& profile : profiles) {
        if (profile.isSymLink())
            continue;
        const QDir pd(profile.absoluteFilePath());
        const auto entries = pd.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const QFileInfo& e : entries) {
            // Links inside the prefix (e.g. "My Documents" -> "Documents") are left alone.
            if (!e.isSymLink() || inside(e.symLinkTarget()))
                continue;
            if (QFile::remove(e.absoluteFilePath())) {  // removes the link itself, not its target
                QDir().mkpath(e.absoluteFilePath());
                ++replaced;
            }
        }
    }
    return replaced;
}

QString PrefixManager::wineserverPath(const Runner& r)
{
    if (!r.system)
        return r.binDir + "/wineserver";
    const QString found = QStandardPaths::findExecutable(QStringLiteral("wineserver"));
    return found.isEmpty() ? QStringLiteral("wineserver") : found;
}

// True if the process executable is a Wine binary (wine, wine64, wine-preloader, wineserver...).
// Only such processes are killed: a shell or terminal that merely has WINEPREFIX exported must survive.
static bool isWineProcess(qint64 pid)
{
    const QString exe = QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).symLinkTarget();
    return QFileInfo(exe).fileName().startsWith(QLatin1String("wine"), Qt::CaseInsensitive);
}

QList<qint64> PrefixManager::processes(const QString& prefixPath)
{
    // Any process whose environment has WINEPREFIX pointing to this prefix:
    // programs launched from Cask, wineserver, winetricks, and anything started from a terminal.
    QList<QByteArray> wanted{"WINEPREFIX=" + QDir::cleanPath(prefixPath).toLocal8Bit()};
    const QString canon = QFileInfo(prefixPath).canonicalFilePath();
    if (!canon.isEmpty() && canon != QDir::cleanPath(prefixPath))
        wanted << "WINEPREFIX=" + canon.toLocal8Bit();

    QList<qint64> out;
    const qint64 self = QCoreApplication::applicationPid();
    const QStringList entries = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& e : entries) {
        bool ok = false;
        const qint64 pid = e.toLongLong(&ok);
        if (!ok || pid == self)
            continue;
        QFile f(QStringLiteral("/proc/%1/environ").arg(pid));
        if (!f.open(QIODevice::ReadOnly))
            continue;  // someone else's process or already exited
        const QList<QByteArray> vars = f.readAll().split('\0');
        for (const QByteArray& v : vars) {
            if (wanted.contains(v)) {
                if (isWineProcess(pid))
                    out << pid;
                break;
            }
        }
    }
    return out;
}

int PrefixManager::killProcesses(const QString& prefixPath)
{
    int n = 0;
    for (qint64 pid : processes(prefixPath))
        if (::kill(static_cast<pid_t>(pid), SIGKILL) == 0)
            ++n;
    return n;
}

int PrefixManager::stopProcesses(const Prefix& p)
{
    const int before = processes(p.path).size();
    if (before == 0)
        return 0;

    // Politely first: wineserver -k terminates all prefix processes.
    if (const auto r = RunnerManager::find(p.runner)) {
        QProcess k;
        k.setProcessEnvironment(makeEnv(p, *r, false));
        k.start(wineserverPath(*r), {"-k"});
        k.waitForFinished(5000);
    }
    // wineserver -k only broadcasts the signal: allow up to 3 seconds, then finish off the survivors.
    QElapsedTimer t;
    t.start();
    while (!processes(p.path).isEmpty() && t.elapsed() < 3000)
        QThread::msleep(100);
    killProcesses(p.path);
    return before;
}

bool PrefixManager::stopAll(const QString& name, QString* err)
{
    const auto p = find(name);
    if (!p) {
        setErr(err, tr("Prefix not found."));
        return false;
    }
    const int n = stopProcesses(*p);
    emit message(n == 0 ? tr("No running processes in prefix '%1'").arg(name)
                        : tr("All processes of prefix '%1' stopped").arg(name),
                 true);
    return true;
}

std::optional<Prefix> PrefixManager::find(const QString& name) const
{
    if (name.isEmpty() || name.contains('/') || name.startsWith('.'))
        return std::nullopt;
    return Prefix::load(Config::instance().prefixesDir() + "/" + name);
}

QProcessEnvironment PrefixManager::makeEnv(const Prefix& p, const Runner& r, bool forLaunch)
{
    QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
    e.insert("WINEPREFIX", p.path);
    e.insert("WINEARCH", p.arch);
    if (!r.system)
        e.insert("PATH", r.binDir + ":" + e.value("PATH"));
    if (!e.contains("WINEDEBUG"))
        e.insert("WINEDEBUG", "-all");

    // The shader cache lives inside the prefix folder (as in Bottles):
    //  <prefix>/cache/{dxvk_shader,mesa_shader,vkd3d_shader}
    // DXVK_STATE_CACHE_PATH: DXVK state cache before 2.7 (removed in 2.7); D7VK, as a fork,
    // may use it. DXVK_SHADER_CACHE_PATH: DXVK 3.0+ shader cache.
    const QString cache = p.path + "/cache";
    const QString dxvk = cache + "/dxvk_shader";
    const QString mesa = cache + "/mesa_shader";
    const QString vkd3d = cache + "/vkd3d_shader";
    QDir().mkpath(dxvk);
    QDir().mkpath(mesa);
    QDir().mkpath(vkd3d);
    e.insert("DXVK_STATE_CACHE_PATH", dxvk);
    e.insert("DXVK_SHADER_CACHE_PATH", dxvk);
    e.insert("MESA_SHADER_CACHE_DIR", mesa);
    e.insert("VKD3D_SHADER_CACHE_PATH", vkd3d);

    // The language Wine sees. Cask does not touch sync (ntsync etc.).
    LaunchSettings::applyLocale(e, p.settings.locale);

    // Wine must not put its own or the programs' shortcuts into the user's menu and file associations
    // (~/.local/share/applications/wine, mimeinfo.cache): the programs are listed in Cask itself.
    if (!forLaunch) {
        addDllOverride(e, QStringLiteral("winemenubuilder.exe=d"));
        return e;
    }

    // The prefix's own variables. Variables managed by Cask (WINEPREFIX, PATH, language,
    // cache paths...) are not overridden by them. The rest, including WINEDEBUG (Cask only sets
    // a default value for it), are applied.
    for (auto it = p.settings.env.cbegin(); it != p.settings.env.cend(); ++it) {
        if (LaunchSettings::isValidEnvName(it.key()) && !LaunchSettings::isReservedEnvName(it.key()))
            e.insert(it.key(), it.value());
    }
    addDllOverride(e, QStringLiteral("winemenubuilder.exe=d"));  // also if the prefix sets its own overrides
    return e;
}

bool PrefixManager::checkIdle(QString* err) const
{
    if (!isBusy())
        return true;
    setErr(err, tr("Wait for the current operation to finish."));
    return false;
}

void PrefixManager::startJob(const Prefix&, const Runner& r, const QStringList& args,
                             const QProcessEnvironment& env, const QString& okMsg,
                             std::function<void(bool)> after, const QString& workDir)
{
    auto* proc = new QProcess(this);
    proc->setProcessEnvironment(env);
    proc->setProgram(r.wine);
    proc->setArguments(args);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    if (!workDir.isEmpty())
        proc->setWorkingDirectory(workDir);

    auto finish = [this, proc, after](bool ok, const QString& msg) {
        emit message(msg, ok);
        if (after)
            after(ok);
        proc->deleteLater();
        if (--m_jobs == 0)
            emit busyChanged(false);
    };

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, proc, okMsg, finish](int code, QProcess::ExitStatus st) {
                const bool ok = st == QProcess::NormalExit && code == 0;
                QString msg = okMsg;
                if (!ok) {
                    const QString out = QString::fromLocal8Bit(proc->readAll()).trimmed().right(500);
                    msg = tr("Wine error (code %1). %2").arg(code).arg(out);
                }
                finish(ok, msg);
            });
    connect(proc, &QProcess::errorOccurred, this, [proc, finish, this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            finish(false, tr("Failed to start %1").arg(proc->program()));
    });

    if (++m_jobs == 1)
        emit busyChanged(true);
    proc->start();
}

bool PrefixManager::create(const QString& name, const QString& runner, const QString& arch, QString* err)
{
    if (!checkIdle(err))
        return false;
    static const QRegularExpression re(QStringLiteral("^[\\p{L}\\p{N}][\\p{L}\\p{N} _.\\-]{0,63}\\z"));
    if (!re.match(name).hasMatch()) {
        setErr(err, tr("Invalid name. Letters, digits, space, _ . - are allowed (up to 64 characters)."));
        return false;
    }
    if (arch != "win64" && arch != "win32") {
        setErr(err, tr("Unknown architecture: %1").arg(arch));
        return false;
    }
    const auto r = RunnerManager::find(runner);
    if (!r) {
        setErr(err, tr("Wine '%1' was not found.").arg(runner));
        return false;
    }

    const QString path = Config::instance().prefixesDir() + "/" + name;
    if (QFileInfo::exists(path)) {
        setErr(err, tr("A prefix with this name already exists."));
        return false;
    }
    if (!QDir().mkpath(path)) {
        setErr(err, tr("Could not create folder %1").arg(path));
        return false;
    }

    Prefix p;
    p.name = name;
    p.path = path;
    p.runner = runner;
    p.arch = arch;
    p.created = QDateTime::currentDateTime();
    if (!p.save()) {
        QDir().rmdir(path);
        setErr(err, tr("Could not write cask.json"));
        return false;
    }
    emit changed();

    QProcessEnvironment env = makeEnv(p, *r, false);
    addDllOverride(env, QStringLiteral("mscoree,mshtml=d"));  // no Mono/Gecko dialogs
    startJob(p, *r, {"wineboot", "--init"}, env, tr("Prefix '%1' created").arg(name),
             [this, path](bool ok) {
                 if (ok)
                     unlinkUserDirs(path);  // Desktop, Documents... must not link to the home folder
                 emit changed();
             });
    return true;
}

bool PrefixManager::remove(const QString& name, QString* err)
{
    const auto p = find(name);
    if (!p) {
        setErr(err, tr("Prefix not found."));
        return false;
    }
    if (!checkIdle(err))
        return false;
    const QString root = QFileInfo(Config::instance().prefixesDir()).canonicalFilePath();
    const QString canon = QFileInfo(p->path).canonicalFilePath();
    if (root.isEmpty() || !canon.startsWith(root + "/")) {
        setErr(err, tr("The prefix path is outside the prefixes folder; deletion cancelled."));
        return false;
    }

    // Stop all prefix processes so nothing is left in the folder being deleted.
    stopProcesses(*p);

    if (!removePrefixDir(p->path)) {
        setErr(err, tr("Not everything could be removed from %1").arg(p->path));
        emit changed();
        return false;
    }
    QStringList ord = order();
    if (ord.removeAll(name) > 0)
        setOrder(ord);
    emit changed();
    emit message(tr("Prefix '%1' removed").arg(name), true);
    return true;
}

bool PrefixManager::setRunner(const QString& name, const QString& runner, QString* err)
{
    if (!checkIdle(err))
        return false;
    auto p = find(name);
    if (!p) {
        setErr(err, tr("Prefix not found."));
        return false;
    }
    const auto r = RunnerManager::find(runner);
    if (!r) {
        setErr(err, tr("Wine '%1' was not found.").arg(runner));
        return false;
    }
    // A wineserver of the previous Wine build would refuse to talk to the new one.
    stopProcesses(*p);
    p->runner = runner;
    if (!p->save()) {
        setErr(err, tr("Could not write cask.json"));
        return false;
    }
    emit changed();
    startJob(*p, *r, {"wineboot", "-u"}, makeEnv(*p, *r, false),
             tr("Wine of prefix '%1' changed to %2").arg(name, runner), [this, path = p->path](bool ok) {
                 if (ok)
                     unlinkUserDirs(path);  // wineboot -u may have recreated the links
                 emit changed();
             });
    return true;
}

// Rolls back the component's files. Returns the override names that are no longer
// needed by anyone (they must be removed from the registry).
QStringList PrefixManager::restoreFiles(Prefix& p, const QString& type)
{
    auto it = p.components.find(type);
    if (it == p.components.end())
        return {};

    QStringList names = overrideNames(it->files);
    const QString win = p.windowsDir();
    for (const QString& rel : std::as_const(it->files)) {
        const QString dst = win + "/" + rel;
        QFile::remove(dst);
        if (QFile::exists(dst + kBak))
            QFile::rename(dst + kBak, dst);
    }
    p.components.erase(it);

    QSet<QString> stillUsed;
    for (const AppliedComponent& c : std::as_const(p.components))
        for (const QString& n : overrideNames(c.files))
            stillUsed.insert(n);

    QStringList result;
    for (const QString& n : names)
        if (!stillUsed.contains(n))
            result << n;
    return result;
}

void PrefixManager::importOverrides(const Prefix& p, const Runner& r, const QStringList& setNative,
                                    const QStringList& removeNames, const QString& okMsg)
{
    if (setNative.isEmpty() && removeNames.isEmpty()) {
        emit message(okMsg, true);
        return;
    }

    QByteArray data = "Windows Registry Editor Version 5.00\r\n\r\n"
                      "[HKEY_CURRENT_USER\\Software\\Wine\\DllOverrides]\r\n";
    for (const QString& n : setNative)
        data += "\"" + n.toUtf8() + "\"=\"native,builtin\"\r\n";
    for (const QString& n : removeNames)
        data += "\"" + n.toUtf8() + "\"=-\r\n";

    // Pass the file by relative name with workDir = prefix: more reliable
    // than a unix path in regedit.
    const QString regName = QStringLiteral("cask-overrides.reg");
    const QString regPath = p.path + "/" + regName;
    QFile f(regPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit message(tr("Could not write %1").arg(regPath), false);
        return;
    }
    f.write(data);
    f.close();

    startJob(p, r, {"regedit", "/S", regName}, makeEnv(p, r, false), okMsg,
             [regPath](bool) { QFile::remove(regPath); }, p.path);
}

bool PrefixManager::applyComponent(const QString& name, const QString& type, const QString& version, QString* err)
{
    if (!checkIdle(err))
        return false;
    auto po = find(name);
    if (!po) {
        setErr(err, tr("Prefix not found."));
        return false;
    }
    Prefix p = *po;
    const auto r = RunnerManager::find(p.runner);
    if (!r) {
        setErr(err, tr("The Wine of this prefix ('%1') was not found.").arg(p.runner));
        return false;
    }
    const Components::Type* t = Components::find(type);
    if (!t) {
        setErr(err, tr("Unknown component: %1").arg(type));
        return false;
    }
    const QString srcRoot = Config::instance().componentsDir(type) + "/" + version;
    if (!QDir(srcRoot).exists()) {
        setErr(err, tr("Folder not found: %1").arg(srcRoot));
        return false;
    }
    const QString win = p.windowsDir();
    if (!QDir(win + "/system32").exists()) {
        setErr(err, tr("The prefix is not initialized yet (no drive_c/windows/system32)."));
        return false;
    }

    // Copy plan: (source file, path relative to windows/)
    struct Item { QString src; QString rel; };
    struct Map { QStringList srcDirs; QString dst; };
    QList<Map> maps;
    if (p.arch == "win64")
        maps = {{t->dirs64, "system32"}, {t->dirs32, "syswow64"}};
    else
        maps = {{t->dirs32, "system32"}};

    QList<Item> plan;
    for (const Map& m : std::as_const(maps)) {
        for (const QString& d : m.srcDirs) {
            const QDir sd(srcRoot + "/" + d);
            if (!sd.exists())
                continue;
            for (const QString& dll : sd.entryList({"*.dll"}, QDir::Files))
                plan.append({sd.filePath(dll), m.dst + "/" + dll});
            break;  // take the first existing subfolder
        }
    }
    if (plan.isEmpty()) {
        setErr(err, tr("No DLLs found in %1 (subfolders x64 and x32/x86 are expected).").arg(srcRoot));
        return false;
    }

    // Roll back the old version of this same component.
    const QStringList staleOverrides = restoreFiles(p, type);

    AppliedComponent a;
    a.version = version;
    for (const Item& it : std::as_const(plan)) {
        const QString dst = win + "/" + it.rel;
        if (QFile::exists(dst) && !QFile::exists(dst + kBak))
            QFile::rename(dst, dst + kBak);  // keep the original once
        QFile::remove(dst);
        a.files << it.rel;  // before the copy: a failed copy is rolled back with this file too
        if (!QFile::copy(it.src, dst)) {
            p.components.insert(type, a);
            restoreFiles(p, type);
            p.save();
            setErr(err, tr("Could not copy %1").arg(it.src));
            emit changed();
            return false;
        }
        QFile::setPermissions(dst, QFile::permissions(dst) | QFile::WriteOwner);
        // the file now belongs to this component (the last one applied wins)
        for (auto c = p.components.begin(); c != p.components.end(); ++c)
            c->files.removeAll(it.rel);
    }
    p.components.insert(type, a);
    if (!p.save()) {
        setErr(err, tr("Could not write cask.json"));
        return false;
    }
    emit changed();

    const QStringList setNative = overrideNames(a.files);
    QStringList toRemove;
    for (const QString& n : staleOverrides)
        if (!setNative.contains(n))
            toRemove << n;
    importOverrides(p, *r, setNative, toRemove,
                    tr("%1 %2 applied to '%3'").arg(t->title, version, name));
    return true;
}

bool PrefixManager::removeComponent(const QString& name, const QString& type, QString* err)
{
    if (!checkIdle(err))
        return false;
    auto po = find(name);
    if (!po) {
        setErr(err, tr("Prefix not found."));
        return false;
    }
    Prefix p = *po;
    if (!p.components.contains(type)) {
        setErr(err, tr("Component is not applied."));
        return false;
    }
    const auto r = RunnerManager::find(p.runner);
    if (!r) {
        setErr(err, tr("The Wine of this prefix ('%1') was not found.").arg(p.runner));
        return false;
    }
    const QStringList drop = restoreFiles(p, type);
    if (!p.save()) {
        setErr(err, tr("Could not write cask.json"));
        return false;
    }
    emit changed();
    const Components::Type* t = Components::find(type);
    importOverrides(p, *r, {}, drop, tr("%1 removed from '%2'").arg(t ? t->title : type, name));
    return true;
}

bool PrefixManager::prepareLaunch(const QString& name, const QStringList& wineArgs, bool game, LaunchSpec* out,
                                  QString* err)
{
    const auto p = find(name);
    if (!p) {
        setErr(err, tr("Prefix not found."));
        return false;
    }
    const auto r = RunnerManager::find(p->runner);
    if (!r) {
        setErr(err, tr("The Wine of this prefix ('%1') was not found.").arg(p->runner));
        return false;
    }

    // Wrappers: gamemoderun mangohud wine ...
    QStringList chain;
    if (game && p->settings.gamemode) {
        const QString gm = LaunchSettings::gamemodeBinary();
        if (!gm.isEmpty())
            chain << gm;
        else
            emit message(tr("GameMode is enabled for this prefix, but gamemoderun was not found. "
                            "Launching without it."),
                         true);
    }
    bool portableHud = false;
    if (game && p->settings.mangohud) {
        if (LaunchSettings::hasPortableMangohud()) {
            portableHud = true;  // the environment is added below, no wrapper needed
        } else {
            const QString mh = LaunchSettings::mangohudBinary();
            if (!mh.isEmpty())
                chain << mh;
            else
                emit message(tr("MangoHud is enabled for this prefix, but mangohud was not found. "
                                "Launching without it."),
                             true);
        }
    }
    chain << r->wine;
    out->program = chain.takeFirst();
    out->args = chain + wineArgs;

    out->env = makeEnv(*p, *r, true);
    if (portableHud && !LaunchSettings::applyPortableMangohud(out->env))
        emit message(tr("Could not set up the bundled MangoHud in %1. Launching without it.")
                         .arg(LaunchSettings::mangohudDir()),
                     true);
    if (game && p->settings.mangohud)
        LaunchSettings::applyMangohudConfig(out->env);
    return true;
}

bool PrefixManager::launch(const QString& name, const QStringList& wineArgs, const QString& workDir, QString* err,
                           bool game)
{
    LaunchSpec spec;
    if (!prepareLaunch(name, wineArgs, game, &spec, err))
        return false;

    QProcess proc;
    proc.setProgram(spec.program);
    proc.setArguments(spec.args);
    proc.setProcessEnvironment(spec.env);
    if (!workDir.isEmpty())
        proc.setWorkingDirectory(workDir);
    if (!proc.startDetached()) {
        setErr(err, tr("Failed to start %1").arg(spec.program));
        return false;
    }
    return true;
}

namespace {

QString shellQuote(const QString& s)
{
    QString q = s;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

// A terminal emulator and the arguments that put "sh -c <script>" into it.
struct Terminal {
    QString program;
    QStringList args;
};

std::optional<Terminal> findTerminal(const QString& script)
{
    struct Known {
        const char* exe;
        QStringList before;  // between the program and "sh -c script"
    };
    static const QList<Known> known{
        {"gnome-terminal", {"--"}},       {"kgx", {"-e"}},       {"konsole", {"-e"}},
        {"xfce4-terminal", {"-x"}},       {"mate-terminal", {"-x"}}, {"tilix", {"-e"}},
        {"lxterminal", {"-e"}},           {"terminator", {"-x"}},    {"alacritty", {"-e"}},
        {"kitty", {}},                    {"foot", {}},              {"wezterm", {"start", "--"}},
        {"x-terminal-emulator", {"-e"}},  {"xterm", {"-e"}},         {"urxvt", {"-e"}},
    };
    const QStringList sh{QStringLiteral("sh"), QStringLiteral("-c"), script};

    // The user's choice first, then the one that fits the desktop, then the rest.
    const QString custom = qEnvironmentVariable("TERMINAL");
    if (!custom.isEmpty()) {
        const QString exe = QStandardPaths::findExecutable(custom);
        if (!exe.isEmpty())
            return Terminal{exe, QStringList{QStringLiteral("-e")} + sh};
    }
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
    QStringList order;
    if (desktop.contains(QLatin1String("kde")))
        order << "konsole";
    else if (desktop.contains(QLatin1String("gnome")))
        order << "gnome-terminal" << "kgx";
    else if (desktop.contains(QLatin1String("xfce")))
        order << "xfce4-terminal";
    for (const Known& k : known)
        if (!order.contains(QLatin1String(k.exe)))
            order << QLatin1String(k.exe);
    for (const QString& name : std::as_const(order)) {
        const QString exe = QStandardPaths::findExecutable(name);
        if (exe.isEmpty())
            continue;
        for (const Known& k : known)
            if (name == QLatin1String(k.exe))
                return Terminal{exe, k.before + sh};
    }
    return std::nullopt;
}

}  // namespace

bool PrefixManager::launchInTerminal(const QString& name, const QStringList& wineArgs, const QString& workDir,
                                     QString* err)
{
    LaunchSpec spec;
    if (!prepareLaunch(name, wineArgs, true, &spec, err))
        return false;

    // The terminal may be started by a server that does not pass our environment on (gnome-terminal), so
    // the variables that differ from the current ones are set in the command itself.
    const QProcessEnvironment sys = QProcessEnvironment::systemEnvironment();
    QStringList cmd{QStringLiteral("env")};
    for (const QString& key : spec.env.keys())
        if (spec.env.value(key) != sys.value(key))
            cmd << shellQuote(key + QLatin1Char('=') + spec.env.value(key));
    cmd << shellQuote(spec.program);
    for (const QString& a : std::as_const(spec.args))
        cmd << shellQuote(a);

    QString script;
    if (!workDir.isEmpty())
        script += QStringLiteral("cd %1 2>/dev/null; ").arg(shellQuote(workDir));
    script += cmd.join(QLatin1Char(' '));
    script += QStringLiteral("; code=$?; printf '\\n%s %s\\n' %1 \"$code\"; printf '%s\\n' %2; read -r _")
                  .arg(shellQuote(tr("Exit code:")), shellQuote(tr("Press Enter to close this window.")));

    const auto term = findTerminal(script);
    if (!term) {
        setErr(err, tr("No terminal emulator was found. Install one (for example konsole, gnome-terminal or "
                       "xterm) or set the TERMINAL environment variable."));
        return false;
    }
    QProcess proc;
    proc.setProgram(term->program);
    proc.setArguments(term->args);
    proc.setProcessEnvironment(spec.env);
    if (!workDir.isEmpty())
        proc.setWorkingDirectory(workDir);
    if (!proc.startDetached()) {
        setErr(err, tr("Failed to start %1").arg(term->program));
        return false;
    }
    return true;
}

bool PrefixManager::setPrograms(const QString& name, const QList<Program>& programs, const QStringList& hidden,
                                QString* err)
{
    if (!checkIdle(err))
        return false;
    auto p = find(name);
    if (!p) {
        setErr(err, tr("Prefix not found."));
        return false;
    }
    if (p->programs == programs && p->hiddenPrograms == hidden)
        return true;
    p->programs = programs;
    p->hiddenPrograms = hidden;
    if (!p->save()) {
        setErr(err, tr("Could not write cask.json"));
        return false;
    }
    return true;
}

bool PrefixManager::syncPrograms(const QString& name)
{
    if (isBusy())
        return false;
    auto p = find(name);
    if (!p)
        return false;

    bool changed = false;
    // A hidden program whose file is gone (it was uninstalled) is forgotten too: if it is installed again,
    // it appears in the list again. Only files inside the prefix are checked (another drive may be unmounted).
    if (QFileInfo(p->path + "/drive_c").isDir()) {
        for (int i = p->hiddenPrograms.size() - 1; i >= 0; --i) {
            const QString& h = p->hiddenPrograms.at(i);
            if (!QDir::isAbsolutePath(h) && !QFileInfo::exists(Programs::resolve(p->path, h))) {
                p->hiddenPrograms.removeAt(i);
                changed = true;
            }
        }
    }

    QSet<QString> known;
    for (const Program& pr : std::as_const(p->programs))
        known.insert(Programs::resolve(p->path, pr.path).toLower());
    for (const QString& h : std::as_const(p->hiddenPrograms))
        known.insert(Programs::resolve(p->path, h).toLower());

    // Programs found automatically disappear when their file does (the program was uninstalled).
    // Only files inside the prefix are checked: a program on another drive may just be unmounted.
    if (QFileInfo(p->path + "/drive_c").isDir()) {
        for (int i = p->programs.size() - 1; i >= 0; --i) {
            const Program& pr = p->programs.at(i);
            if (pr.automatic && !QDir::isAbsolutePath(pr.path)
                && !QFileInfo::exists(Programs::resolve(p->path, pr.path))) {
                p->programs.removeAt(i);
                changed = true;
            }
        }
    }
    for (const QString& file : Programs::discover(p->path)) {
        if (known.contains(file.toLower()))
            continue;
        known.insert(file.toLower());
        p->programs.append({QFileInfo(file).fileName(), Programs::store(p->path, file), true});
        changed = true;
    }
    return changed && p->save();
}

QList<qint64> PrefixManager::imageProcesses(const QString& prefixPath, const QString& imageName)
{
    QList<qint64> out;
    for (qint64 pid : processes(prefixPath)) {
        QFile f(QStringLiteral("/proc/%1/cmdline").arg(pid));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        // Wine shows the Windows path of the program as the process name ("C:\Program Files\7-Zip\7zFM.exe");
        // any argument that ends with the image name is taken as a match.
        for (const QByteArray& arg : f.readAll().split('\0')) {
            const QString a = QString::fromLocal8Bit(arg);
            const QString base = a.mid(qMax(a.lastIndexOf(QLatin1Char('\\')), a.lastIndexOf(QLatin1Char('/'))) + 1);
            if (!base.isEmpty() && base.toLower() == imageName) {
                out << pid;
                break;
            }
        }
    }
    return out;
}

QStringList PrefixManager::runningImages(const QString& prefixPath)
{
    QSet<QString> names;
    for (qint64 pid : processes(prefixPath)) {
        QFile f(QStringLiteral("/proc/%1/cmdline").arg(pid));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        for (const QByteArray& arg : f.readAll().split('\0')) {
            const QString a = QString::fromLocal8Bit(arg);
            const QString base = a.mid(qMax(a.lastIndexOf(QLatin1Char('\\')), a.lastIndexOf(QLatin1Char('/'))) + 1);
            if (!base.isEmpty())
                names.insert(base.toLower());
        }
    }
    return names.values();
}

int PrefixManager::stopProgram(const QString& prefixPath, const QString& imageName)
{
    const QList<qint64> pids = imageProcesses(prefixPath, imageName);
    int n = 0;
    for (qint64 pid : pids)
        if (::kill(static_cast<pid_t>(pid), SIGTERM) == 0)
            ++n;
    if (n > 0) {
        // What ignores SIGTERM is finished off after a while (only processes that still belong to the program).
        QTimer::singleShot(3000, this, [prefixPath, imageName, pids] {
            // Only the processes that were asked to stop: a new instance started meanwhile is left alone.
            for (qint64 pid : imageProcesses(prefixPath, imageName))
                if (pids.contains(pid))
                    ::kill(static_cast<pid_t>(pid), SIGKILL);
        });
    }
    return n;
}

QStringList PrefixManager::programArgs(const QString& file)
{
    return QFileInfo(file).suffix().compare(QStringLiteral("exe"), Qt::CaseInsensitive) == 0
               ? QStringList{file}
               : QStringList{QStringLiteral("start"), QStringLiteral("/unix"), file};
}

bool PrefixManager::setSettings(const QString& name, const PrefixSettings& in, QString* err)
{
    auto p = find(name);
    if (!p) {
        setErr(err, tr("Prefix not found."));
        return false;
    }

    PrefixSettings s = in;
    s.locale = s.locale.trimmed();
    for (auto it = s.env.cbegin(); it != s.env.cend(); ++it) {
        if (!LaunchSettings::isValidEnvName(it.key())) {
            setErr(err, tr("Invalid variable name: %1").arg(it.key()));
            return false;
        }
        if (LaunchSettings::isReservedEnvName(it.key())) {
            setErr(err, tr("The variable %1 is set by Cask and cannot be changed.").arg(it.key()));
            return false;
        }
    }

    if (s == p->settings)
        return true;
    p->settings = s;
    if (!p->save()) {
        setErr(err, tr("Could not write cask.json"));
        return false;
    }
    emit message(tr("Settings of prefix '%1' saved").arg(name), true);
    return true;
}
