// Session record and replay (design.md section 9).
//
// "Recording a session and replaying it in the editor is probably the
// highest-value debugging feature available, and almost nothing in this space
// has it." AI bugs are frequently non-reproducible: "the guard sometimes runs
// into the wall" takes a day to reproduce and thirty seconds to diagnose once
// you can see it.
//
// You do not need the game state -- only what the AI saw and what it decided.
// At 10 Hz with 30 actions and 20 inputs that is roughly 200 bytes per agent
// per frame, small enough to record continuously.
//
// The payoff beyond reproduction is section 9.4: replay a recorded session
// against an *edited* behavior and diff the decisions. "Your curve change
// makes the guard take cover 40% more often in this recorded encounter" is a
// design review conversation nobody currently gets to have, and it is a small
// amount of code on top of the recording itself.

#ifndef AIB_RECORD_H
#define AIB_RECORD_H

#include <cstdint>
#include <string>
#include <vector>

#include "aib/selection.h"

namespace aib {

struct RecordedFrame {
    double time = 0.0;
    std::uint32_t agent_id = 0;
    float dt = 0.0f;  // the evaluation delta, so replay does not depend on
                      // the frame rate it is replayed at (section 9.3)
    std::vector<float> inputs;
    int selected_action = -1;
    float selected_score = 0.0f;
    bool switched = false;
};

struct Recording {
    std::string behavior_name;
    std::uint32_t behavior_version = 0;  // which compiled asset produced this
    std::uint64_t rng_seed = 0;
    std::vector<RecordedFrame> frames;
};

std::vector<std::uint8_t> SerializeRecording(const Recording& recording);
bool DeserializeRecording(const std::uint8_t* data, std::size_t size,
                          Recording& out);
bool SaveRecording(const std::string& path, const Recording& recording);
bool LoadRecording(const std::string& path, Recording& out);

// ------------------------------------------------------------------ replay

struct ReplayDivergence {
    std::size_t frame_index = 0;
    double time = 0.0;
    int recorded_action = -1;
    int replayed_action = -1;
    float recorded_score = 0.0f;
    float replayed_score = 0.0f;
};

struct ReplayResult {
    std::size_t frames = 0;
    std::size_t matching = 0;
    std::vector<ReplayDivergence> divergences;
    // True when the recording was made against a different compiled behavior.
    // Divergence is expected in that case and is the point of section 9.4;
    // divergence *without* it is a determinism bug.
    bool version_mismatch = false;

    bool Deterministic() const { return divergences.empty(); }
};

// Feed a recording's inputs back through the core and compare the decisions.
//
// Against the same behavior this must match frame for frame -- the scoring
// core has no side effects, no engine dependency and no wall-clock dependency,
// so anything else is a bug. That makes this a strong regression suite: it
// catches a curve change that alters historical behavior, which is exactly
// what you want to know before shipping a tuning pass.
ReplayResult Replay(const Recording& recording, const CompiledBehavior& behavior);

// ------------------------------------------- comparing two behavior versions

struct ActionShare {
    int action = -1;
    std::size_t frames = 0;
    double fraction = 0.0;
};

struct BehaviorDiff {
    std::size_t frames = 0;
    std::size_t decisions_changed = 0;
    double changed_fraction = 0.0;
    std::size_t switches_before = 0;
    std::size_t switches_after = 0;
    std::vector<ActionShare> before;  // sorted by frames, descending
    std::vector<ActionShare> after;
};

// Replay one recording against two behaviors and report how the decisions
// differ. This is section 9.4: it turns tuning from guesswork into
// measurement.
BehaviorDiff DiffBehaviors(const Recording& recording,
                           const CompiledBehavior& before,
                           const CompiledBehavior& after);

}  // namespace aib

#endif  // AIB_RECORD_H
