// The performance budget from design.md section 11, measured.
//
//     200 agents x 30 actions x 5 considerations = 30,000 evaluations
//     "At 10 ns each that's 0.3 ms -- fine."
//
// Game AI gets 1-2 ms of a 16.7 ms frame. This measures the scoring core
// against that, so the claim is a number rather than an estimate.
//
// Section 11.3 is right that shipping numbers come from Unreal Insights in a
// cooked build -- editor performance is not shipping performance. This is the
// cheaper check that runs before the engine is involved at all: if the core
// itself is over budget, no amount of engine-side profiling will help.

#include <chrono>
#include <cstdio>
#include <vector>

#include "aib/selection.h"
#include "fixtures.h"

using namespace aib;
using namespace fixtures;

namespace {

CompiledBehavior BenchBehavior(int actions, int considerations_each) {
    CompiledBehavior b;
    b.name = "Bench";
    for (int i = 0; i < 16; ++i) b.input_names.push_back("In" + std::to_string(i));
    b.task_names = {"Noop"};

    const CurveType types[] = {CurveType::Linear, CurveType::Polynomial,
                               CurveType::Logistic, CurveType::Logit};

    for (int i = 0; i < actions; ++i) {
        CompiledAction a;
        a.name = "A" + std::to_string(i);
        a.bucket = static_cast<std::uint8_t>(i % 4);
        a.weight = 1.0f;
        for (int j = 0; j < considerations_each; ++j) {
            CompiledConsideration c;
            c.input_index = static_cast<std::uint16_t>((i * 3 + j) % 16);
            c.input_min = 0.0f;
            c.input_max = 1.0f;
            // Rising curves with a floor, so scores land in the 0.2-0.9 band
            // a tuned behavior actually produces. Curves that mostly return
            // near zero would make the bucket threshold never fire and the
            // benchmark would measure the wrong thing.
            c.curve = Curve{types[(i + j) % 4], 1.0f, 1.0f, 0.15f, 0.0f};
            a.considerations.push_back(c);
        }
        b.actions.push_back(a);
    }
    b.policy.min_commitment_seconds = 0.0f;
    Rebuild(b);
    return b;
}

struct Result {
    double total_ms;
    double per_agent_us;
    long long considerations;
};

Result RunBench(const CompiledBehavior& behavior, int agents, bool early_out,
                bool buckets, int repeats) {
    CompiledBehavior tuned = behavior;
    // Turning the bucket break off is how the benchmark isolates each lever
    // rather than reporting one combined number.
    tuned.policy.bucket_satisfied_threshold = buckets ? 0.3f : 2.0f;

    std::vector<SelectionState> states(static_cast<std::size_t>(agents));
    std::vector<InputCache> inputs(static_cast<std::size_t>(agents));
    Rng rng(99);
    for (int i = 0; i < agents; ++i) {
        inputs[static_cast<std::size_t>(i)] =
            RandomInputs(rng, tuned.input_names.size());
    }

    long long considerations = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int r = 0; r < repeats; ++r) {
        for (int i = 0; i < agents; ++i) {
            const SelectionResult result =
                Select(tuned, inputs[static_cast<std::size_t>(i)],
                       states[static_cast<std::size_t>(i)], 0.1f, {early_out});
            considerations += result.considerations_evaluated;
        }
    }
    const auto end = std::chrono::steady_clock::now();

    const double total_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    Result out;
    out.total_ms = total_ms / repeats;
    out.per_agent_us = (total_ms * 1000.0) / (repeats * agents);
    out.considerations = considerations / repeats;
    return out;
}

void Report(const char* label, const Result& r, int agents) {
    std::printf("  %-30s %8.3f ms  %7.2f us/agent  %8lld considerations\n",
                label, r.total_ms, r.per_agent_us, r.considerations);
    (void)agents;
}

}  // namespace

