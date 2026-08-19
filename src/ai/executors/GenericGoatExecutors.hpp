#pragma once

#include "../Executor.hpp"

// The default ordered rule table registered into every GoatAgent: a small,
// deliberately narrow set of high-frequency GOAT staples (confirmed present,
// legal, and scripted in this project's pinned card pool/banlist — see
// tests/ai_staple_test.cpp) layered over generic fallback rules for summon,
// reposition, and setting decisions. This is meant to prove the executor
// architecture cleanly, not to cover the full GOAT card pool — see the
// project's AI report for recommended next cards/decks.

namespace goat::ai {

// Passcodes verified against external/BabelCDB/cards.cdb,
// external/LFLists/GOAT.lflist.conf, and external/CardScripts/official/ —
// see the verification step in the AI implementation plan. Exposed so tests
// can reference the exact same ids without re-deriving them.
namespace goat_card {
constexpr uint32_t PotOfGreed = 55144522;
constexpr uint32_t GracefulCharity = 79571449;
constexpr uint32_t HeavyStorm = 19613556;
constexpr uint32_t MysticalSpaceTyphoon = 5318639;
constexpr uint32_t MirrorForce = 44095762;
constexpr uint32_t TorrentialTribute = 53582587;
constexpr uint32_t SakuretsuArmor = 56120475;
constexpr uint32_t CallOfTheHaunted = 97077563;
constexpr uint32_t BookOfMoon = 14087893;

// Archetype-specific staples (executors/ChaosControlExecutor.cpp,
// GearfriedExecutor.cpp, BurnExecutor.cpp) — same verification standard as
// the generic staples above.
constexpr uint32_t BlackLusterSoldierEnvoy = 72989439;
constexpr uint32_t ChaosSorcerer = 9596126;
constexpr uint32_t CardDestruction = 72892473;
constexpr uint32_t ReturnFromTheDifferentDimension = 27174286;
constexpr uint32_t GearfriedTheIronKnight = 423705;
constexpr uint32_t ReinforcementOfTheArmy = 32807846;
constexpr uint32_t ExiledForce = 74131780;
constexpr uint32_t JustDesserts = 24068492;
constexpr uint32_t SecretBarrel = 27053506;
constexpr uint32_t Ceasefire = 36468556;
constexpr uint32_t MagicCylinder = 62279055;
constexpr uint32_t NightmareWheel = 54704216;
constexpr uint32_t OjamaTrio = 29843091;
constexpr uint32_t SkillDrain = 82732705;
constexpr uint32_t SwordsOfRevealingLight = 72302403;
constexpr uint32_t WallOfRevealingLight = 17078030;
} // namespace goat_card

// Appends the generic fallback rules (staple advantage/removal cards plus
// generic summon/reposition/set behavior) to an existing list — used both
// standalone (build_generic_goat_executors) and as the tail of every
// deck-specific executor table, so a deck-specific list only has to declare
// what makes it different, never re-declare the shared baseline.
void add_generic_goat_rules(ExecutorList& list);

ExecutorList build_generic_goat_executors();

} // namespace goat::ai
