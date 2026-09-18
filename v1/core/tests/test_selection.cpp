// Buckets, hysteresis and commitment (design.md sections 3.5, 3.6, 16.3).

#include <cmath>

#include "aib/selection.h"
#include "fixtures.h"
#include "test.h"

using namespace aib;
using namespace fixtures;

namespace {

// Two actions whose true scores sit a hair apart, with jitter on top. This is
// the configuration the design document describes: "Two actions scoring 0.71
// and 0.70 will swap every frame as inputs jitter, and the agent visibly
// twitches."
InputCache JitteredInputs(int frame, float amplitude) {
    InputCache cache;
    // Deterministic pseudo-jitter: two incommensurate sinusoids, so the two
    // actions cross repeatedly rather than settling.
    const float t = static_cast<float>(frame);
    cache.values = {
        0.71f + amplitude * std::sin(t * 1.31f),
        0.70f + amplitude * std::sin(t * 0.97f + 1.1f),
    };
    return cache;
}

int CountSwitches(const CompiledBehavior& behavior, float amplitude,
                  int frames = 600) {
    SelectionState state;
    int switches = 0;
    for (int frame = 0; frame < frames; ++frame) {
        const InputCache inputs = JitteredInputs(frame, amplitude);
        // 0.1 s per step: 600 frames is 60 seconds at 10 Hz.
        if (Select(behavior, inputs, state, 0.1f).switched) ++switches;
    }
    return switches;
}

}  // namespace

TEST(near_tied_actions_do_not_thrash) {
    // Section 16.3, as the document writes it.
    const CompiledBehavior behavior = TwoCloseActions();
    const int switches = CountSwitches(behavior, 0.02f);
    CHECK_MSG(switches < 10, "jittered near-ties must not produce a twitch");
}

TEST(all_three_mitigations_are_load_bearing) {
    // "Any one alone leaves visible flicker in some configurations" (3.5).
    // Rather than take that on faith, turn each off and measure.
    const CompiledBehavior tuned = TwoCloseActions();
    const int baseline = CountSwitches(tuned, 0.02f);

    CompiledBehavior none = tuned;
    none.policy.current_action_bonus = 1.0f;
    none.policy.switch_threshold = 0.0f;
    none.policy.min_commitment_seconds = 0.0f;
    const int uncontrolled = CountSwitches(none, 0.02f);

    CHECK_MSG(uncontrolled > 100,
              "without any mitigation this configuration must actually thrash, "
              "or the test is not measuring anything");
    CHECK(baseline < uncontrolled / 10);

    // Each mitigation on its own.
    CompiledBehavior bonus_only = none;
    bonus_only.policy.current_action_bonus = 1.1f;
    CompiledBehavior threshold_only = none;
    threshold_only.policy.switch_threshold = 0.05f;
    CompiledBehavior commitment_only = none;
    commitment_only.policy.min_commitment_seconds = 0.5f;

    const int with_bonus = CountSwitches(bonus_only, 0.02f);
    const int with_threshold = CountSwitches(threshold_only, 0.02f);
    const int with_commitment = CountSwitches(commitment_only, 0.02f);

    // Each helps on its own...
    CHECK(with_bonus < uncontrolled);
    CHECK(with_threshold < uncontrolled);
    CHECK(with_commitment < uncontrolled);

    // ...and all three together beat every one of them alone, which is the
    // claim worth checking.
    CHECK_MSG(baseline <= with_bonus, "all three must beat the bonus alone");
    CHECK_MSG(baseline <= with_threshold, "all three must beat the threshold alone");
    CHECK_MSG(baseline <= with_commitment, "all three must beat commitment alone");
}

TEST(commitment_blocks_a_switch_and_costs_nothing_to_enforce) {
    CompiledBehavior behavior = TwoCloseActions();
    behavior.policy.min_commitment_seconds = 1.0f;

    SelectionState state;
    InputCache inputs;
    inputs.values = {0.9f, 0.1f};
    const SelectionResult first = Select(behavior, inputs, state, 0.1f);
    CHECK_EQ(first.action, 0);

    // Flip the inputs hard. Inside the commitment window nothing should move,
    // and crucially nothing should even be scored.
    inputs.values = {0.1f, 0.9f};
    const SelectionResult blocked = Select(behavior, inputs, state, 0.1f);
    CHECK_EQ(blocked.action, 0);
    CHECK(blocked.committed);
    CHECK_MSG(blocked.actions_scored == 0,
              "the cheapest evaluation is the one that does not happen");

    // Past the window it switches.
    SelectionResult later = blocked;
    for (int i = 0; i < 12; ++i) {
        later = Select(behavior, inputs, state, 0.1f);
    }
    CHECK_EQ(later.action, 1);
}

