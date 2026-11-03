#!/bin/sh
# Builds the application inside the container of docker-compose.yml. The arguments are the ones of scripts/build.mjs (--debug, --web-only,
# --legacy-physics).
#
#   /src         the sources of the project, mounted read-only: nothing is written there
#   /work        a copy of what the build needs of them (a few MB: src/, scripts/, the configuration files)
#   /work/node_modules   the Node packages: a named volume (installed again when package-lock.json changes)
#   /opt/rme-tools       a named volume: Emscripten (emsdk), Box3D (lib/box3d) and the compiled objects of both, which the build
#                        reuses: it compiles again only what changed
#   /dist        the output of the build, mounted read-write
set -eu

TOOLS="${RME_TOOLS_DIR:-/opt/rme-tools}"
SRC="${RME_SRC_DIR:-/src}"
WORK="${RME_WORK_DIR:-/work}"
EMSDK_VERSION="${EMSDK_VERSION:-6.0.13}"   # the Emscripten the project was made with
export RME_TOOLS_DIR="$TOOLS" RME_DIST_DIR="${RME_DIST_DIR:-/dist}"

mkdir -p "$TOOLS" "$WORK" "$RME_DIST_DIR"

# --- Emscripten, once (the version is remembered in the volume, a different one is installed over it)
if [ ! -x "$TOOLS/emsdk/upstream/emscripten/em++" ] || [ "$(cat "$TOOLS/emsdk/.rme-version" 2>/dev/null || true)" != "$EMSDK_VERSION" ]; then
  echo "== installing Emscripten $EMSDK_VERSION into $TOOLS/emsdk"
  [ -d "$TOOLS/emsdk/.git" ] || git clone --depth 1 https://github.com/emscripten-core/emsdk "$TOOLS/emsdk"
  (cd "$TOOLS/emsdk" && ./emsdk install "$EMSDK_VERSION" && ./emsdk activate "$EMSDK_VERSION")
  echo "$EMSDK_VERSION" > "$TOOLS/emsdk/.rme-version"
fi

# --- the sources: copied, so that the build can write next to them (the sources stay as they are). What the copy leaves out is what
# the host has of its own: its Node packages and tools (for the host's system), its output, its game files.
rsync -a --delete \
  --exclude node_modules --exclude .tools --exclude dist --exclude .git --exclude .idea \
  --exclude cmake-build-debug --exclude assets --exclude .unpacked --exclude __pycache__ \
  "$SRC"/ "$WORK"/

# --- the Node packages, installed again only when the lock file is not the one they were installed from
cd "$WORK"
LOCK_HASH="$(sha256sum package-lock.json | cut -d' ' -f1)"
if [ ! -f node_modules/.rme-lock-hash ] || [ "$(cat node_modules/.rme-lock-hash)" != "$LOCK_HASH" ]; then
  echo "== installing the Node packages"
  npm ci --no-audit --no-fund
  echo "$LOCK_HASH" > node_modules/.rme-lock-hash
fi

# --- the build (it fetches Box3D into $TOOLS/lib/box3d at the commit it names, when that is not there yet)
status=0
node scripts/build.mjs "$@" || status=$?

# files made by the container are the host user's, when compose says who that is
if [ -n "${HOST_UID:-}" ] && [ -n "${HOST_GID:-}" ]; then
  chown -R "$HOST_UID:$HOST_GID" "$RME_DIST_DIR" || true
fi
exit "$status"
