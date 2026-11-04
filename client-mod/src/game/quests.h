#pragma once
// Finished quests, reported to the server so it can pay the reward into the ledger.
//
// Quests run on each player's own Fable. When Fable marks a quest completed
// (CQuestManager 0x004B1D30, tools/re-notes/14-quest-rewards.md), the mod reads the quest's
// card: its def name and the gold and renown on it at that moment (boasts included). The
// server pays at most what that card and its boasts can pay, once per hero.
#include <cstdint>
#include <string>
#include <vector>

namespace fo::game::quests {

struct Completed {
    std::string script;  // the quest script's name, e.g. "Q_BanditCamp"
    std::string card;    // the card's def name, "" when the quest has no card
    int32_t gold = 0;
    int32_t renown = 0;
};

/** Patches the two calls to Fable's quest-completed function. False if the bytes differ. */
bool install();
/** Quests completed since the last call, oldest first. Any thread. */
std::vector<Completed> take();

}  // namespace fo::game::quests
