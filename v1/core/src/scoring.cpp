#include "aib/scoring.h"

namespace aib {

float Compensate(float score, int n) noexcept {
    if (n <= 1) return score;
    const float mod_factor = 1.0f - (1.0f / static_cast<float>(n));
    const float make_up = (1.0f - score) * mod_factor;
    return score + (make_up * score);
}

ScoreTrace ScoreActionTraced(const CompiledAction& action,
                             const InputCache& inputs,
                             float best_so_far) noexcept {
    ScoreTrace trace;
    float score = action.weight;
    const int n = static_cast<int>(action.considerations.size());

    for (int i = 0; i < n; ++i) {
        // The product only decreases and compensation only raises, so the
        // compensated running score is an upper bound on where this action can
        // still finish. If that bound is already below the incumbent, no
        // amount of further evaluation can change the outcome.
        //
        // This is the property that makes early-out an optimization rather
        // than an approximation, and early_out_never_changes_the_winner
        // asserts it across random inputs -- because an "optimization" that
        // occasionally picks a different action is a bug that only shows up
        // under load.
        if (Compensate(score, n) < best_so_far) {
            trace.score = 0.0f;
            trace.outcome = ScoreOutcome::EarlyOut;
            trace.stopped_at = i;
            trace.considerations_evaluated = i;
            return trace;
        }

        const CompiledConsideration& c = action.considerations[i];
        const float raw = inputs.Get(c.input_index);
        const float norm = Normalize(raw, c.input_min, c.input_max);
        score *= EvaluateCurve(c.curve, norm);
        trace.considerations_evaluated = i + 1;

        // Zero is absorbing, so this is a hard veto and there is nothing left
        // to learn. Worth having separately from the bound check: a veto is a
        // different fact about the action than "it could not win", and the
        // debugger shows them differently.
        if (score <= 0.0f) {
            trace.score = 0.0f;
            trace.outcome = ScoreOutcome::Vetoed;
            trace.stopped_at = i;
            return trace;
        }
    }

    trace.score = Compensate(score, n);
    trace.outcome = ScoreOutcome::Complete;
    return trace;
}

float ScoreAction(const CompiledAction& action, const InputCache& inputs,
                  float best_so_far) noexcept {
    // Deliberately not `return ScoreActionTraced(...).score`: the traced
    // version allocates nothing but does write fields the hot path does not
    // need, and this loop runs 30,000 times a frame at the design document's
    // 200-agent target. Keeping them separate costs a little duplication and
    // keeps the shipping path tight.
    float score = action.weight;
    const int n = static_cast<int>(action.considerations.size());

    for (int i = 0; i < n; ++i) {
        if (Compensate(score, n) < best_so_far) return 0.0f;

        const CompiledConsideration& c = action.considerations[i];
        const float raw = inputs.Get(c.input_index);
        const float norm = Normalize(raw, c.input_min, c.input_max);
        score *= EvaluateCurve(c.curve, norm);

        if (score <= 0.0f) return 0.0f;
    }
    return Compensate(score, n);
}

ActionBreakdown Explain(const CompiledBehavior& behavior,
                        std::uint16_t action_index, const InputCache& inputs) {
    ActionBreakdown out;
    out.action_index = action_index;
    if (action_index >= behavior.actions.size()) return out;

    const CompiledAction& action = behavior.actions[action_index];
    out.weight = action.weight;

    float score = action.weight;
    const int n = static_cast<int>(action.considerations.size());
    out.considerations.reserve(action.considerations.size());

    for (int i = 0; i < n; ++i) {
        const CompiledConsideration& c = action.considerations[i];
        ConsiderationBreakdown cb;
        cb.input_index = c.input_index;
        cb.raw = inputs.Get(c.input_index);
        cb.normalized = Normalize(cb.raw, c.input_min, c.input_max);
        cb.curve_output = EvaluateCurve(c.curve, cb.normalized);
        score *= cb.curve_output;
        cb.running_product = score;
        out.considerations.push_back(cb);

        // Report the veto but keep evaluating. A breakdown that stops at the
        // first zero hides the reason the designer is looking for: they need
        // to see that AllyCount was 0.10 *and* that Health was fine, not just
        // that something failed.
        if (cb.curve_output <= 0.0f && out.stopped_at < 0) {
            out.outcome = ScoreOutcome::Vetoed;
            out.stopped_at = i;
        }
    }

    out.raw_product = score;
    out.final_score = Compensate(score, n);
    out.compensation = (score > 0.0f) ? (out.final_score / score) : 1.0f;
    return out;
}

}  // namespace aib
