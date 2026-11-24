#include "auto_pickup.h"
#include "game_frame.h"
#include "../process/control.h"
#include <chrono>
#include <exception>
#include <string>
#include <vector>

namespace sacred {
namespace {
// Sacred.exe 2.28 (the executable the generated code is pinned to).
constexpr uint32_t object_manager_cell = 0x00ad5c40; // cObjectManager*; its first word is the map
constexpr uint32_t item_types_cell = 0x00aab5e4;     // the item type table
constexpr uint32_t mouse_cell = 0x00cddb5c;          // cMouse*; +0x68 the item on the cursor
constexpr uint32_t network_cell = 0x0182ebec;        // low byte set in a network game
constexpr uint32_t render_thread_flags = 0x00ad6ef4; // bit 0 once the in-game loop runs

constexpr uint32_t local_hero = 0x00603e30;      // thiscall (object manager) -> cCreature*
constexpr uint32_t object_data = 0x005fe000;     // thiscall (object manager, id) -> cObject*
constexpr uint32_t is_pickable = 0x00427920;     // thiscall (type table, type id) -> bool
constexpr uint32_t collect_filter = 0x004ec330;  // thiscall (creature, cObject*) -> bool, the PICKUPAUTO filter
constexpr uint32_t pickup_item = 0x00558080;     // thiscall (creature, id)
constexpr uint32_t map_cell_of = 0x00634c50;     // thiscall (map, coordinate*, creature +0x2c) -> cell*
constexpr uint32_t cell_coordinate = 0x006224e0; // thiscall (coordinate*, position*): world position -> cell

// cObject / cCreature fields.
constexpr uint32_t object_id = 0x0c, object_type = 0x10, object_flags = 0x14, object_position = 0x18,
                   object_next = 0x30;
constexpr uint32_t creature_map_key = 0x2c, creature_action = 0xfc;
constexpr uint32_t object_taken = 0x2;   // object_flags: picked up (owned by an inventory)
constexpr uint32_t creature_rides = 0x2; // object_flags of a creature
constexpr uint32_t cell_first_object = 0x08;

constexpr uint64_t tick_ns = 100'000'000;
constexpr int max_objects_per_cell = 64;

const Signature thiscall1{Convention::Thiscall, ReturnChannel::Eax, {{4}}, ""};
const Signature thiscall2{Convention::Thiscall, ReturnChannel::Eax, {{4}, {4}}, ""};
const Signature thiscall3{Convention::Thiscall, ReturnChannel::Eax, {{4}, {4}, {4}}, ""};

uint64_t now_ns() {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint32_t call(Control& control, uint32_t target, const Signature& signature, const std::vector<uint64_t>& args) {
    return uint32_t(control.invoke(target, signature, args).integer);
}
}

void AutoPickup::note(const std::string& line) {
    if (trace_ && line != last_note_) {
        last_note_ = line;
        log_("AUTO_PICKUP_TRACE " + line);
    }
}

void AutoPickup::present(Control& control) {
    if (broken_) {
        return;
    }

    try {
        Memory& memory = control.memory;
        if (!(memory.load32(render_thread_flags) & 1)) {
            return;
        }

        if (const uint32_t mouse = memory.load32(mouse_cell); mouse && memory.load32(mouse + 0x68)) {
            held_ = true;
        }

        const uint64_t now = now_ns();
        if (now - last_ns_ < tick_ns) {
            return;
        }

        if (!main_loop_present(control)) {
            note("not the main loop's present");
            return;
        }

        last_ns_ = now;
        tick(control);
    } catch (const std::exception& error) {
        broken_ = true;
        log_(std::string("AUTO_PICKUP_DISABLED ") + error.what());
    }
}

void AutoPickup::tick(Control& control) {
    Memory& memory = control.memory;
    const bool dropped = held_;
    held_ = false;
    if (memory.load32(network_cell) & 0xff) {
        note("network game");
        return;
    }

    if (const uint32_t mouse = memory.load32(mouse_cell); mouse && memory.load32(mouse + 0x68)) {
        held_ = true; // still dragging: nothing is picked up under the cursor's item
        note("item on the cursor");
        return;
    }

    const uint32_t manager = memory.load32(object_manager_cell);
    if (!manager) {
        return;
    }

    const uint32_t map = memory.load32(manager);
    const uint32_t hero = call(control, local_hero, thiscall1, {manager});
    if (!map || !hero) {
        note("no hero");
        return;
    }

    // Standing (0) or walking and running (1, 2) on foot; fights, arts, dialogues and the game's own pick-up keep
    // their course.
    const uint32_t action = memory.load32(hero + creature_action) & 0xffff;
    if (action > 2 || (memory.load32(hero + object_flags) & creature_rides)) {
        note("hero busy: action " + std::to_string(action));
        return;
    }

    if (!scratch_) {
        scratch_ = control.heap.allocate(32);
    }

    const uint32_t centre = scratch_, probe = scratch_ + 16;
    call(control, cell_coordinate, thiscall2, {centre, hero + object_position});
    const int32_t cx = int32_t(memory.load32(centre + 4)), cy = int32_t(memory.load32(centre + 8));
    const uint32_t map_key = memory.load32(hero + creature_map_key);
    const uint32_t types = memory.load32(item_types_cell);
    int cells = 0, objects = 0, items = 0;

    std::unordered_set<uint32_t> near;
    std::vector<uint32_t> wanted;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            // A coordinate with sector 0 is a world cell, as the game's area pick-up builds it (0x6224b0).
            memory.store32(probe, 0);
            memory.store32(probe + 4, uint32_t(cx + dx));
            memory.store32(probe + 8, uint32_t(cy + dy));
            memory.store32(probe + 12, 0);
            const uint32_t cell = call(control, map_cell_of, thiscall3, {map, probe, map_key});
            if (!cell) {
                continue;
            }

            ++cells;

            uint32_t id = memory.load32(cell + cell_first_object);
            for (int n = 0; id && n < max_objects_per_cell; ++n) {
                const uint32_t object = call(control, object_data, thiscall2, {manager, id});
                if (!object) {
                    break;
                }

                ++objects;
                const uint32_t type = memory.load32(object + object_type);
                if (object != hero && !(memory.load32(object + object_flags) & object_taken) &&
                    (call(control, is_pickable, thiscall2, {types, type}) & 0xff)) {
                    near.insert(id);
                    ++items;
                    const bool fresh = !near_.count(id);
                    auto kept = kept_.find(id);
                    if (kept != kept_.end() && kept->second != type) {
                        kept_.erase(kept); // the id now names another object
                        kept = kept_.end();
                    }

                    if (dropped && fresh) {
                        kept_[id] = type; // appeared at the hero's feet as an item left the cursor
                    } else if (kept == kept_.end() && !refused_.count(id) &&
                               (call(control, collect_filter, thiscall2, {hero, object}) & 0xff)) {
                        wanted.push_back(id);
                    }
                }

                id = memory.load32(object + object_next);
            }
        }
    }

    for (auto it = refused_.begin(); it != refused_.end();) {
        it = near.count(*it) ? std::next(it) : refused_.erase(it);
    }

    near_ = std::move(near);
    if (trace_) {
        note("cell " + std::to_string(cx) + "," + std::to_string(cy) + ": " + std::to_string(cells) + " cells, " +
             std::to_string(objects) + " objects, " + std::to_string(items) + " items, " +
             std::to_string(wanted.size()) + " accepted" + (dropped ? ", after a drop" : ""));
    }

    for (const uint32_t id : wanted) {
        const uint32_t object = call(control, object_data, thiscall2, {manager, id});
        if (!object || (memory.load32(object + object_flags) & object_taken)) {
            continue;
        }

        call(control, pickup_item, thiscall2, {hero, id});
        const uint32_t after = call(control, object_data, thiscall2, {manager, id});
        if (after && !(memory.load32(after + object_flags) & object_taken) && memory.load32(after + object_id) == id) {
            refused_.insert(id);
        } else if (++picked_ == 1) {
            log_("AUTO_PICKUP first item");
        }
    }
}
}
