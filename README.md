# Project GOAT

A native Win32 C++ Yu-Gi-Oh! desktop simulator for the 2005.4 **GOAT Format**, built on Project Ignis's `ygopro-core` duel engine. The engine — not this application — determines legal actions; the client only renders what the engine advertises and sends back the player's choice.

**Platform note:** the graphical client (`goat-client.exe`) is native Win32 (GDI rendering, Windows Imaging Component, Win32 `EDIT` controls) and only builds on Windows. The headless duel engine (`goat-sim.exe`) has no Windows-specific dependencies and builds on Linux/macOS as well, but without a GUI it's a CLI-only duel simulator — there's currently no way to actually play the game outside Windows.

## Build (MSYS2 / Windows)

`external/ygopro-core` (the duel engine source, compiled directly into both binaries) isn't tracked in git — fetch it first:

```sh
pwsh ./scripts/update-dependencies.ps1
```

Then build:

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/goat-sim.exe duel decks/vanilla-a.ydk decks/vanilla-b.ydk --seed 12345
# Or, when CMake/Ninja aren't set up in the local MSYS environment:
./scripts/build-smoke.sh
```

`scripts/update-dependencies.ps1` pins `external/ygopro-core`, `external/CardScripts`, `external/BabelCDB`, `external/LFLists`, and `external/windbot` to specific upstream commits (it only follows upstream `master` when passed `-AllowUnpinned`). Of those, only `ygopro-core` needs fetching for a normal build — `BabelCDB` and `LFLists` are committed in full, and `CardScripts` is committed as the subset actually resolvable through this project's card database and GOAT banlist (~1,300 of the upstream project's ~22,700 files; every non-normal-monster card the GOAT banlist permits, skipping printings this project's `external/BabelCDB` doesn't carry). `external/windbot` is unused reference code and never needs fetching for a normal build. Re-run the script (optionally after editing the pinned commits) any time you need a dependency refreshed, or if a future change references a `CardScripts` file outside the committed subset.

## Running

Double-click `Start-GOAT-Simulator.cmd`, or run `build/goat-client.exe` directly from the project root — it needs to run with this directory as its working directory, since decks, card data, scripts, art, and the duel engine are all loaded through project-relative paths.

## What's in the client

- **Hub** — campaign navigation: Auto Duel (CPU vs. CPU), Play CPU, Shop, Collection, Edit Deck, Test Duel.
- **Play CPU** — a roster of 12 NPCs across 3 difficulty tiers (paged), each unlocking once every NPC in the tier below has 10 recorded wins. 11 of them run real GOAT-format tournament decklists (`decks/starter/flc*-*.ydk`); the 12th (Tide Master) runs a water/fusion deck.
- **Shop** — a pack grid (aspect-correct art) with a Buy Pack flow that opens into an animated card-by-card reveal screen.
- **Collection** — a paginated, searchable, type-filterable grid of every card you own, with a click-to-inspect detail panel.
- **Deck Editor / Deck List** — build decks from your owned collection (search + Monster/Spell/Trap filters), with a dedicated full-screen Main/Extra decklist view for easy add/remove. Legality is re-checked live against the GOAT banlist. Player-made decks save under `decks/player/`; shipped decks are never overwritten in place.
- **Duel board** — live LP, hand, and field state streamed from the engine via a file-based IPC bridge (`src/main.cpp` ↔ `src/client/main.cpp`), including paged legal-action lists, per-hand-card action popups, and a card inspector.

Campaign progression (credits, active deck, collection, sealed packs, NPC wins) is native C++ state persisted to `saves/default.sav` — see `src/game/Progression.*`. It isn't tracked in git; a fresh checkout starts with a fresh profile.

## Testing

`ctest --test-dir build` runs the full suite: engine smoke duel, the player-decision IPC bridge (`scripts/test-player-ipc.ps1`), card database resolution, banlist parsing, deck read/write round-tripping, and catalog/roster validation (every NPC deck is checked for size, banlist legality, and local art coverage against whatever's currently in `data/npcs.json`).

## Documentation

- `docs/ENGINE_ARCHITECTURE.md` / `docs/GAME_ARCHITECTURE.md` — engine and client architecture.
- `docs/GOAT_RULESET.md` — the 2005.4 GOAT ruleset this project targets.
- `docs/PROTOCOL.md` — the engine↔client decision/IPC protocol.
- `THIRD_PARTY_NOTICES.md` — third-party licensing and asset attribution.