TEST(force_reevaluate_breaks_commitment_for_interruptible_actions) {
    // "A 'combat begins' event should interrupt a committed idle action
    // immediately" (section 3.5).
    CompiledBehavior behavior = TwoCloseActions();
    behavior.policy.min_commitment_seconds = 10.0f;

    SelectionState state;
    InputCache inputs;
    inputs.values = {0.9f, 0.1f};
    Select(behavior, inputs, state, 0.1f);
    CHECK_EQ(state.current_action, 0);

    inputs.values = {0.1f, 0.9f};
    CHECK_EQ(Select(behavior, inputs, state, 0.1f).action, 0);  // committed

    state.force_reevaluate = true;
    const SelectionResult forced = Select(behavior, inputs, state, 0.1f);
    CHECK_EQ(forced.action, 1);
    CHECK(forced.switched);
    CHECK_MSG(!state.force_reevaluate, "the flag must be consumed, not latched");
}

TEST(an_uninterruptible_action_holds_even_against_force_reevaluate) {
    // The distinction between "combat began, drop what you are doing" and
    // "you are mid-reload, finish it".
    CompiledBehavior behavior = TwoCloseActions();
    behavior.policy.min_commitment_seconds = 10.0f;
    behavior.actions[0].interruptible = false;

    SelectionState state;
    InputCache inputs;
    inputs.values = {0.9f, 0.1f};
    Select(behavior, inputs, state, 0.1f);
    CHECK_EQ(state.current_action, 0);

    inputs.values = {0.1f, 0.9f};
    state.force_reevaluate = true;
    const SelectionResult forced = Select(behavior, inputs, state, 0.1f);
    CHECK_EQ(forced.action, 0);
    CHECK(forced.committed);
}

TEST(a_collapsed_incumbent_does_not_lock_the_agent_out) {
    // Regression. The switch threshold has to be measured against the
    // incumbent's CURRENT score, not the score it had when it was chosen.
    //
    // The design document's section 15.2 reference carries the score forward
    // (`S.currentScore = best`) and compares the next challenger against it.
    // That means an action selected at 0.90 which then collapses to 0.10 keeps
    // a 0.90 bar standing in front of every challenger: a challenger at 0.90
    // cannot clear 0.90 + 0.05, and the agent stays on an action scoring 0.10
    // indefinitely.
    //
    // It presents as "the guard kept taking cover after the threat was gone",
    // which reads as a tuning problem and is not one.
    CompiledBehavior behavior = TwoCloseActions();
    behavior.policy.min_commitment_seconds = 0.5f;

    SelectionState state;
    InputCache inputs;

    // Pick A at 0.9.
    inputs.values = {0.9f, 0.1f};
    CHECK_EQ(Select(behavior, inputs, state, 0.1f).action, 0);
    CHECK_NEAR(state.current_score, 0.9f, 0.01);

    // A collapses, B is strong. Sit inside the commitment window first, so the
    // stale score would have been carried across an evaluation that did no
    // scoring -- which is exactly how the bug arises in practice.
    inputs.values = {0.05f, 0.9f};
    CHECK(Select(behavior, inputs, state, 0.1f).committed);

    // Past commitment, B must take over. Under the carried-forward rule it
    // never would: 0.9 is not greater than 0.9 + 0.05.
    SelectionResult r{};
    for (int i = 0; i < 8; ++i) r = Select(behavior, inputs, state, 0.1f);

    CHECK_MSG(r.action == 1,
              "a challenger at 0.90 must replace an incumbent now at 0.055");
    CHECK_NEAR(r.score, 0.9f, 0.01);
}

