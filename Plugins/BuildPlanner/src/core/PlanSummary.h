#pragma once
// BuildPlanner - the plan's objective rows
// PlanSummary.h - the objective rows of a plan and the "next objective" that the overlay's summary
// box and the Passives tab name. Pure and header-only (no SDK, no ImGui). A port of POE1
// PassiveTreePlanner's PlanSummary.h.
//
// Threads: value types and a pure function; safe anywhere.
#include <cstdint>
#include <vector>

#include "Types.h"

namespace bp {

// One row of the plan's objective list: the consecutive steps that walk to one objective.
struct ObjectiveRow {
    uint32_t node = kNoIndex;   // the objective's tree node index (kNoIndex: the remaining points that lead to none)
    int cost = 0;               // POINTS the row costs: its steps that cost a point (Node::CostsPoint),
                                // the objective included
    int freeSteps = 0;          // its steps that cost no point (free nodes, multiple-choice options)
    int tier = 0;               // PlanStep::tier: 1-based index of the earlier PoB tree that had it, 0 none
    int firstNumber = 0;        // step number of the row's first point (0 = unnumbered)
    bool reachable = true;
};

enum class NextKind : uint8_t {
    None,         // points are missing, but none can be reached yet (the Passives tab says why)
    Main,         // the main tree's first reachable objective
    Ascendancy,   // the main tree has nothing reachable left: the ascendancy's first reachable objective
    Done          // nothing left to take: neither on the main tree nor in the ascendancy
};

struct NextObjective {
    NextKind kind = NextKind::None;
    const ObjectiveRow* row = nullptr;   // Main / Ascendancy: a row of the list passed in
};

// `missing` / `ascMissing` are Plan::missing / Plan::ascMissing. Done needs BOTH to be zero: a
// finished main tree with ascendancy points still to take is not "nothing left to take".
inline NextObjective PickNext(const std::vector<ObjectiveRow>& main, const std::vector<ObjectiveRow>& asc, int missing,
                              int ascMissing) noexcept {
    NextObjective r;
    for (const ObjectiveRow& o : main)
        if (o.reachable) {
            r.kind = NextKind::Main;
            r.row = &o;
            return r;
        }
    if (missing <= 0 && ascMissing <= 0) {
        r.kind = NextKind::Done;
        return r;
    }
    for (const ObjectiveRow& o : asc)
        if (o.reachable) {
            r.kind = NextKind::Ascendancy;
            r.row = &o;
            return r;
        }
    return r;
}

} // namespace bp
