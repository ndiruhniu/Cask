# Cask

A Wine prefix manager written in C++/Qt, in the spirit of Bottles. Wine builds and DXVK/D7VK/VKD3D-Proton
packages are placed manually (there is no downloader yet).

![Cask main window](docs/screenshot.png)

## Portable layout

```
Cask/
├── cask            the binary
├── cask.ini        settings (created next to the binary)
├── lib/            bundled dependencies (Qt etc.), RPATH = $ORIGIN/lib
├── data/
│   ├── runners/<name>/bin/wine      Wine builds
│   ├── d7vk/<version>/{x64,x32}/*.dll
│   ├── dxvk/<version>/{x64,x32}/*.dll
│   ├── vkd3d-proton/<version>/{x64,x86}/*.dll
│   ├── winetricks/                  winetricks script (optional) and cache/
│   └── mangohud/lib64/              portable MangoHud (optional), 64-bit
└── prefixes/<name>/                 WINEPREFIX + cask.json
    └── cache/                       shader caches of the prefix
        ├── dxvk_shader/             DXVK_SHADER_CACHE_PATH (DXVK 3.0+),
        │                            DXVK_STATE_CACHE_PATH (DXVK < 2.7, D7VK)
        ├── mesa_shader/             MESA_SHADER_CACHE_DIR
        └── vkd3d_shader/            VKD3D_SHADER_CACHE_PATH
```

- The root can be overridden with the `CASK_ROOT` environment variable.
- The `data` and `prefixes` folders can be changed in Settings. Paths inside the root are stored as relative.
- A prefix stores no absolute paths (its name is the folder name) and refers to Wine by runner name,
  so everything moves together with the folder.
- The cache variables are set automatically on every Wine launch in a prefix.

## Features

- Create (win64/win32, choice of Wine) and delete prefixes.
- Change the Wine of an existing prefix by picking it in the list: the selected build is applied
  immediately (`wineboot -u`).
- D7VK, DXVK and VKD3D-Proton work like in Bottles: each has its own list, "Disabled" by default. Picking a
  version applies it (DLLs are copied to `system32`/`syswow64`, originals are backed up as `*.cask-bak`,
  DLL overrides are set through `regedit`); picking "Disabled" removes the component and restores the originals.