TEST(the_incumbent_bonus_does_not_get_pruned_away_by_early_out) {
    // Regression, and a subtle one. The incumbent's score is multiplied by the
    // bonus AFTER scoring, but early-out prunes DURING scoring. If the pruning
    // bound is the raw best-so-far, an incumbent whose bonused score would win
    // can be abandoned partway through -- and then early-out is no longer an
    // exact optimization, it is one that differs only for the currently-running
    // action. That surfaces as an agent that will not stick to anything, under
    // load, and only sometimes.
    //
    // Constructed so the incumbent's true score is just under the challenger's
    // and its bonused score is just over.
    CompiledBehavior behavior;
    behavior.name = "BonusPruning";
    behavior.input_names = {"X"};
    behavior.task_names = {"Noop"};

    // Challenger: one consideration at 0.90.
    CompiledAction challenger = FlatAction("Challenger", 1, 0.90f, 0);
    // Incumbent: four considerations whose product is 0.9^4 = 0.6561,
    // compensated to 0.8253. Bonused by 1.1 that is 0.9078 -- it wins. The
    // running bound dips below 0.90 partway through, so a naive bound prunes it.
    CompiledAction incumbent = FlatAction("Incumbent", 4, 0.9f, 0);

    behavior.actions = {challenger, incumbent};
    behavior.policy.current_action_bonus = 1.1f;
    behavior.policy.min_commitment_seconds = 0.0f;
    behavior.policy.switch_threshold = 0.05f;
    behavior.policy.bucket_satisfied_threshold = 2.0f;
    Rebuild(behavior);

    InputCache inputs;
    inputs.values = {0.5f};

    // Confirm the fixture really is in the interesting regime.
    const float challenger_score = ScoreAction(behavior.actions[0], inputs, 0.0f);
    const float incumbent_raw = ScoreAction(behavior.actions[1], inputs, 0.0f);
    CHECK_NEAR(challenger_score, 0.90f, 0.01);
    CHECK_NEAR(incumbent_raw, 0.8253f, 0.01);
    CHECK_MSG(incumbent_raw < challenger_score,
              "incumbent must lose on raw score");
    CHECK_MSG(incumbent_raw * 1.1f > challenger_score,
              "and win once the bonus is applied, or this proves nothing");

    // With the incumbent running, it must hold -- with early-out on and off.
    for (bool early_out : {true, false}) {
        SelectionState state;
        state.current_action = 1;
        state.current_score = incumbent_raw * 1.1f;
        state.time_in_current_action = 10.0f;

        const SelectionResult r =
            Select(behavior, inputs, state, 0.1f, {early_out});
        CHECK_MSG(r.action == 1,
                  early_out ? "early-out pruned the bonused incumbent"
                            : "full scoring lost the incumbent");
        CHECK(!r.switched);
    }
}

TEST(early_out_matches_full_scoring_with_an_incumbent_present) {
    // early_out_never_changes_the_winner in the scoring suite starts from a
    // fresh state every trial, so no bonus is ever applied and the interaction
    // above goes unexercised. This is the same property with an incumbent
    // carried across evaluations, which is the configuration that actually
    // runs in a game.
    const CompiledBehavior behavior = ManyActions(24);
    Rng rng(20260917);

    // One shared state, forked each frame. Running two independent states
    // would let a single disagreement send the two down different trajectories
    // and turn every later frame into noise; this asks the sharper question --
    // given identical state and identical inputs, do the two paths agree?
    SelectionState shared;

    for (int frame = 0; frame < 500; ++frame) {
        const InputCache inputs = RandomInputs(rng, behavior.input_names.size());

        SelectionState with_state = shared;
        SelectionState without_state = shared;
        const SelectionResult with =
            Select(behavior, inputs, with_state, 1.0f, {true});
        const SelectionResult without =
            Select(behavior, inputs, without_state, 1.0f, {false});

        if (with.action != without.action) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "frame %d: early-out %d (%.6f), full %d (%.6f), "
                          "incumbent was %d",
                          frame, with.action, with.score, without.action,
                          without.score, shared.current_action);
            CHECK_MSG(false, buf);
            return;
        }
        CHECK_NEAR(with.score, without.score, 1e-5);
        CHECK(with.switched == without.switched);

        shared = without_state;
    }
}

TEST(a_satisfied_bucket_stops_lower_buckets_being_scored) {
    // Section 3.6, and it compounds with early-out: an emergency that fires
    // makes the entire ambient tier free.
    CompiledBehavior behavior = Guard();
    behavior.policy.min_commitment_seconds = 0.0f;

    SelectionState state;
    InputCache inputs;
    // Low health, close target: TakeCover in bucket 0 should win outright.
    inputs.values = {200.0f, 0.1f, 1.0f, 30.0f};

    const SelectionResult result = Select(behavior, inputs, state, 1.0f);
    CHECK_EQ(result.action, 0);
    CHECK_EQ(result.buckets_evaluated, 1);
    CHECK_MSG(result.actions_scored == 1,
              "buckets 1 and 2 must not be scored at all");
}

TEST(lower_buckets_run_when_the_higher_one_is_unsatisfied) {
    CompiledBehavior behavior = Guard();
    behavior.policy.min_commitment_seconds = 0.0f;

    SelectionState state;
    InputCache inputs;
    // Full health, target far away: nothing in bucket 0 or 1 scores well, so
    // patrol in bucket 2 should be reached.
    inputs.values = {3000.0f, 1.0f, 0.0f, 30.0f};

    const SelectionResult result = Select(behavior, inputs, state, 1.0f);
    CHECK_EQ(result.buckets_evaluated, 3);
    CHECK_EQ(result.action, 2);  // Patrol
}

