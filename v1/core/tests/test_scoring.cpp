// Compensation and early-out (design.md sections 3.3, 3.4, 16.1).

#include <cmath>

#include "aib/scoring.h"
#include "aib/selection.h"
#include "fixtures.h"
#include "test.h"

using namespace aib;
using namespace fixtures;

TEST(the_multiplication_bias_is_real_and_this_is_its_size) {
    // The table from section 3.3, reproduced as an assertion so the problem
    // the compensation solves is documented in code rather than only in prose.
    CHECK_NEAR(std::pow(0.9f, 2), 0.810f, 0.001);
    CHECK_NEAR(std::pow(0.9f, 3), 0.729f, 0.001);
    CHECK_NEAR(std::pow(0.9f, 4), 0.656f, 0.001);
    CHECK_NEAR(std::pow(0.9f, 5), 0.590f, 0.001);
    CHECK_NEAR(std::pow(0.9f, 8), 0.430f, 0.001);

    // Uncompensated, an eight-consideration action loses to a two-consideration
    // one by 0.38 -- and it loses *by construction*, no matter how good it is.
    const float gap = std::pow(0.9f, 2) - std::pow(0.9f, 8);
    CHECK(gap > 0.37f);
}

TEST(compensation_removes_the_bias_against_complex_actions) {
    // Section 3.3's worked example, pinned exactly. These four numbers are
    // stated in the design document and the implementation reproduces them.
    CHECK_NEAR(Compensate(std::pow(0.9f, 2), 2), 0.887f, 0.002);
    CHECK_NEAR(Compensate(std::pow(0.9f, 4), 4), 0.825f, 0.002);
    CHECK_NEAR(std::pow(0.9f, 2) - std::pow(0.9f, 4), 0.154f, 0.002);
    CHECK_NEAR(Compensate(std::pow(0.9f, 2), 2) - Compensate(std::pow(0.9f, 4), 4),
               0.062f, 0.002);

    // Section 16.1 writes this same test against *eight* considerations and
    // asserts the compensated gap is under 0.15. That threshold does not hold
    // for the formula the document itself specifies: 2-vs-8 compensates to
    // 0.887 against 0.645, a gap of 0.242. The 0.15 figure fits the 2-vs-4
    // case the document works through in 3.3 and appears to have been carried
    // across to the 2-vs-8 case in the test without being recomputed.
    //
    // See 1—Documentation/v1/notes-on-the-spec.md. What the test was reaching for -- that
    // compensation substantially closes the gap -- is true and is asserted
    // here against the number the formula actually produces.
    const float simple = Compensate(std::pow(0.9f, 2), 2);
    const float complex_ = Compensate(std::pow(0.9f, 8), 8);
    const float raw_gap = std::pow(0.9f, 2) - std::pow(0.9f, 8);
    const float compensated_gap = simple - complex_;

    CHECK_NEAR(raw_gap, 0.380f, 0.002);
    CHECK_NEAR(compensated_gap, 0.242f, 0.003);
    CHECK_MSG(compensated_gap < raw_gap * 0.65f,
              "compensation must close at least a third of the 2-vs-8 gap");
}

TEST(compensation_reduces_the_bias_without_eliminating_it) {
    // Deliberate, and stated as such in section 3.3: "An action with more
    // requirements *should* be slightly harder to satisfy; it just shouldn't be
    // impossible." A test that demanded exact parity would be asserting the
    // wrong thing, so this pins both directions.
    const float two = Compensate(std::pow(0.9f, 2), 2);
    for (int n = 3; n <= 10; ++n) {
        const float raw = std::pow(0.9f, n);
        const float compensated = Compensate(raw, n);
        CHECK_MSG(compensated > raw, "compensation must raise the score");
        CHECK_MSG(compensated < two,
                  "but must not overtake a simpler action at the same quality");
    }

    // And the residual bias shrinks smoothly rather than jumping around --
    // a designer adding a ninth consideration should not see a cliff.
    float previous_gap = 0.0f;
    for (int n = 2; n <= 10; ++n) {
        const float gap = two - Compensate(std::pow(0.9f, n), n);
        CHECK_MSG(gap >= previous_gap - 1e-6f,
                  "residual bias must grow monotonically with n");
        previous_gap = gap;
    }
}

