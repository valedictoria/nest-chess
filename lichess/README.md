# Running Nest as a Lichess bot

Nest talks to Lichess through [lichess-bot](https://github.com/lichess-bot-devs/lichess-bot),
the bridge maintained by the Lichess bot developers. The scripts here build
the engine, install lichess-bot beside it and write a configuration for it.

## Before you start

- **A separate Lichess account.** Only an account that has **never played a
  game** can become a bot, and the change **cannot be undone**. Do not use
  your personal account: register a new one for the bot.
- **Python 3.11 or newer.** The Python that ships with macOS is too old;
  `brew install python` provides a suitable one.
- Xcode Command Line Tools on macOS (`xcode-select --install`).

## Setup

```sh
./lichess/setup.sh
```

This downloads the networks, builds `nest` for this machine, runs its bench
as a self-check, installs lichess-bot into `lichess/lichess-bot` with its own
Python environment, and writes `lichess/config.yml` with a thread count and
hash size chosen for the machine.

## Lichess side

1. Log in to the bot's account and create a token with only the
   **Play games with the bot API** (`bot:play`) permission:
   <https://lichess.org/account/oauth/token/create?scopes[]=bot:play&description=Nest>
2. Make the token available to the scripts. It is read from the environment
   and never written to a file in this repository:

   ```sh
   export LICHESS_BOT_TOKEN=lip_xxxxxxxxxxxxxxxx
   ```

3. Upgrade the account, once (this is the irreversible step):

   ```sh
   ./lichess/run.sh -u
   ```

4. Start the bot:

   ```sh
   ./lichess/run.sh
   ```

It now accepts challenges. Stop it with Ctrl-C.

## Settings worth knowing

All in `lichess/config.yml`:

| Setting | Default here | Notes |
|---|---|---|
| `challenge.modes` | `casual` only | Add `rated` when you are satisfied with how it plays |
| `engine.ponder` | `false` | `true` thinks on the opponent's time and keeps the CPU busy all game |
| `engine.uci_options.Threads` | performance / physical cores | Leave a core free if you use the machine meanwhile |
| `engine.uci_options.Hash` | about 1/8 of RAM | In MB |
| `move_overhead` | 1500 ms | Raise it if the bot loses on time over a slow connection |
| `challenge.variants` | `standard` | Nest implements Chess960, but only its move generation has been tested so far |
| `matchmaking.allow_matchmaking` | `false` | `true` makes the bot look for other bots to play |

## What to expect

- Lichess allows engines that use another engine's network, so the Stockfish
  networks are fine here. Say in the bot's profile that it runs Nest with
  Stockfish 17.1 networks (GPL-3.0): it is both courteous and a licence
  matter.
- A Lichess rating is not a strength measurement. It depends on who the bot
  happens to play and at which time control; use SPRT matches for that.
- `tools/selfplay_smoke.py ./nest` plays timed games of the engine against
  itself and fails on a crash, a rejected move or a lost clock. Run it after
  changing the engine and before putting the bot back online.