TEST(a_high_bucket_beats_a_better_scoring_low_bucket) {
    // "Stops a lucky 'idle' from beating a well-tuned 'flee'."
    CompiledBehavior behavior;
    behavior.name = "Priority";
    behavior.input_names = {"X"};
    behavior.task_names = {"Noop"};

    CompiledAction emergency = FlatAction("Flee", 1, 0.45f, 0);
    CompiledAction ambient = FlatAction("Idle", 1, 0.99f, 1);
    behavior.actions = {emergency, ambient};
    behavior.policy.bucket_satisfied_threshold = 0.3f;
    behavior.policy.min_commitment_seconds = 0.0f;
    Rebuild(behavior);

    SelectionState state;
    InputCache inputs;
    inputs.values = {0.5f};

    const SelectionResult result = Select(behavior, inputs, state, 1.0f);
    CHECK_MSG(result.action == 0,
              "Flee at 0.45 must beat Idle at 0.99 because it is a tier up");
    CHECK_EQ(result.buckets_evaluated, 1);
}

TEST(the_first_evaluation_is_never_blocked_by_commitment) {
    // An agent with no current action must be allowed to pick one, whatever
    // the commitment setting says.
    CompiledBehavior behavior = TwoCloseActions();
    behavior.policy.min_commitment_seconds = 100.0f;

    SelectionState state;
    InputCache inputs;
    inputs.values = {0.9f, 0.1f};
    const SelectionResult first = Select(behavior, inputs, state, 0.01f);
    CHECK_EQ(first.action, 0);
    CHECK(!first.committed);
}

TEST(score_all_ignores_hysteresis_and_buckets) {
    // The debugger's Scores level wants the honest number for every action,
    // not the pruned one -- otherwise the score-history graph shows zeroes for
    // actions that were merely never reached.
    const CompiledBehavior behavior = Guard();
    InputCache inputs;
    inputs.values = {200.0f, 0.1f, 1.0f, 30.0f};

    const std::vector<float> scores = ScoreAll(behavior, inputs);
    CHECK_EQ(scores.size(), 3u);
    for (float s : scores) {
        CHECK(s >= 0.0f && s <= 1.0f);
        CHECK(std::isfinite(s));
    }
    CHECK_MSG(scores[2] > 0.0f,
              "Patrol is in an unreached bucket but still has a real score");
}

TEST(evaluation_rate_is_a_policy_not_a_hard_coded_frequency) {
    // Section 11.2's first lever: evaluate at 5-10 Hz, not 60. Roughly an
    // order of magnitude, and it improves stability for free.
    const CompiledBehavior behavior = TwoCloseActions();
    CHECK_NEAR(behavior.policy.evaluation_hz, 5.0f, 0.01);

    // Switches at 60 Hz against switches at 10 Hz over the same wall time.
    // The point is that the slower rate is not merely cheaper: it thrashes
    // less, because there are fewer opportunities to thrash.
    SelectionState fast_state;
    int fast_switches = 0;
    for (int frame = 0; frame < 3600; ++frame) {
        const InputCache inputs = JitteredInputs(frame, 0.05f);
        if (Select(behavior, inputs, fast_state, 1.0f / 60.0f).switched) {
            ++fast_switches;
        }
    }

    SelectionState slow_state;
    int slow_switches = 0;
    for (int frame = 0; frame < 600; ++frame) {
        const InputCache inputs = JitteredInputs(frame * 6, 0.05f);
        if (Select(behavior, inputs, slow_state, 0.1f).switched) ++slow_switches;
    }

    CHECK_MSG(slow_switches <= fast_switches,
              "a slower evaluation rate must not produce more switching");
}

TEST_MAIN("selection", {
    RUN(near_tied_actions_do_not_thrash);
    RUN(all_three_mitigations_are_load_bearing);
    RUN(commitment_blocks_a_switch_and_costs_nothing_to_enforce);
    RUN(force_reevaluate_breaks_commitment_for_interruptible_actions);
    RUN(an_uninterruptible_action_holds_even_against_force_reevaluate);
    RUN(a_collapsed_incumbent_does_not_lock_the_agent_out);
    RUN(the_incumbent_bonus_does_not_get_pruned_away_by_early_out);
    RUN(early_out_matches_full_scoring_with_an_incumbent_present);
    RUN(a_satisfied_bucket_stops_lower_buckets_being_scored);
    RUN(lower_buckets_run_when_the_higher_one_is_unsatisfied);
    RUN(a_high_bucket_beats_a_better_scoring_low_bucket);
    RUN(the_first_evaluation_is_never_blocked_by_commitment);
    RUN(score_all_ignores_hysteresis_and_buckets);
    RUN(evaluation_rate_is_a_policy_not_a_hard_coded_frequency);
})
