# Third-party notices

This project depends on EDOPro `ygopro-core` and Project Ignis CardScripts. Both pinned upstream projects are licensed under AGPL-3.0-or-later. Their source, copyright notices, and Lua-script attribution remain in `external/ygopro-core` and `external/CardScripts`; nothing in this project removes or relicenses those notices.

`external/card_images` and `external/packart` currently contain Yu-Gi-Oh! card and pack artwork that is not original to this project and is owned by Konami. It is included for local development/testing convenience and is intended to be replaced with original artwork.

## WindBot (design inspiration only, no code vendored)

The CPU-agent architecture in `src/ai/` (the `DuelAgent`/executor pattern, the generic reactive-trap and board-power heuristics, the battle-target selection algorithm) was designed after studying `external/windbot`, a reference copy of WindBot kept locally for that study and not built or linked into this project. WindBot's own lineage: originally IceYGO's WindBot (MIT License, Copyright (c) 2015-2017 IceYGO), later extended as "WindBot Ignite" for Project Ignis: EDOPro (GNU Affero General Public License v3.0-or-later, Copyright (c) 2019-2020 Edoardo Lolletti) — see `external/windbot/LICENSE` for the full text and required notices.

No WindBot source, comments, or data tables (e.g. its `_CardId`/`_Setcode` constant blocks) were copied into `src/ai/`; every file there is an original C++ implementation of the underlying *concepts* (rule-table-with-priority-order, board-power comparison heuristics, greedy attack-target selection, an activation-count loop breaker), rewritten for `ygopro-core` and the classic/GOAT ruleset from scratch. This project does not embed, statically or dynamically link against, or run any WindBot binary or source file.
