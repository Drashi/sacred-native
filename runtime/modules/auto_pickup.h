#pragma once
// Opt-in automatic pick-up (Settings.cfg AUTO_PICKUP : 1, or SACRED_AUTO_PICKUP=1, which wins; =trace logs why): the
// hero picks up loot lying next to it while standing or walking, without a click. What is picked up is decided by the
// game's own pick-up filter, the in-game option PICKUPAUTO (cCreature_collectFilter 0x4ec330), the one the game applies
// when it gathers the items around a pile the player clicked.
//
// The work runs at a present of the in-game main loop (cEngine_renderThread_run 0x60e350 calling dxDriver7_flip
// 0x645e40), after that frame's game logic, at most ten times a second (the rate of the game's own object logic).
// It walks the 3 x 3 map cells around the hero exactly as the game's area pick-up does (0x4ecec4) and calls the
// game's cCreature::pickupItem (0x558080) for every item the filter accepts, so gold, inventory space, sounds and
// messages behave as with a click. Single player only: a network game keeps the original behaviour.
//
// Items the player drops (an item on the cursor that leaves it while it lies next to the hero) are never picked up
// again on their own; an item the game refuses (no room in the inventory) is tried again only after the hero has
// left it behind.
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace sacred {
class Control;

class AutoPickup {
public:
    // `trace`: one log line whenever the reason for doing nothing or the cells' contents change
    // (SACRED_AUTO_PICKUP=trace).
    AutoPickup(std::function<void(const std::string&)> log, bool trace) : log_(std::move(log)), trace_(trace) {}
    // At every present of the primary surface, on the guest thread that presents.
    void present(Control& control);

private:
    void tick(Control& control);
    void note(const std::string& line);
    std::function<void(const std::string&)> log_;
    bool trace_ = false;
    std::string last_note_;
    bool broken_ = false;
    bool held_ = false;                           // an item was on the cursor at a present since the last tick
    uint64_t last_ns_ = 0;                        // host time of the last tick
    uint32_t scratch_ = 0;                        // two 16-byte map coordinates in guest memory
    std::unordered_set<uint32_t> near_;           // item ids next to the hero at the last tick
    std::unordered_map<uint32_t, uint32_t> kept_; // id -> type of items left alone for good (dropped by the player)
    std::unordered_set<uint32_t> refused_;        // ids the game refused to pick up while they stayed near
    uint64_t picked_ = 0;
};
}