int main() {
    // The design document's stated shape.
    constexpr int kAgents = 200;
    constexpr int kActions = 30;
    constexpr int kConsiderations = 5;
    constexpr int kRepeats = 200;

    const CompiledBehavior behavior = BenchBehavior(kActions, kConsiderations);

    std::printf("design.md section 11: %d agents x %d actions x %d "
                "considerations\n",
                kAgents, kActions, kConsiderations);
    std::printf("budget: AI gets 1-2 ms of a 16.7 ms frame at 60 fps\n\n");

    std::printf("One full evaluation of every agent:\n");
    const Result naive = RunBench(behavior, kAgents, false, false, kRepeats);
    const Result eo = RunBench(behavior, kAgents, true, false, kRepeats);
    const Result bk = RunBench(behavior, kAgents, false, true, kRepeats);
    const Result both = RunBench(behavior, kAgents, true, true, kRepeats);

    Report("everything scored", naive, kAgents);
    Report("+ early-out", eo, kAgents);
    Report("+ buckets", bk, kAgents);
    Report("+ both", both, kAgents);

    std::printf("\n  early-out prunes    %.0f%% of the work\n",
                100.0 * (1.0 - static_cast<double>(eo.considerations) /
                                   static_cast<double>(naive.considerations)));
    std::printf("  buckets prune       %.0f%%\n",
                100.0 * (1.0 - static_cast<double>(bk.considerations) /
                                   static_cast<double>(naive.considerations)));
    std::printf("  together            %.0f%%\n",
                100.0 * (1.0 - static_cast<double>(both.considerations) /
                                   static_cast<double>(naive.considerations)));

    // Section 11.2's first and largest lever: evaluate at 5-10 Hz, not 60.
    // The cost below is per *evaluation*; at 10 Hz on a 60 fps frame only a
    // sixth of the agents are due on any given frame, and section 11.2 says to
    // stagger them so that sixth is spread rather than spiking.
    // Honest reading of the numbers above: early-out removes about a third
    // of the considerations and buys almost no wall-clock time here. The bound
    // check costs a compensation (a divide and two multiplies) per
    // consideration, and against arithmetic this cheap that roughly cancels
    // what it saves.
    //
    // That is not an argument against early-out -- it is an argument about
    // what it is for. Section 3.4 says to order cheap, discriminating
    // considerations first, and section 11.1 points out that a consideration
    // doing a navmesh query costs microseconds rather than nanoseconds. Skipping
    // one of those is worth thousands of bound checks. Early-out pays for
    // itself in proportion to how expensive the considerations it skips are,
    // and on pure arithmetic it is approximately free in both directions.
    if (eo.total_ms > naive.total_ms * 0.95) {
        std::printf("\n  Note: early-out pruned %.0f%% of considerations and saved\n"
                    "  no measurable time. The bound check costs about what this\n"
                    "  arithmetic costs. It pays when considerations are expensive\n"
                    "  -- which is exactly when it matters (sections 3.4, 11.1).\n",
                    100.0 * (1.0 - static_cast<double>(eo.considerations) /
                                       static_cast<double>(naive.considerations)));
    }

    std::printf("\nPer frame at 60 fps, staggered (section 11.2):\n");
    for (float hz : {60.0f, 10.0f, 5.0f}) {
        const double fraction = static_cast<double>(hz) / 60.0;
        const double per_frame_ms = both.total_ms * fraction;
        std::printf("  %4.0f Hz  %6.1f agents/frame  %7.4f ms  %s\n", hz,
                    kAgents * fraction, per_frame_ms,
                    per_frame_ms < 1.0 ? "within budget" : "OVER 1 ms");
    }

    std::printf("\nNote: a consideration that does a navmesh query or a line\n"
                "trace costs microseconds, not nanoseconds (section 11.1).\n"
                "These numbers are the scoring arithmetic alone; the input\n"
                "gathering is the part that actually blows budgets, which is\n"
                "why section 11.2 lever 5 is to cache expensive inputs once\n"
                "per evaluation and share them across considerations.\n");
    return 0;
}
