# Game architecture

The finished product is a C++ desktop game, not a separate web client. `ygopro-core` remains the sole authority for duel legality, state transitions, card effects, and outcomes.

## Layers

| Layer | Responsibility |
| --- | --- |
| `engine/` | Owns a duel, translates core messages, submits only encoded responses, and exposes snapshots/events. |
| `cards/` | Reads BabelCDB (`cards.cdb` and `goat-entries.cdb`), resolves GOAT compatibility IDs/scripts, banlists, names, and image paths. |
| `game/` | Player profile, collection, deck inventory, NPC progression, rewards, packs, shop prices, and save data. No duel rules live here. |
| `ai/` | CPU difficulty profiles and GOAT strategies. Every action must originate from an engine-provided legal-action request. |
| `ui/` | Native C++ desktop screens: title, campaign map, deck builder, shop, collection, duel board, and results. It renders `external/card_images/<passcode>.jpg`; it never calculates rules. |

## Player journey

1. Choose a starter deck.
2. Fight named GOAT NPCs with distinct decks and AI profiles.
3. Earn currency/cards from duels.
4. Buy packs in the shop and add cards to the collection.
5. Build legal GOAT decks and challenge stronger NPCs.

## Delivery order

1. Stabilize complete headless GOAT duels with BabelCDB, LFLists, CardScripts, and full response decoding.
2. Extract the C++ engine/game interfaces from the current smoke harness.
3. Add native C++ board rendering, card-image cache, player input, and CPU actions.
4. Add profiles, starter decks, NPCs, save data, rewards, packs, shop, and deck builder.
5. Add campaign balancing, animations, audio, accessibility, and packaging.

The UI will present only player-relative information. Facedown cards and hidden hands stay hidden even though the engine internally knows them.
