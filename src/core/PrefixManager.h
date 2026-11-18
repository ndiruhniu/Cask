#pragma once

#include "Prefix.h"
#include "Runner.h"

#include <QList>
#include <QObject>
#include <QProcessEnvironment>
#include <functional>
#include <optional>

class PrefixManager : public QObject {
    Q_OBJECT
public:
    explicit PrefixManager(QObject* parent = nullptr);

    // List in the user-defined order (setOrder); new prefixes go at the end.
    QList<Prefix> list() const;
    std::optional<Prefix> find(const QString& name) const;

    // The prefix order is stored next to them: <prefixes dir>/.cask-order.json
    QStringList order() const;
    bool setOrder(const QStringList& names);

    // Replaces the Desktop, Documents, Downloads... symlinks in the Wine profile that point outside
    // the prefix (to the home folder) with plain empty folders. Files behind the links are left alone.
    // Returns the number of replacements, -1 if the profile has not been created yet.
    static int unlinkUserDirs(const QString& prefixPath);

    // Stops all prefix processes: wineserver -k, then SIGKILL for whatever remains.
    bool stopAll(const QString& name, QString* err);
    // PIDs of processes whose WINEPREFIX points to this prefix.
    static QList<qint64> processes(const QString& prefixPath);
    // SIGKILL to all such processes; returns the number successfully signalled.
    static int killProcesses(const QString& prefixPath);
    // wineserver of the selected Wine build.
    static QString wineserverPath(const Runner& r);

    // Creates the prefix folder and runs wineboot --init in the background.
    bool create(const QString& name, const QString& runner, const QString& arch, QString* err);
    bool remove(const QString& name, QString* err);

    // Changes the prefix's Wine and updates it (wineboot -u) in the background.
    bool setRunner(const QString& name, const QString& runner, QString* err);

    // Copies DLLs from data/<type>/<version> into the prefix and sets the overrides.
    bool applyComponent(const QString& name, const QString& type, const QString& version, QString* err);
    bool removeComponent(const QString& name, const QString& type, QString* err);

    // Runs wine in the prefix (detached): a program, winecfg, regedit...
    // game = true: a user program; GameMode and MangoHud are applied to it (if enabled for the prefix).
    bool launch(const QString& name, const QStringList& wineArgs, const QString& workDir, QString* err,
                bool game = false);

    // Same, in a terminal window: the output of the program is visible and the window stays open
    // until Enter is pressed.
    bool launchInTerminal(const QString& name, const QStringList& wineArgs, const QString& workDir, QString* err);

    // The program list of a prefix (cask.json). `hidden` holds the paths that were removed by the user.
    // Does not emit changed(): the window updates its own list.
    bool setPrograms(const QString& name, const QList<Program>& programs, const QStringList& hidden, QString* err);
    // Adds the programs installed in the prefix that are not in the list (and were not removed by the
    // user). Returns true if the list changed. Does nothing while a background job runs.
    bool syncPrograms(const QString& name);
    // Lower-case image names (7zfm.exe) of the Wine processes of the prefix.
    static QStringList runningImages(const QString& prefixPath);
    // Terminates the processes of one program: SIGTERM, then SIGKILL for what is left after 3 seconds.
    // Returns the number of processes signalled.
    int stopProgram(const QString& prefixPath, const QString& imageName);

    // Wine arguments to launch a file: .exe directly, anything else (.msi, .bat, .lnk) via `start /unix`.
    static QStringList programArgs(const QString& file);

    // Saves the prefix launch settings (variables, sync, language, GameMode).
    bool setSettings(const QString& name, const PrefixSettings& settings, QString* err);

    // Environment for wine. forLaunch = false: internal tasks (wineboot, regedit),
    // base, cache and language only. true: user launch, plus the prefix's own variables
    // (they do not override variables managed by Cask).
    static QProcessEnvironment makeEnv(const Prefix& p, const Runner& r, bool forLaunch);

    bool isBusy() const { return m_jobs > 0; }

signals:
    void changed();
    void busyChanged(bool busy);
    void message(const QString& text, bool ok);

private:
    bool checkIdle(QString* err) const;  // false (with an error) while a background job runs
    struct LaunchSpec {
        QString program;
        QStringList args;
        QProcessEnvironment env;
    };
    // The command (with GameMode/MangoHud wrappers when game) and the environment of a launch.
    bool prepareLaunch(const QString& name, const QStringList& wineArgs, bool game, LaunchSpec* out, QString* err);
    static QList<qint64> imageProcesses(const QString& prefixPath, const QString& imageName);
    int stopProcesses(const Prefix& p);  // returns how many processes were running before the stop
    void startJob(const Prefix& p, const Runner& r, const QStringList& args, const QProcessEnvironment& env,
                  const QString& okMsg, std::function<void(bool)> after = {}, const QString& workDir = {});
    void importOverrides(const Prefix& p, const Runner& r, const QStringList& setNative,
                         const QStringList& removeNames, const QString& okMsg);
    QStringList restoreFiles(Prefix& p, const QString& type);

    int m_jobs = 0;
};
