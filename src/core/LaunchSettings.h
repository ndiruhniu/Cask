#pragma once

#include <QMap>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

// Launch settings, stored separately for each prefix (cask.json -> "settings").
// Cask leaves synchronization (esync/fsync/ntsync) alone: modern Wine picks ntsync itself.
struct PrefixSettings {
    QString locale;                          // empty = system language, otherwise e.g. ru_RU.UTF-8
    bool gamemode = false;                   // run programs via gamemoderun
    bool mangohud = false;                   // run programs via mangohud
    bool discreteGpu = false;                // use the discrete GPU of a laptop with two graphics cards
    QMap<QString, QString> env;              // custom environment variables

    bool operator==(const PrefixSettings& o) const
    {
        return locale == o.locale && gamemode == o.gamemode && mangohud == o.mangohud && discreteGpu == o.discreteGpu
               && env == o.env;
    }
    bool operator!=(const PrefixSettings& o) const { return !(*this == o); }
};

namespace LaunchSettings {

QStringList localePresets();

bool isValidEnvName(const QString& name);

// Variables managed by Cask itself. Custom variables do not override them.
const QStringList& reservedEnvNames();
bool isReservedEnvName(const QString& name);

// LANG and LC_ALL: these are what Wine reads to choose its language.
void applyLocale(QProcessEnvironment& env, const QString& locale);

// Asks the system to use the discrete GPU: NVIDIA PRIME render offload with the proprietary NVIDIA driver,
// DRI_PRIME=1 (Mesa: AMD, Intel, Nouveau) otherwise.
void applyDiscreteGpu(QProcessEnvironment& env);

QString gamemodeBinary();    // path to gamemoderun or empty
QString mangohudBinary();    // path to mangohud or empty

// Portable MangoHud (64-bit only): data/mangohud/lib64/libMangoHud{,_shim,_opengl}.so
QString mangohudDir();       // <data>/mangohud
bool hasPortableMangohud();  // whether data/mangohud/lib64/libMangoHud_shim.so and libMangoHud.so exist
// Adds to env: MANGOHUD=1, LD_PRELOAD with the shim (for OpenGL) and the Vulkan layer path
// (a layer with an absolute library path is created in <data>/mangohud/layer on every call).
bool applyPortableMangohud(QProcessEnvironment& env);

// If <data>/mangohud/MangoHud.conf exists and MANGOHUD_CONFIGFILE is unset, points it there.
// Works with the system mangohud too. Returns true if the variable was set.
QString mangohudConfigPath();
bool applyMangohudConfig(QProcessEnvironment& env);

}  // namespace LaunchSettings
