# Cask package (derivation).
#
# Cask is a portable application: it keeps its settings, Wine builds and prefixes in a "root" folder.
# The Nix store is read-only, so the installed `cask` command is a small launcher that points the
# root (CASK_ROOT) at a writable directory in the user's home (see `dataDir`) and then starts
# the Qt-wrapped binary. The C++ sources are not touched.
#
# Standalone use (without the flake): pkgs.callPackage ./package.nix { };
# The sources are not part of this branch; they come from the `main` branch.
{
  lib,
  stdenv,
  cmake,
  ninja,
  qt6,
  writeText,
  runtimeShell,
  makeDesktopItem,
  copyDesktopItems,
  buildFHSEnv,
  # Tools Cask runs: winetricks and what it needs to unpack/download, GameMode and MangoHud.
  winetricks,
  cabextract,
  unzip,
  p7zip,
  curl,
  gamemode,
  mangohud,

  # Sources of Cask (the `main` branch). The flake passes its pinned `cask-src` input here; without
  # the flake the latest `main` is fetched. Pass your own path or fetchFromGitHub to override.
  # (Not called `src`: nixpkgs has a top-level `src` alias that callPackage would pick up.)
  caskSrc ? builtins.fetchGit {
    url = "https://github.com/ndiruhniu/cask";
    ref = "main";
  },

  # Version shown in "About" (and used as the package version): a release number or the short commit hash.
  version ? caskSrc.shortRev or "dev",

  # Where Cask keeps everything (CASK_ROOT: cask.ini, data/, prefixes/). A shell expression:
  # it may use $HOME or $XDG_DATA_HOME. An existing CASK_ROOT in the environment always wins.
  dataDir ? "\${XDG_DATA_HOME:-$HOME/.local/share}/cask",

  # A Wine package that is put first on Cask's PATH (appears as "System Wine"), e.g. pkgs.wineWowPackages.staging.
  wine ? null,

  # Tools on Cask's PATH, and any extra ones.
  runtimePackages ? [
    winetricks
    cabextract
    unzip
    p7zip
    curl
    gamemode
    mangohud
  ],
  extraPackages ? [ ],
}:

let
  mimeTypes = [
    "application/x-ms-dos-executable"
    "application/vnd.microsoft.portable-executable"
    "application/x-msdownload"
    "application/x-msi"
    "application/x-ms-shortcut"
    "application/x-bat"
  ];

  # Libraries of the FHS environment (see passthru.fhs). Also used by the declarative setup service of
  # the NixOS module (nix/declarative.nix), which has to run downloaded Wine builds in the same environment.
  fhsMultiPkgs =
    pkgs:
    let
      # nixpkgs is moving the X11 libraries out of `xorg`; accept both spellings.
      x = new: old: pkgs.${new} or pkgs.xorg.${old};
    in
    with pkgs;
    [
      alsa-lib
      fontconfig
      freetype
      gnutls
      libGL
      libpulseaudio
      libusb1
      libunwind # ntdll.so of the GE/Lutris builds
      libxkbcommon
      openal
      sdl3
      vulkan-loader
      (x "libx11" "libX11")
      (x "libxcomposite" "libXcomposite")
      (x "libxcursor" "libXcursor")
      (x "libxext" "libXext")
      (x "libxi" "libXi")
      (x "libxinerama" "libXinerama")
      (x "libxrandr" "libXrandr")
      (x "libxrender" "libXrender")
      (x "libxxf86vm" "libXxf86vm")
    ];

  fhsProfile = ''
    export LD_LIBRARY_PATH=/run/opengl-driver/lib:/run/opengl-driver-32/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
  '';

  # `bin/cask`: sets the root and starts the wrapped binary. The Nix parts are expanded when the
  # package is built, the shell parts (written with ''${ ) when the user runs it.
  launcher = writeText "cask-launcher" ''
    #!@shell@
    export CASK_ROOT="''${CASK_ROOT:-@root@}"
    exec @out@/libexec/cask/cask "$@"
  '';
in
stdenv.mkDerivation (finalAttrs: {
  pname = "cask";
  inherit version;

  src = caskSrc;

  cmakeFlags = [ (lib.cmakeFeature "CASK_VERSION" version) ];

  nativeBuildInputs = [
    cmake
    ninja
    qt6.wrapQtAppsHook
    copyDesktopItems
  ];

  buildInputs = [ qt6.qtbase ] ++ lib.optional stdenv.hostPlatform.isLinux qt6.qtwayland;

  # We install by hand: the project's own install rules are meant for the portable folder layout.
  dontWrapQtApps = true;

  installPhase = ''
    runHook preInstall

    install -Dm755 Cask/cask $out/libexec/cask/cask

    mkdir -p $out/bin
    substitute ${launcher} $out/bin/cask \
      --subst-var-by shell ${runtimeShell} \
      --subst-var-by root '${dataDir}' \
      --subst-var out
    chmod +x $out/bin/cask

    runHook postInstall
  '';

  postFixup = ''
    wrapQtApp $out/libexec/cask/cask \
      --prefix PATH : ${lib.makeBinPath (lib.optional (wine != null) wine ++ runtimePackages ++ extraPackages)}
  '';

  desktopItems = [
    (makeDesktopItem {
      name = "cask";
      desktopName = "Cask";
      genericName = "Wine prefix manager";
      comment = "Minimalist Wine prefix manager";
      exec = "cask %f";
      icon = "wine";
      categories = [ "Utility" ];
      mimeTypes = mimeTypes;
      startupNotify = true;
    })
  ];

  passthru = {
    inherit mimeTypes fhsMultiPkgs fhsProfile;

    # Cask inside an FHS environment (like steam-run). Needed for Wine/Proton builds that you
    # download and put into Cask's data/runners: such generic Linux binaries do not run on NixOS
    # otherwise. Not needed when you use a Wine from nixpkgs (the `wine` argument).
    fhs = buildFHSEnv {
      name = "cask";
      targetPkgs = _: [ finalAttrs.finalPackage ];
      # Without multiArch the 32-bit libraries (multiPkgs) and the 32-bit loader /lib/ld-linux.so.2 are left out,
      # and the 32-bit part of Wine builds (bin/wine of most of them) cannot start.
      multiArch = true;
      multiPkgs = fhsMultiPkgs;
      runScript = "cask";
      profile = fhsProfile;
      extraInstallCommands = ''
        mkdir -p $out/share
        ln -s ${finalAttrs.finalPackage}/share/applications $out/share/applications
      '';
    };
  };

  meta = {
    description = "Wine prefix manager written in C++/Qt";
    license = lib.licenses.gpl3Plus;
    mainProgram = "cask";
    platforms = lib.platforms.linux;
  };
})
