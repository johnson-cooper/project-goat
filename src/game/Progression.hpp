#pragma once

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace goat::game {

struct Reward { int credits{}; std::string pack_id; };
// `tier` gates when an NPC becomes selectable in normal play: tier 1 is
// always open, tier N+1 unlocks once every tier-N NPC has 10 recorded wins.
// `agent` names which goat::ai::DuelAgent implementation should play this
// NPC's seat ("random" or "goat" — see src/ai/); npcs.json has carried this
// per-NPC since before the AI module existed, but nothing consumed it until
// the CPU-agent architecture landed. Both fields are appended after the
// existing ones (not inserted) so existing positional aggregate-initializers
// (e.g. in tests) keep compiling unchanged.
struct Npc { std::string id; std::string name; int difficulty{}; std::string deck_path; Reward reward; int tier{1}; std::string agent{"random"}; };
// `required_tier` gates purchasing a pack in the shop the same way (reward
// packs from NPC victories bypass this — you can earn a pack before you can
// buy more of it).
struct Pack { std::string id; std::string name; int price{}; int cards_per_pack{}; std::string art; std::vector<uint32_t> pool; int required_tier{1}; };

struct Profile {
    std::string player_name = "Challenger";
    int credits = 300;
    std::string selected_deck = "decks/starter/water-fusion.ydk";
    std::map<uint32_t, int> collection;
    std::map<std::string, int> sealed_packs;
    std::map<std::string, int> npc_wins;
};

class Progression {
public:
    explicit Progression(Profile profile = {});
    const Profile& profile() const noexcept;
    void select_starter_deck(const std::string& path);
    void grant_starter_deck(const std::string& path);
    void award_npc_victory(const Npc& npc);
    bool buy_pack(const Pack& pack);
    std::vector<uint32_t> open_pack(const Pack& pack, std::mt19937& random);
    bool owns(uint32_t card_code, int copies = 1) const;
private:
    Profile profile_;
};

class ProfileStore {
public:
    static void save(const Profile& profile, const std::string& filename);
    static Profile load(const std::string& filename);
};

} // namespace goat::game
