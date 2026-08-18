# Project GOAT

A cross-platform Yu-Gi-Oh! desktop simulator for the 2005.4 **GOAT Format**, built on Project Ignis's `ygopro-core` duel engine. The engine — not the client — determines legal actions; the client only renders what the engine advertises and sends back the player's choice.

**`goat-client-rl`** (built on [raylib](https://www.raylib.com/)) is the primary client and builds on Windows, Linux, and macOS. The headless duel engine, **`goat-sim`**, has no platform-specific dependencies and runs alongside it as a subprocess — it's what actually plays out a duel; the client only renders what it publishes.

An earlier Windows-only client (`goat-client.exe`, native Win32 GDI/WIC) still lives in `src/client/` and still builds, but it's a legacy build now — see [Cross-platform client](#cross-platform-client) below for why and how the port happened.

## Download

Tagged releases publish self-contained Windows/Linux/macOS builds — binaries plus every data/asset file needed to play, already laid out correctly — via GitHub Actions (`.github/workflows/release.yml`). Grab the latest one from this repo's Releases page, unzip, and run `goat-client-rl` (`.exe` on Windows) from inside the extracted folder.

## Build from source

Fetch the pinned external dependencies first (`external/ygopro-core`, the duel engine source compiled directly into both binaries, and `external/raylib`, the graphics library `goat-client-rl` builds on — neither is tracked in git):

```sh
pwsh ./scripts/update-dependencies.ps1
```

Then build with CMake:

```sh
cmake -S . -B build -G Ninja
cmake --build build --target goat-client-rl goat-sim
./build/goat-sim duel decks/vanilla-a.ydk decks/vanilla-b.ydk --seed 12345
./build/goat-client-rl   # run from the repo root — see "Running" below
```

`scripts/update-dependencies.ps1` also pins `external/CardScripts`, `external/BabelCDB`, `external/LFLists`, and `external/windbot` to specific upstream commits (it only follows upstream `master` when passed `-AllowUnpinned`). Of those, only `ygopro-core` and `raylib` need fetching for a normal build — `BabelCDB` and `LFLists` are committed in full, and `CardScripts` is committed as the subset actually resolvable through this project's card database and GOAT banlist (~1,300 of the upstream project's ~22,700 files). `external/windbot` is unused reference code and never needs fetching for a normal build. Re-run the script (optionally after editing the pinned commits) any time you need a dependency refreshed.

**Windows toolchain note:** building `goat-client-rl` needs a genuine MinGW-w64 GCC (`g++ -dumpmachine` reporting `x86_64-w64-mingw32`) or MSVC — a Cygwin-hosted GCC (`x86_64-pc-cygwin`, only defines `__CYGWIN__` not `_WIN32`) compiles raylib's GLFW backend as empty stubs and fails to link with undefined `_glfwPlatform*` references. If CMake itself isn't working in your local environment, `./scripts/build-client-rl.sh` builds the same target via a direct compiler invocation instead (Windows/MinGW only) and warns if it detects the wrong toolchain.

## Running

Run `build/goat-client-rl` (or `build/goat-client-rl.exe` on Windows) with the repo root (or an extracted release folder) as its working directory — decks, card data, art, and the duel engine binary are all loaded through paths relative to it. `Start-GOAT-Simulator.cmd` does this for you on Windows (double-click it, or run it from anywhere).

## What's in the client

- **Hub** — campaign navigation: Auto Duel (CPU vs. CPU), Play CPU, Shop, Collection, Edit Deck, Test Duel.
- **Play CPU** — a roster of 12 NPCs across 3 difficulty tiers (paged), each unlocking once every NPC in the tier below has 10 recorded wins. 11 of them run real GOAT-format tournament decklists (`decks/starter/flc*-*.ydk`); the 12th (Tide Master) runs a water/fusion deck.
- **Shop** — a pack grid (aspect-correct art) with a Buy Pack flow that opens into an animated card-by-card reveal screen.
- **Collection** — a paginated, searchable, type-filterable grid of every card you own, with a click-to-inspect detail panel.
- **Deck Editor / Deck List** — build decks from your owned collection (search + Monster/Spell/Trap filters; left-click a pool card to inspect it, right-click to add a copy), with a dedicated full-screen Main/Extra decklist view for easy add/remove. Legality is re-checked live against the GOAT banlist. Player-made decks save under `decks/player/`; shipped decks are never overwritten in place.
- **Duel board** — live LP, hand, and field state streamed from the engine via a file-based IPC bridge, including paged legal-action lists, per-hand-card action popups, zone-highlighted legal targets, and a card inspector.

Campaign progression (credits, active deck, collection, sealed packs, NPC wins) is native C++ state persisted to `saves/default.sav` — see `src/game/Progression.*`. It isn't tracked in git; a fresh checkout (or a fresh unzip of a release) starts with a fresh profile.

## Cross-platform client

`goat-client-rl` (`src/client_rl/`) is a from-scratch port of the original Win32 client onto [raylib](https://www.raylib.com/) instead of GDI/WIC/Win32 `EDIT` controls, so it builds on macOS and Linux too, not just Windows. It reached full feature parity with the Win32 original (every screen, including the duel board and engine IPC) and was promoted to the primary client once it did. **See `docs/CROSS_PLATFORM_CLIENT.md` for the full build-out history, architecture decisions, and known verification gaps** — that document is the canonical record of this effort.

`external/raylib` is a pinned dependency like the others (fetched via `scripts/update-dependencies.ps1`, gitignored, not committed).

## Testing

`ctest --test-dir build` runs the full suite: engine smoke duel, the player-decision IPC bridge (`scripts/test-player-ipc.ps1`, Windows only), card database resolution, banlist parsing, deck read/write round-tripping, and catalog/roster validation (every NPC deck is checked for size, banlist legality, and local art coverage against whatever's currently in `data/npcs.json`).

## Documentation

- `docs/ENGINE_ARCHITECTURE.md` / `docs/GAME_ARCHITECTURE.md` — engine and client architecture.
- `docs/GOAT_RULESET.md` — the 2005.4 GOAT ruleset this project targets.
- `docs/PROTOCOL.md` — the engine↔client decision/IPC protocol.
- `docs/CROSS_PLATFORM_CLIENT.md` — the raylib-based client: status, architecture decisions, and remaining-work roadmap.
- `THIRD_PARTY_NOTICES.md` — third-party licensing and asset attribution.
