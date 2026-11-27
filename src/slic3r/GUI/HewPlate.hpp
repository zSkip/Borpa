#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {
class Model;
}

namespace Slic3r { namespace GUI { namespace HewPlate {

// The material of each build item of a 3MF written by Hew, in build order: the name of the
// core-spec <base> its object's pindex points at. Hew names a <base> after its material, and
// a Borpa build material is named after its filament preset. An item whose object or base
// cannot be found reads as an empty name; a file that cannot be read gives an empty list.
std::vector<std::string> item_material_names(const std::string& path_3mf);

// Objects of `model` that touch, as groups of object indices in ascending order: two objects
// touch when a vertex of one lies within `tolerance` mm of the other's surface (a part drawn
// on another's face, an inlay). Every object is in exactly one group.
std::vector<std::vector<size_t>> touching_groups(const Model& model, double tolerance);

// Which project filament slot (1-based) each filament preset prints in.
struct SlotPlan
{
    std::map<std::string, int> slot_of;  // preset name -> slot
    std::vector<std::string>   added;    // presets that need a new slot, in slot order
    std::vector<std::string>   unknown;  // presets the bundle does not have; they print in slot 1
    std::vector<std::string>   overflow; // presets whose slot lies beyond what the printer holds
};

// Plans slots for `needed` (presets in first-use order): a preset already in a slot of
// `current` keeps that slot, a known preset gets a new slot after the `physical` slots, an
// unknown one (`known` false) goes to slot 1. New slots stop at `max_slots`; presets beyond
// that print in slot 1. `printer_holds` is how many filaments the printer can print at once.
SlotPlan plan_slots(const std::vector<std::string>& needed,
                    const std::vector<std::string>& current,
                    size_t                          physical,
                    size_t                          max_slots,
                    size_t                          printer_holds,
                    const std::vector<bool>&        known);

}}} // namespace Slic3r::GUI::HewPlate
