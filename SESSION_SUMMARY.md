# GOAT Duel Simulator — Session Summary

## Goal

Build a native C++ Yu-Gi-Oh! GOAT Format game using Project Ignis: automatic duels and player-versus-CPU duels, local card graphics, NPC progression, starter decks, collection, packs, shop, saves, and a desktop UI.

## Implemented

- Project Ignis / `ygopro-core` duel executable: `build/goat-sim.exe`.
- GOAT card metadata, compatibility entries, scripts, and 2005.4 GOAT banlist support.
- Automatic legal normal-monster duel policy for CPU-versus-CPU matches.
- Player-versus-CPU decision bridge:
  - legal Main and Battle Phase actions;
  - monster-zone and position choices;
  - card target and tribute selections, including bounded multi-card combinations;
  - yes/no confirmations and effect-option choices;
  - player-safe board snapshots containing LP, hand counts, and monster zones.
- Native Win32 C++ / GDI desktop client. No Node or web runtime is used.
- Campaign profile persistence in `saves/default.sav`:
  - credits;
  - selected starter deck;
  - collection ownership;
  - sealed packs;
  - NPC victories.
- Two starter decks, two NPCs, pack purchase/opening, card collection pages, deck ownership validation, and local card artwork.
- Separate in-client screens for title, campaign hub, shop, collection, and duel play.

## Artwork

User-provided artwork lives in `external/packart` and card art lives in `external/card_images`.

Four supplied files were renamed and wired into the catalog/UI:

- `goat-starter.jpg`
- `goat-classics.jpg`
- `warrior-training.jpg`
- `goat-control-training.jpg`

## Launching

Run the client from the project root:

```powershell
cd E:\ygosimulator9000
.\build\goat-client.exe
```

For normal double-click use, launch:

`Start-GOAT-Simulator.cmd`

It explicitly sets `E:\ygosimulator9000` as the working directory before opening the game. This matters because the game loads decks, CDB data, scripts, art, and the duel executable through project-relative paths.

Required MSYS runtime DLLs are copied beside the client in `build`.

## Verification Completed

- `src/main.cpp` compiles cleanly.
- Native client compiles to `build/goat-client.exe`.
- `goat-sim.exe duel decks/vanilla-a.ydk decks/vanilla-b.ydk --seed 12345 --max-turns 100 --quiet` completes with an engine-authored winner.
- `scripts/test-player-ipc.ps1` completes and produces:
  - a valid `winner=<0|1>` result;
  - `build/ipc-test/state.txt` with LP, hand counts, and visible monster zones.

## Important Files

- `src/main.cpp` — Project Ignis integration, duel loop, CPU policy, player IPC, state snapshots.
- `src/client/main.cpp` — native desktop UI and game flow.
- `src/game/Progression.*` — profile, rewards, packs, save format.
- `src/game/Catalog.*` — NPC and pack catalog loader.
- `src/game/DeckBuilder.*` — YDK import/export and collection/GOAT validation.
- `data/npcs.json` and `data/packs.json` — campaign content.
- `scripts/test-player-ipc.ps1` — player decision bridge regression test.

## Remaining Work

- Broaden support for uncommon Project Ignis prompts (chains, counters, sums, sorting, announcements, and all complex card effects).
- Add richer GOAT deck archetypes and CPU strategies.
- Expand campaign content, NPC progression, balancing, packs, and deck-building UI.
- Improve visual polish: animations, sound, accessibility, card inspection, and packaging/distribution without MSYS runtime dependencies.
