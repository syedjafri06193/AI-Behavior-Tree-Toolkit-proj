#include "aib/selection.h"

namespace aib {

SelectionResult Select(const CompiledBehavior& behavior,
                       const InputCache& inputs, SelectionState& state,
                       float dt, SelectionOptions options) {
    SelectionResult result;
    state.time_in_current_action += dt;

    // Commitment gate. Note this returns before any scoring at all: the
    // cheapest evaluation is the one that does not happen, and at 5 Hz with a
    // 0.5 s commitment that is most of them.
    const bool can_switch =
        state.time_in_current_action >= behavior.policy.min_commitment_seconds ||
        state.force_reevaluate || state.current_action < 0;

    if (!can_switch) {
        // An uninterruptible action holds even against a force-reevaluate.
        // That distinction is the difference between "combat began, drop what
        // you are doing" and "you are mid-reload, finish it".
        result.action = state.current_action;
        result.score = state.current_score;
        result.committed = true;
        return result;
    }

    if (state.force_reevaluate && state.current_action >= 0 &&
        state.current_action < static_cast<int>(behavior.actions.size()) &&
        !behavior.actions[state.current_action].interruptible &&
        state.time_in_current_action < behavior.policy.min_commitment_seconds) {
        result.action = state.current_action;
        result.score = state.current_score;
        result.committed = true;
        return result;
    }

    const float bonus = behavior.policy.current_action_bonus;

    float best = 0.0f;
    int best_index = -1;
    // The incumbent's score measured in THIS evaluation, bonus included.
    // Measuring it now rather than carrying it forward is the whole point --
    // see the switch decision below.
    float incumbent_score = 0.0f;
    bool incumbent_scored = false;

    for (std::size_t bucket = 0; bucket < behavior.actions_in_bucket.size();
         ++bucket) {
        result.buckets_evaluated = static_cast<std::uint8_t>(bucket + 1);

        for (std::uint16_t i : behavior.actions_in_bucket[bucket]) {
            const CompiledAction& action = behavior.actions[i];
            const bool is_incumbent = static_cast<int>(i) == state.current_action;
            const float multiplier = is_incumbent ? bonus : 1.0f;

            float raw;
            if (options.early_out && !is_incumbent) {
                raw = 0.0f;
                const ScoreTrace trace = ScoreActionTraced(action, inputs, best);
                raw = trace.score;
                result.considerations_evaluated += trace.considerations_evaluated;
                if (trace.outcome == ScoreOutcome::EarlyOut) ++result.early_outs;
            } else {
                // THE INCUMBENT IS NEVER EARLY-OUT.
                //
                // Early-out is exact for finding the maximum -- an action it
                // abandons provably could not have won. But selection needs
                // more than the maximum: it needs the incumbent's actual score,
                // because that is the bar the challenger has to clear by
                // switch_threshold. A pruned incumbent reports zero, the bar
                // collapses to nothing, and the agent switches on a margin it
                // never actually had.
                //
                // That is a nasty one, because it only fires when the incumbent
                // is losing by less than the bonus covers -- exactly the
                // near-tie case the hysteresis exists to handle. It presents as
                // an agent that will not stick to anything, intermittently.
                //
                // The cost of exempting it is one action out of thirty, and it
                // is the action most likely to win anyway.
                raw = ScoreAction(action, inputs, 0.0f);
                result.considerations_evaluated +=
                    static_cast<int>(action.considerations.size());
            }
            ++result.actions_scored;

            // Incumbent bonus: the main defence against flicker.
            const float score = raw * multiplier;
            if (is_incumbent) {
                incumbent_score = score;
                incumbent_scored = true;
            }

            if (score > best) {
                best = score;
                best_index = static_cast<int>(i);
            }
        }

        // A strong winner here means lower buckets are never scored at all.
        // This is section 3.6 compounding with early-out: an emergency that
        // fires makes the entire ambient tier free.
        //
        // Note this can break before the incumbent has been scored, when the
        // incumbent lives in a lower bucket. That is correct: bucket priority
        // outranks hysteresis, and an emergency should preempt an ambient
        // action without having to clear its bonus first.
        if (best > behavior.policy.bucket_satisfied_threshold) break;
    }

    // The challenger must clear the incumbent by a margin, not merely exceed
    // it -- and the margin is measured against the incumbent's score RIGHT
    // NOW, not against whatever it scored when it was chosen.
    //
    // Comparing against a carried-forward score (which is what the design
    // document's section 15.2 reference does, and what this originally did)
    // has a failure mode worth spelling out: an action selected at 0.90 that
    // subsequently collapses to 0.10 keeps a 0.90 bar in front of every
    // challenger. A challenger at 0.90 cannot clear 0.90 + 0.05, so the agent
    // stays locked on an action that is now scoring 0.10, indefinitely. It
    // surfaces as "the guard kept taking cover after the threat was gone",
    // which reads as a tuning problem and is not one.
    //
    // incumbent_scored is false when a higher bucket was satisfied first, and
    // an unscored incumbent correctly presents no bar at all.
    const float bar = incumbent_scored ? incumbent_score : 0.0f;
    const bool should_switch =
        best_index != state.current_action &&
        best > bar + behavior.policy.switch_threshold;

    if (should_switch) {
        state.current_action = best_index;
        state.time_in_current_action = 0.0f;
        result.switched = true;
        state.current_score = best;
    } else {
        state.current_score = bar;
    }

    state.force_reevaluate = false;
    result.action = state.current_action;
    result.score = state.current_score;
    return result;
}

std::vector<float> ScoreAll(const CompiledBehavior& behavior,
                            const InputCache& inputs) {
    std::vector<float> scores;
    scores.reserve(behavior.actions.size());
    for (const CompiledAction& action : behavior.actions) {
        scores.push_back(ScoreAction(action, inputs, 0.0f));
    }
    return scores;
}

}  // namespace aib
