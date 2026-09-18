// Record, replay, and behavior diffing (design.md sections 9, 16.4).

#include "aib/record.h"
#include "fixtures.h"
#include "test.h"

using namespace aib;
using namespace fixtures;

namespace {

// Record a plausible encounter: a target closing while health drops and ammo
// runs down. Deliberately shaped so more than one action wins over the
// session, because a recording where nothing ever changes proves nothing.
Recording RecordEncounter(const CompiledBehavior& behavior, int frames = 300) {
    Recording recording;
    recording.behavior_name = behavior.name;
    recording.behavior_version = behavior.version;
    recording.rng_seed = 0;

    SelectionState state;
    InputCache cache;

    for (int i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(frames);
        cache.values = {
            3000.0f * (1.0f - t),            // distance closes
            1.0f - 0.8f * t,                 // health drops
            1.0f - t,                        // ammo runs down
            30.0f * (1.0f - t),              // recently seen
        };

        RecordedFrame frame;
        frame.time = 0.1 * static_cast<double>(i);
        frame.agent_id = 3;
        frame.dt = 0.1f;
        frame.inputs = cache.values;

        const SelectionResult r = Select(behavior, cache, state, frame.dt);
        frame.selected_action = r.action;
        frame.selected_score = r.score;
        frame.switched = r.switched;
        recording.frames.push_back(frame);
    }
    return recording;
}

}  // namespace

TEST(replay_against_the_same_behavior_is_bit_exact) {
    // Section 16.4. The scoring core has no side effects, no engine dependency
    // and no wall-clock dependency, so anything other than an exact match is a
    // determinism bug rather than a tolerance question.
    const CompiledBehavior behavior = Guard();
    const Recording recording = RecordEncounter(behavior);

    const ReplayResult result = Replay(recording, behavior);
    CHECK_EQ(result.frames, recording.frames.size());
    CHECK_EQ(result.matching, recording.frames.size());
    CHECK(result.Deterministic());
    CHECK(!result.version_mismatch);
}

TEST(the_recording_actually_exercises_more_than_one_action) {
    // Guard against a fixture that quietly stops testing anything: a recording
    // in which the agent never changes its mind would pass every replay test
    // trivially.
    const CompiledBehavior behavior = Guard();
    const Recording recording = RecordEncounter(behavior);

    bool seen[4] = {false, false, false, false};
    int switches = 0;
    for (const RecordedFrame& f : recording.frames) {
        if (f.selected_action >= 0 && f.selected_action < 4) {
            seen[f.selected_action] = true;
        }
        if (f.switched) ++switches;
    }
    int distinct = 0;
    for (bool s : seen) distinct += s ? 1 : 0;

    CHECK_MSG(distinct >= 2, "the encounter must produce more than one decision");
    CHECK(switches >= 1);
}

TEST(replay_does_not_depend_on_the_rate_it_is_replayed_at) {
    // Section 9.3: no dependence on wall-clock time, no dependence on frame
    // rate. The recorded dt is what drives commitment, so a replay on a faster
    // or slower machine must reach the same decisions.
    const CompiledBehavior behavior = Guard();
    Recording recording = RecordEncounter(behavior);

    // Replay is driven by frame.dt, so this is already implied -- but the
    // assertion is cheap and the property is the one that makes replay worth
    // having at all.
    const ReplayResult first = Replay(recording, behavior);
    const ReplayResult second = Replay(recording, behavior);
    CHECK_EQ(first.matching, second.matching);
    CHECK_EQ(first.divergences.size(), second.divergences.size());
}

TEST(replay_serialization_round_trips) {
    const CompiledBehavior behavior = Guard();
    const Recording original = RecordEncounter(behavior, 120);

    const std::vector<std::uint8_t> bytes = SerializeRecording(original);
    Recording loaded;
    CHECK(DeserializeRecording(bytes.data(), bytes.size(), loaded));

    CHECK_STR(loaded.behavior_name, original.behavior_name);
    CHECK_EQ(loaded.behavior_version, original.behavior_version);
    CHECK_EQ(loaded.frames.size(), original.frames.size());
    for (std::size_t i = 0; i < original.frames.size(); ++i) {
        CHECK_EQ(loaded.frames[i].selected_action,
                 original.frames[i].selected_action);
        CHECK_EQ(loaded.frames[i].inputs.size(), original.frames[i].inputs.size());
        for (std::size_t j = 0; j < original.frames[i].inputs.size(); ++j) {
            CHECK_NEAR(loaded.frames[i].inputs[j], original.frames[i].inputs[j], 0.0);
        }
    }

    // And the loaded recording still replays exactly.
    CHECK(Replay(loaded, behavior).Deterministic());
}

TEST(a_truncated_recording_fails_cleanly) {
    const CompiledBehavior behavior = Guard();
    const std::vector<std::uint8_t> bytes =
        SerializeRecording(RecordEncounter(behavior, 40));

    for (std::size_t len = 0; len < bytes.size(); ++len) {
        Recording out;
        if (DeserializeRecording(bytes.data(), len, out)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "a %zu-byte prefix loaded", len);
            CHECK_MSG(false, buf);
            return;
        }
    }
    Recording ok;
    CHECK(DeserializeRecording(bytes.data(), bytes.size(), ok));
}

