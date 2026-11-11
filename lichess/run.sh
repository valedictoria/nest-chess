#!/usr/bin/env bash
# Starts lichess-bot with Nest. Extra arguments go to lichess-bot,
# e.g.  ./lichess/run.sh -u   (one-time upgrade to a bot account)
#       ./lichess/run.sh -v   (verbose log)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"

if [ ! -x "$here/venv/bin/python" ] || [ ! -f "$here/config.yml" ]; then
  echo "Not set up yet. Run ./lichess/setup.sh first."
  exit 1
fi
if [ -z "${LICHESS_BOT_TOKEN:-}" ]; then
  echo "LICHESS_BOT_TOKEN is not set. Create a token (see lichess/README.md), then:"
  echo "  export LICHESS_BOT_TOKEN=..."
  exit 1
fi

cd "$here/lichess-bot"
exec "$here/venv/bin/python" lichess-bot.py --config "$here/config.yml" "$@"