TEST(compensation_is_a_no_op_below_two_considerations) {
    CHECK_NEAR(Compensate(0.5f, 1), 0.5f, 1e-6);
    CHECK_NEAR(Compensate(0.5f, 0), 0.5f, 1e-6);
    CHECK_NEAR(Compensate(0.0f, 5), 0.0f, 1e-6);
    CHECK_NEAR(Compensate(1.0f, 5), 1.0f, 1e-6);
}

TEST(compensation_is_monotonic_in_the_score) {
    // If it ever inverted, a better-scoring action could lose to a worse one
    // with the same consideration count, which would be indefensible.
    for (int n = 1; n <= 8; ++n) {
        float previous = -1.0f;
        for (float s = 0.0f; s <= 1.0f; s += 0.01f) {
            const float c = Compensate(s, n);
            CHECK(c >= previous - 1e-6f);
            CHECK(c >= -1e-6f && c <= 1.0f + 1e-6f);
            previous = c;
        }
    }
}

TEST(early_out_never_changes_the_winner) {
    // Section 16.1's second test, and the one that keeps the optimization
    // honest. Early-out is only valid if it is provably equivalent; an
    // "optimization" that occasionally picks a different action is a bug that
    // shows up under load and nowhere else.
    //
    // 24 actions across 3 buckets, 400 random input sets, both paths.
    const CompiledBehavior behavior = ManyActions(24);
    Rng rng(12345);

    int pruned_total = 0;
    for (int trial = 0; trial < 400; ++trial) {
        const InputCache inputs = RandomInputs(rng, behavior.input_names.size());

        SelectionState state_a;
        SelectionState state_b;
        SelectionOptions on{true};
        SelectionOptions off{false};

        const SelectionResult with = Select(behavior, inputs, state_a, 1.0f, on);
        const SelectionResult without = Select(behavior, inputs, state_b, 1.0f, off);

        pruned_total += with.early_outs;

        if (with.action != without.action) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "trial %d: early-out picked %d, full scoring picked %d",
                          trial, with.action, without.action);
            CHECK_MSG(false, buf);
            return;
        }
    }

    // If early-out never fired, the test proved nothing. This is the guard
    // against a fixture that quietly stops exercising the thing under test.
    CHECK_MSG(pruned_total > 200,
              "early-out must actually be pruning for this test to mean anything");
}

TEST(early_out_prunes_work_rather_than_merely_being_correct) {
    const CompiledBehavior behavior = ManyActions(24);
    Rng rng(777);

    int scored_with = 0;
    int scored_without = 0;
    for (int trial = 0; trial < 100; ++trial) {
        const InputCache inputs = RandomInputs(rng, behavior.input_names.size());
        SelectionState a, b;
        scored_with += Select(behavior, inputs, a, 1.0f, {true}).early_outs;
        (void)Select(behavior, inputs, b, 1.0f, {false});
        ++scored_without;
    }
    CHECK(scored_with > 0);
    CHECK(scored_without == 100);
}

TEST(a_zero_consideration_vetoes_the_action_immediately) {
    // Zero is absorbing under multiplication, so there is nothing left to
    // learn -- and the trace says "vetoed" rather than "early out", because
    // they are different facts and the debugger shows them differently.
    CompiledAction action = FlatAction("Vetoed", 4, 0.9f);
    action.considerations[1].curve =
        Curve{CurveType::Linear, 0.0f, 1.0f, 0.0f, 0.0f};  // constant zero

    InputCache inputs;
    inputs.values = {0.5f};

    const ScoreTrace trace = ScoreActionTraced(action, inputs, 0.0f);
    CHECK_NEAR(trace.score, 0.0f, 1e-6);
    CHECK(trace.outcome == ScoreOutcome::Vetoed);
    CHECK_EQ(trace.stopped_at, 1);
    CHECK_MSG(trace.considerations_evaluated == 2,
              "must stop at the veto, not evaluate the remaining two");
}