- Run programs. Tools: Explorer, Control panel, winecfg, regedit, uninstaller, task manager, winetricks.
- Programs list (the bottom panel of a prefix). Programs installed in the prefix appear in it automatically:
  Cask reads the Start Menu shortcuts (`.lnk`) of the prefix and lists the `.exe` files they point to
  (Wine's own programs, the Windows folder and uninstallers are left out). Add your own with "Add program...".
  Click a name to rename it (the default is the file name), drag rows to reorder them, hover a row to see the
  path. Buttons: Run (turns into Stop while the program is running), Run in terminal (shows the Wine output),
  Programs found automatically disappear from the list when their file is deleted (the program was
  uninstalled), and so does a program you removed from the list by hand (it returns if it is installed again);
  programs you added yourself stay. Show in folder, Remove from the list (a removed program is not added again automatically; the list and the
  hidden entries are kept in `cask.json`).
- Wine's `winemenubuilder` is disabled (`WINEDLLOVERRIDES=winemenubuilder.exe=d`): installers do not add
  shortcuts or file associations to your system menu (`~/.local/share/applications/wine`). The installed
  programs are listed in Cask instead. Entries that Wine created earlier are not removed.
- The prefix list can be reordered by drag and drop. The order is remembered in
  `prefixes/.cask-order.json` and moves together with the prefixes.
- The Wine profile folders (Desktop, Documents, Downloads, Music, Pictures, Videos...) do not point to your
  real home directory: after a prefix is created and after its Wine is changed, such links are replaced
  with empty regular folders. Your files are not touched. The `z:` drive (system root) is left as is.
- "Stop all processes": runs `wineserver -k` for the prefix, then sends SIGKILL to the Wine processes
  (executable name starting with `wine`) that have this prefix in their `WINEPREFIX`, including ones started
  from a terminal. A shell that merely has `WINEPREFIX` exported is not touched. Deleting a prefix and
  changing its Wine use the same mechanism.
- Winetricks (Tools → Winetricks…): searchable list of verbs with categories, installed verbs are marked,
  selected verbs are installed with live output and a cancel button (cancelling also stops the Wine
  processes). The `dxvk*`, `vkd3d*` and `d9vk` verbs are hidden because Components manages them.
  Cask does not download the script: put it into `data/winetricks/winetricks` or install the `winetricks`
  package. Files downloaded by winetricks are cached in `data/winetricks/cache` (`W_CACHE`). As with any
  winetricks, system tools such as `cabextract`, `unzip` and `wget` or `curl` are needed.
- Opening files from a file manager: `cask <file.exe|.msi|.bat|.lnk>` shows a prefix picker (prefixes are
  created in the main window), launches the file with that prefix's settings and exits. The last chosen
  prefix is offered first.
- Desktop integration (Settings → "Integrate with the desktop"): creates `cask.desktop` (category
  Utility) and adds Cask to the `.exe`, `.msi`, `.bat` and `.lnk` entries of
  `~/.local/share/applications/mimeinfo.cache`, so a double click in the file manager opens Cask. Cask
  checks the path in the `.desktop` file on every start, which keeps it valid after the folder is moved.
  When the option is off, the files are removed and only Cask's own lines are taken out of
  `mimeinfo.cache` (the file itself and other entries stay).

### Launch settings (per prefix, stored in `cask.json`)

| Setting | What it does |
|---|---|
| Wine language | Empty = system language, otherwise the value for `LANG` and `LC_ALL` (for example `ru_RU.UTF-8`; you can type your own). |
| GameMode | Programs started with "Run program…" are launched through `gamemoderun`. |
| MangoHud | The same through `mangohud` (system package or portable, see below). `gamemoderun mangohud wine ...` when combined with GameMode. |
| Environment variables | Any custom variables (for example `DXVK_HUD=fps`). **They do not replace variables managed by Cask**: `WINEPREFIX`, `WINEARCH`, `PATH`, `LANG`, `LC_ALL`, `DXVK_STATE_CACHE_PATH`, `DXVK_SHADER_CACHE_PATH`, `MESA_SHADER_CACHE_DIR`, `VKD3D_SHADER_CACHE_PATH`. The editor rejects these names. Everything else, including `WINEDEBUG`, is applied. |

Cask does not configure synchronization (ntsync etc.): Wine 11 uses ntsync by itself when it is available.
If a special build needs another mode, add the variable yourself (for example `WINEFSYNC=1`).

Custom variables, GameMode and MangoHud apply only to programs started by the user. Cask's internal tasks
(`wineboot`, `regedit`) run without them (the language is still applied).

### MangoHud

Without any setup Cask uses the system `mangohud` (the `mangohud` package of your distribution).

**Portable MangoHud (64-bit only).** From a release archive on GitHub (`MangoHud-<version>.tar.gz`), copy
the folder `MangoHud/pkg/usr/lib/mangohud/lib64/` to `Cask/data/mangohud/lib64/` (it must contain
`libMangoHud.so`, `libMangoHud_shim.so` and `libMangoHud_opengl.so`). Cask then sets `MANGOHUD=1`,
`LD_PRELOAD` (OpenGL) and the Vulkan layer itself (a manifest pointing to your copy is generated in
`data/mangohud/layer/` on launch), so the `mangohud` wrapper script is not needed. `libxkbcommon` and
`libwayland-client` must be present on the system. 32-bit games are not supported.

**Configuration.** Put `Cask/data/mangohud/MangoHud.conf` there (an example is in the archive:
`usr/share/doc/mangohud/MangoHud.conf.example`). Cask points `MANGOHUD_CONFIGFILE` at it, for the system
`mangohud` as well. If `MANGOHUD_CONFIGFILE` is set in the prefix's custom variables, that value wins.
`MANGOHUD_CONFIG` still adds parameters on top of the file.

## Interface language

English and Russian. The system language is used by default and can be changed in Settings without a
restart. Strings in the code are English; translations live in `resources/lang/<code>.json` (embedded
into the binary).

To add a language, copy `resources/lang/en.json` to `resources/lang/<code>.json`, set `_name` to the
language's own name, translate the values and list the file in `resources/cask.qrc`. Untranslated strings stay
in English. `python3 scripts/check-translations.py` checks the files; `--sync-en` regenerates `en.json` after
strings in the code change.

## Building

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/Cask/cask
```

Requires Qt 6 (or Qt 5) Widgets and CMake ≥ 3.16. The portable folder is produced in `build/Cask/`.
`scripts/bundle-linux.sh build/Cask` additionally copies the Qt libraries and plugins into `lib/` and `lib/plugins/` (needs
`patchelf`).

Translation check (also run by GitHub Actions):

```
python3 scripts/check-translations.py        # all strings translated, placeholders match
```

## License

Cask is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 3 of the License, or (at your option)
any later version. See [LICENSE](LICENSE). Release archives also contain the Qt libraries, which are
licensed under the LGPL-3.0 (https://www.qt.io/licensing/).
