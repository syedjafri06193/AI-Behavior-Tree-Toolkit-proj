// Action selection: buckets, hysteresis, commitment (design.md sections 3.5,
// 3.6).
//
// Two actions scoring 0.71 and 0.70 will swap every frame as inputs jitter,
// and the agent visibly twitches. The design document calls this "the most
// common complaint about utility AI in practice" and prescribes three
// mitigations, noting that you generally want all three:
//
//   - an incumbent bonus, which makes the current action harder to beat;
//   - a switch threshold, so a challenger must clear it by a margin rather
//     than merely exceed it;
//   - a commitment time, below which no switch happens at all.
//
// Any one alone leaves visible flicker in some configurations, which is why
// all three are here and why near_tied_actions_do_not_thrash exercises them
// together against jittered inputs.

#ifndef AIB_SELECTION_H
#define AIB_SELECTION_H

#include <cstdint>
#include <vector>

#include "aib/scoring.h"

namespace aib {

struct SelectionState {
    int current_action = -1;
    float current_score = 0.0f;
    float time_in_current_action = 0.0f;
    // Set by the game to break commitment early -- "combat begins" should
    // interrupt a committed idle immediately (section 3.5). Cleared by Select.
    bool force_reevaluate = false;
};

struct SelectionResult {
    int action = -1;
    float score = 0.0f;
    bool switched = false;
    // True when commitment blocked the evaluation entirely: no scoring ran,
    // which is also the cheapest possible outcome and worth knowing about when
    // reading a profile.
    bool committed = false;
    std::uint8_t buckets_evaluated = 0;
    int actions_scored = 0;
    int early_outs = 0;
    // The unit that actually costs something. `actions_scored` counts actions
    // visited, which is the same with and without early-out and therefore
    // measures nothing -- pruning shows up as considerations not evaluated.
    int considerations_evaluated = 0;
};

struct SelectionOptions {
    // Turning early-out off must not change the winner. It exists so the test
    // suite can prove that, and so the debugger can produce a complete
    // breakdown of every action rather than a partial one.
    bool early_out = true;
};

SelectionResult Select(const CompiledBehavior& behavior,
                       const InputCache& inputs, SelectionState& state,
                       float dt, SelectionOptions options = {});

// Score every action with no early-out and no hysteresis. For the debugger's
// Scores detail level and for the score-history graph (section 8.2), both of
// which want the honest number for every action rather than the pruned one.
std::vector<float> ScoreAll(const CompiledBehavior& behavior,
                            const InputCache& inputs);

}  // namespace aib

#endif  // AIB_SELECTION_H