TEST(the_trace_reports_which_consideration_caused_the_early_out) {
    // Section 3.4: "Worth surfacing in the debugger: which consideration
    // caused each early-out, and how often. That tells a designer where their
    // expensive checks are."
    const CompiledAction action = FlatAction("Weak", 5, 0.5f);
    InputCache inputs;
    inputs.values = {0.5f};

    const ScoreTrace trace = ScoreActionTraced(action, inputs, 0.9f);
    CHECK(trace.outcome == ScoreOutcome::EarlyOut);
    CHECK(trace.stopped_at >= 0);
    CHECK_MSG(trace.considerations_evaluated < 5,
              "an action that cannot win should not be finished");
}

TEST(weight_scales_the_action_before_considerations) {
    CompiledAction heavy = FlatAction("Heavy", 2, 1.0f);
    heavy.weight = 0.5f;
    InputCache inputs;
    inputs.values = {0.5f};

    // Two considerations at 1.0, weight 0.5 -> raw 0.5, compensated 0.625.
    CHECK_NEAR(ScoreAction(heavy, inputs, 0.0f), Compensate(0.5f, 2), 1e-5);
}

TEST(explain_shows_every_level_of_the_calculation) {
    // Section 8.2: raw input, normalized value, curve applied, running
    // product, and the compensation multiplier. Without all five, "why did it
    // pick that?" is unanswerable.
    const CompiledBehavior behavior = Guard();
    InputCache inputs;
    inputs.values = {930.0f, 0.22f, 0.8f, 5.0f};  // distance, health, ammo, time

    const ActionBreakdown breakdown = Explain(behavior, 0, inputs);

    CHECK_EQ(breakdown.considerations.size(), 2u);
    CHECK_NEAR(breakdown.considerations[0].raw, 0.22f, 1e-5);
    CHECK_NEAR(breakdown.considerations[0].normalized, 0.22f, 1e-5);
    CHECK(breakdown.considerations[0].curve_output > 0.9f);  // low health

    CHECK_NEAR(breakdown.considerations[1].raw, 930.0f, 1e-3);
    CHECK_NEAR(breakdown.considerations[1].normalized, 0.31f, 0.01);
    CHECK(breakdown.considerations[1].curve_output > 0.9f);  // close

    CHECK(breakdown.compensation > 1.0f);
    CHECK_NEAR(breakdown.final_score,
               Compensate(breakdown.raw_product, 2), 1e-5);
    CHECK(breakdown.final_score > breakdown.raw_product);
}

TEST(explain_does_not_stop_at_the_first_veto) {
    // A breakdown that stops at the first zero hides the thing the designer is
    // looking for. They need to see that AllyCount was 0.10 *and* that Health
    // was fine -- not just that something failed.
    CompiledBehavior behavior = Guard();
    behavior.actions[0].considerations[0].curve =
        Curve{CurveType::Linear, 0.0f, 1.0f, 0.0f, 0.0f};  // veto

    InputCache inputs;
    inputs.values = {930.0f, 0.22f, 0.8f, 5.0f};
    const ActionBreakdown breakdown = Explain(behavior, 0, inputs);

    CHECK_EQ(breakdown.considerations.size(), 2u);
    CHECK(breakdown.outcome == ScoreOutcome::Vetoed);
    CHECK_EQ(breakdown.stopped_at, 0);
    CHECK_MSG(breakdown.considerations[1].curve_output > 0.9f,
              "the second consideration must still be reported");
    CHECK_NEAR(breakdown.final_score, 0.0f, 1e-6);
}

TEST_MAIN("scoring", {
    RUN(the_multiplication_bias_is_real_and_this_is_its_size);
    RUN(compensation_removes_the_bias_against_complex_actions);
    RUN(compensation_reduces_the_bias_without_eliminating_it);
    RUN(compensation_is_a_no_op_below_two_considerations);
    RUN(compensation_is_monotonic_in_the_score);
    RUN(early_out_never_changes_the_winner);
    RUN(early_out_prunes_work_rather_than_merely_being_correct);
    RUN(a_zero_consideration_vetoes_the_action_immediately);
    RUN(the_trace_reports_which_consideration_caused_the_early_out);
    RUN(weight_scales_the_action_before_considerations);
    RUN(explain_shows_every_level_of_the_calculation);
    RUN(explain_does_not_stop_at_the_first_veto);
})
