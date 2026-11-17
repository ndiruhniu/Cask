#!/usr/bin/env bash
# Turns build/Cask into a portable folder: puts Qt and its plugins into lib/ and lib/plugins/,
# so Cask runs without Qt installed.
# Usage: scripts/bundle-linux.sh build/Cask
set -euo pipefail

DIR="$(cd "$1" && pwd)"
BIN="$DIR/cask"
LIB="$DIR/lib"
PLUG="$LIB/plugins"
mkdir -p "$LIB" "$PLUG/platforms" "$PLUG/xcbglintegrations"

QT_PLUGINS="$(find /usr/lib -type d -path '*/qt6/plugins' | head -n1)"
[ -n "$QT_PLUGINS" ] || { echo "Qt plugins dir not found" >&2; exit 1; }

# Platform plugins
for p in libqxcb.so libqoffscreen.so libqwayland-generic.so; do
  [ -f "$QT_PLUGINS/platforms/$p" ] && cp -L "$QT_PLUGINS/platforms/$p" "$PLUG/platforms/"
done
cp -L "$QT_PLUGINS"/xcbglintegrations/*.so "$PLUG/xcbglintegrations/" 2>/dev/null || true

# Which libraries to bundle (the rest, e.g. glibc/libGL, comes from the system).
# libxkbcommon and libxkbcommon-x11 must come from the same build: the x11 part uses the internals of the
# main library, so a bundled -x11 next to a different system libxkbcommon crashes while Qt reads the keyboard.
want() {
  case "$(basename "$1")" in
    libQt6*|libicu*|libdouble-conversion*|libpcre2-16*|libmd4c*|libb2*) return 0 ;;
    libxcb-cursor*|libxcb-icccm*|libxcb-image*|libxcb-keysyms*|libxcb-render-util*|libxcb-shape*|libxcb-xkb*|libxkbcommon*) return 0 ;;
    *) return 1 ;;
  esac
}

collect() {
  ldd "$1" 2>/dev/null | awk '/=> \//{print $3}' | while read -r so; do
    if want "$so" && [ ! -f "$LIB/$(basename "$so")" ]; then
      cp -L "$so" "$LIB/"
    fi
  done
}

# Several passes to pull in dependencies of dependencies
for _ in 1 2 3; do
  collect "$BIN"
  for f in "$PLUG"/platforms/*.so "$PLUG"/xcbglintegrations/*.so "$LIB"/*.so*; do
    [ -f "$f" ] && collect "$f"
  done
done

# RPATH: the binary looks in ./lib, libraries next to each other, plugins (lib/plugins/<kind>/) in ../..
patchelf --set-rpath '$ORIGIN/lib' "$BIN"
for f in "$LIB"/*.so*; do patchelf --set-rpath '$ORIGIN' "$f"; done
for f in "$PLUG"/platforms/*.so "$PLUG"/xcbglintegrations/*.so; do
  [ -f "$f" ] && patchelf --set-rpath '$ORIGIN/../..' "$f"
done

# No qt.conf: the binary adds lib/plugins to the Qt library path itself (see src/main.cpp).

# Every binary we ship must resolve all its libraries, including the Qt plugins
# (an unresolved plugin dependency only shows up at run time as "could not load the platform plugin").
for f in "$BIN" "$PLUG"/platforms/*.so "$PLUG"/xcbglintegrations/*.so; do
  [ -f "$f" ] || continue
  if ldd "$f" | grep -q 'not found'; then
    echo "Unresolved libraries in $f:" >&2
    ldd "$f" | grep 'not found' >&2
    exit 1
  fi
done
echo "Bundled $(ls "$LIB" | wc -l) libraries into $LIB"
