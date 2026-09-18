// Action scoring: compensation and early-out (design.md sections 3.3, 3.4).
//
// THE MULTIPLICATION PROBLEM. An action's score is the product of its
// considerations, and every consideration is <= 1. So more considerations mean
// a lower score, mechanically, regardless of how good the action is:
//
//     2 considerations at 0.9 -> 0.810
//     4 at 0.9                -> 0.656
//     8 at 0.9                -> 0.430
//
// A carefully-authored action with eight considerations loses to a crude one
// with two, every time. Designers respond by deleting considerations, which is
// exactly backwards. The design document calls this "the single most commonly
// missed piece of a utility implementation", and its absence is invisible
// until somebody complains that the detailed behaviors never fire.
//
// Compensate() is the standard fix. It does not eliminate the bias and is not
// meant to: an action with more requirements should be slightly harder to
// satisfy, it just should not be impossible.

#ifndef AIB_SCORING_H
#define AIB_SCORING_H

#include <cstdint>
#include <vector>

#include "aib/compiled.h"

namespace aib {

// Values indexed by the compiler's input table. No names in the hot loop.
struct InputCache {
    std::vector<float> values;

    float Get(std::uint16_t index) const {
        return index < values.size() ? values[index] : 0.0f;
    }
};

// Scale a product back up in proportion to how many terms went into it.
//
//     mod    = 1 - 1/n
//     makeup = (1 - score) * mod
//     final  = score + makeup * score
//
// Four at 0.9: 0.656 -> 0.825. Two at 0.9: 0.810 -> 0.887. The gap narrows
// from 0.154 to 0.062.
float Compensate(float score, int n) noexcept;

// Why an action stopped being evaluated. Recorded so the debugger can show
// which consideration caused each early-out and how often (section 3.4) --
// that is how a designer learns where an expensive check is being wasted.
enum class ScoreOutcome : std::uint8_t {
    Complete = 0,   // every consideration evaluated
    EarlyOut = 1,   // cannot beat best_so_far; abandoned
    Vetoed = 2,     // a consideration returned exactly zero
};

struct ScoreTrace {
    float score = 0.0f;
    ScoreOutcome outcome = ScoreOutcome::Complete;
    // Index of the consideration that ended evaluation, or -1 if it ran to
    // completion.
    int stopped_at = -1;
    int considerations_evaluated = 0;
};

// Score one action.
//
// `best_so_far` enables early-out. Pass 0 to force a full evaluation, which is
// what the debugger's FullBreakdown detail level does -- a breakdown that
// stops halfway is not a breakdown.
float ScoreAction(const CompiledAction& action, const InputCache& inputs,
                  float best_so_far) noexcept;

// Same, but reports why it stopped.
ScoreTrace ScoreActionTraced(const CompiledAction& action,
                             const InputCache& inputs,
                             float best_so_far) noexcept;

// --------------------------------------------------------------- breakdown

// One consideration, at every level: raw input, normalized value, curve
// output. Section 8.2 -- utility AI has no structure to point at, so without
// this "why did it pick that?" is unanswerable.
struct ConsiderationBreakdown {
    std::uint16_t input_index = 0;
    float raw = 0.0f;
    float normalized = 0.0f;
    float curve_output = 0.0f;
    float running_product = 0.0f;
};

struct ActionBreakdown {
    std::uint16_t action_index = 0;
    float weight = 1.0f;
    float raw_product = 0.0f;    // before compensation
    float compensation = 1.0f;   // the multiplier it worked out to
    float final_score = 0.0f;
    ScoreOutcome outcome = ScoreOutcome::Complete;
    int stopped_at = -1;
    std::vector<ConsiderationBreakdown> considerations;
};

// Full evaluation with no early-out, for the debugger.
ActionBreakdown Explain(const CompiledBehavior& behavior,
                        std::uint16_t action_index, const InputCache& inputs);

}  // namespace aib

#endif  // AIB_SCORING_H