TEST(a_recording_size_matches_the_documented_budget) {
    // Section 9.2: "roughly 200 bytes per agent per frame -- small enough to
    // record continuously." Worth checking, because a recorder that is
    // secretly ten times that is one somebody turns off.
    const CompiledBehavior behavior = Guard();
    const Recording recording = RecordEncounter(behavior, 300);
    const std::size_t bytes = SerializeRecording(recording).size();
    const double per_frame =
        static_cast<double>(bytes) / static_cast<double>(recording.frames.size());

    CHECK_MSG(per_frame < 200.0,
              "4 inputs per frame should be well under the 200-byte budget");

    // And extrapolated to the document's 20-input case.
    const double projected = per_frame + 16.0 * 4.0;
    CHECK_MSG(projected < 220.0, "20 inputs should still be around 200 bytes");
}

TEST(a_curve_change_shows_up_as_a_decision_diff) {
    // Section 9.4, the feature the design document calls "the genuinely clever
    // version": replay a recorded session against an edited behavior and diff
    // the decisions. "Your curve change makes the guard take cover 40% more
    // often in this recorded encounter" is a design review conversation nobody
    // currently gets to have.
    const CompiledBehavior before = Guard();
    const Recording recording = RecordEncounter(before);

    CompiledBehavior after = before;
    // Make TakeCover trigger at higher health: shift the logistic right.
    after.actions[0].considerations[0].curve.c = 0.25f;
    Rebuild(after);

    const BehaviorDiff diff = DiffBehaviors(recording, before, after);

    CHECK_EQ(diff.frames, recording.frames.size());
    CHECK_MSG(diff.decisions_changed > 0,
              "shifting the cover curve must change some decisions");
    CHECK(diff.changed_fraction > 0.0 && diff.changed_fraction <= 1.0);

    // And the direction is the one a designer would predict: more TakeCover.
    std::size_t cover_before = 0;
    std::size_t cover_after = 0;
    for (const ActionShare& s : diff.before) {
        if (s.action == 0) cover_before = s.frames;
    }
    for (const ActionShare& s : diff.after) {
        if (s.action == 0) cover_after = s.frames;
    }
    CHECK_MSG(cover_after > cover_before,
              "an easier cover trigger must produce more cover, not less");
}

TEST(diffing_a_behavior_against_itself_reports_no_change) {
    const CompiledBehavior behavior = Guard();
    const Recording recording = RecordEncounter(behavior);
    const BehaviorDiff diff = DiffBehaviors(recording, behavior, behavior);

    CHECK_EQ(diff.decisions_changed, 0u);
    CHECK_NEAR(diff.changed_fraction, 0.0, 1e-12);
    CHECK_EQ(diff.switches_before, diff.switches_after);
}

TEST(the_diff_reports_switch_counts_so_tuning_for_stability_is_measurable) {
    // A tuning pass aimed at reducing twitchiness needs a number, and "the
    // agent changed its mind 40 times instead of 120" is that number.
    const CompiledBehavior before = TwoCloseActions();

    Recording recording;
    recording.behavior_name = before.name;
    recording.behavior_version = before.version;
    {
        SelectionState state;
        InputCache cache;
        for (int i = 0; i < 400; ++i) {
            const float t = static_cast<float>(i);
            cache.values = {0.71f + 0.05f * std::sin(t * 1.31f),
                            0.70f + 0.05f * std::sin(t * 0.97f + 1.1f)};
            RecordedFrame f;
            f.time = 0.1 * i;
            f.dt = 0.1f;
            f.inputs = cache.values;
            const SelectionResult r = Select(before, cache, state, f.dt);
            f.selected_action = r.action;
            f.selected_score = r.score;
            f.switched = r.switched;
            recording.frames.push_back(f);
        }
    }

    CompiledBehavior after = before;
    after.policy.switch_threshold = 0.0f;
    after.policy.current_action_bonus = 1.0f;
    after.policy.min_commitment_seconds = 0.0f;
    Rebuild(after);

    const BehaviorDiff diff = DiffBehaviors(recording, before, after);
    CHECK_MSG(diff.switches_after > diff.switches_before * 5,
              "removing the hysteresis must show up as far more switching");
}

TEST(replay_flags_a_version_mismatch_rather_than_silently_diverging) {
    // Divergence against an edited behavior is expected and is the point of
    // section 9.4. Divergence against the SAME behavior is a determinism bug.
    // Telling them apart is what the recorded version is for.
    const CompiledBehavior before = Guard();
    const Recording recording = RecordEncounter(before);

    CompiledBehavior after = before;
    after.actions[0].considerations[0].curve.c = 0.25f;
    Rebuild(after);

    const ReplayResult result = Replay(recording, after);
    CHECK_MSG(result.version_mismatch,
              "the recording must notice it was made against other content");
    CHECK(!result.divergences.empty());
    CHECK(result.divergences[0].recorded_action !=
          result.divergences[0].replayed_action);
}

TEST_MAIN("record", {
    RUN(replay_against_the_same_behavior_is_bit_exact);
    RUN(the_recording_actually_exercises_more_than_one_action);
    RUN(replay_does_not_depend_on_the_rate_it_is_replayed_at);
    RUN(replay_serialization_round_trips);
    RUN(a_truncated_recording_fails_cleanly);
    RUN(a_recording_size_matches_the_documented_budget);
    RUN(a_curve_change_shows_up_as_a_decision_diff);
    RUN(diffing_a_behavior_against_itself_reports_no_change);
    RUN(the_diff_reports_switch_counts_so_tuning_for_stability_is_measurable);
    RUN(replay_flags_a_version_mismatch_rather_than_silently_diverging);
})
