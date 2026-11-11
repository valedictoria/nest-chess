#!/usr/bin/env bash
# Builds Nest and installs lichess-bot next to it, ready to run.
# Run from anywhere:  ./lichess/setup.sh
# Safe to run again: it rebuilds the engine and updates lichess-bot.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
cd "$root"

say() { printf '\n== %s\n' "$1"; }

# --- 1. tools -------------------------------------------------------------
for tool in make git curl; do
  command -v "$tool" >/dev/null || { echo "Missing '$tool'. On macOS run: xcode-select --install"; exit 1; }
done

python=""
for candidate in python3.13 python3.12 python3.11 python3; do
  if command -v "$candidate" >/dev/null \
     && "$candidate" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)'; then
    python="$candidate"; break
  fi
done
if [ -z "$python" ]; then
  echo "lichess-bot needs Python 3.11 or newer, and none was found."
  echo "On macOS: brew install python   (the system python3 is too old)"
  exit 1
fi

# --- 2. engine ------------------------------------------------------------
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) arch="apple-silicon" ;;
  *)            arch="native" ;;
esac

say "Downloading and checking the networks"
make net

say "Building Nest (ARCH=$arch)"
make -j build ARCH="$arch"

say "Engine self-check"
signature="$(./nest bench | tail -1)"
echo "bench: $signature"
case "$signature" in
  *" nodes "*) ;;
  *) echo "The engine did not finish its bench run; not continuing."; exit 1 ;;
esac

# --- 3. lichess-bot -------------------------------------------------------
say "Installing lichess-bot"
if [ -d "$here/lichess-bot/.git" ]; then
  git -C "$here/lichess-bot" pull --ff-only
else
  git clone --depth 1 https://github.com/lichess-bot-devs/lichess-bot "$here/lichess-bot"
fi
"$python" -m venv "$here/venv"
"$here/venv/bin/pip" install --quiet --upgrade pip
"$here/venv/bin/pip" install --quiet -r "$here/lichess-bot/requirements.txt"

# --- 4. configuration -----------------------------------------------------
# Threads: the performance cores on Apple Silicon, physical cores elsewhere.
if [ "$(uname -s)" = "Darwin" ]; then
  threads="$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || sysctl -n hw.physicalcpu)"
  mem_mb=$(( $(sysctl -n hw.memsize) / 1048576 ))
else
  threads="$(lscpu -p=CORE 2>/dev/null | grep -v '^#' | sort -u | wc -l | tr -d ' ')"
  [ "$threads" -ge 1 ] 2>/dev/null || threads="$(nproc)"
  mem_mb="$(awk '/MemTotal/ {print int($2 / 1024)}' /proc/meminfo)"
fi
# Hash: about an eighth of RAM, between 64 MB and 2 GB.
hash=$(( mem_mb / 8 ))
[ "$hash" -lt 64 ] && hash=64
[ "$hash" -gt 2048 ] && hash=2048

if [ -f "$here/config.yml" ]; then
  echo "Keeping your existing lichess/config.yml"
else
  sed -e "s|@ENGINE_DIR@|$root|g" -e "s|@THREADS@|$threads|g" -e "s|@HASH@|$hash|g" \
      "$here/config.template.yml" > "$here/config.yml"
  echo "Wrote lichess/config.yml (Threads $threads, Hash $hash MB)"
fi

cat <<TEXT

Done. What is left is on the Lichess side; see lichess/README.md:
  1. create a bot token and put it in LICHESS_BOT_TOKEN
  2. upgrade the account to a bot account, once:   ./lichess/run.sh -u
  3. start playing:                                ./lichess/run.sh
TEXT
