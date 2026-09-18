// A CLI that exposes the core's decisions, so another implementation can be
// checked against it.
//
// The editor has its own scorer (editor/src/runtime/score.ts) because it has
// to evaluate behaviors with no game running -- replay against an edited
// behavior (section 9.4), the always-wins/never-wins validation (10.4), the
// curve editor (10.3). Two implementations of the same decision procedure is a
// liability unless they are checked against each other, and this is how.
//
// The C++ side is normative: it is what ships in the game and what the
// recorded sessions were produced by. When the two disagree, this one is
// right by definition and the other is the bug.
//
//   aib_probe curves                 dump a curve grid as TSV
//   aib_probe score  <file.aib>      read input vectors on stdin, emit scores
//   aib_probe select <file.aib>      the same, but full selection with state
//   aib_probe info   <file.aib>      the header, for a load check

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "aib/record.h"
#include "aib/selection.h"

using namespace aib;

namespace {

// Printed with %.9g: nine significant digits round-trips any float32 exactly,
// so the comparison on the other side is against the true value rather than
// against a rounded one.
constexpr const char* kFloatFmt = "%.9g";

void PrintFloat(float v) { std::printf(kFloatFmt, static_cast<double>(v)); }

int CmdCurves() {
    const CurveType types[] = {CurveType::Linear, CurveType::Polynomial,
                               CurveType::Logistic, CurveType::Logit};
    const float ms[] = {-3.0f, -1.5f, -1.0f, -0.25f, 0.0f, 0.25f, 1.0f, 1.5f, 3.0f};
    const float ks[] = {0.25f, 0.5f, 1.0f, 2.0f, 3.0f};
    const float bs[] = {-0.5f, -0.1f, 0.0f, 0.1f, 0.5f};
    const float cs[] = {-0.5f, -0.2f, 0.0f, 0.2f, 0.5f};

    std::printf("type\tm\tk\tb\tc\tx\ty\n");
    for (CurveType type : types) {
        for (float m : ms) {
            for (float k : ks) {
                for (float b : bs) {
                    for (float c : cs) {
                        for (int i = 0; i <= 20; ++i) {
                            const float x = static_cast<float>(i) / 20.0f;
                            const Curve curve{type, m, k, b, c};
                            std::printf("%d\t", static_cast<int>(type));
                            PrintFloat(m); std::printf("\t");
                            PrintFloat(k); std::printf("\t");
                            PrintFloat(b); std::printf("\t");
                            PrintFloat(c); std::printf("\t");
                            PrintFloat(x); std::printf("\t");
                            PrintFloat(EvaluateCurve(curve, x));
                            std::printf("\n");
                        }
                    }
                }
            }
        }
    }
    return 0;
}

bool ReadInputLine(InputCache& cache, std::size_t expected) {
    cache.values.clear();
    cache.values.reserve(expected);

    std::string line;
    int ch;
    while ((ch = std::getchar()) != EOF && ch != '\n') {
        line.push_back(static_cast<char>(ch));
    }
    if (line.empty() && ch == EOF) return false;

    const char* p = line.c_str();
    char* end = nullptr;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        if (!*p) break;
        const double v = std::strtod(p, &end);
        if (end == p) break;
        cache.values.push_back(static_cast<float>(v));
        p = end;
    }
    return !cache.values.empty();
}

int CmdInfo(const char* path) {
    CompiledBehavior behavior;
    const LoadStatus status = LoadFromFile(path, behavior);
    if (status != LoadStatus::Ok) {
        std::fprintf(stderr, "load failed: %s\n", LoadStatusName(status));
        return 1;
    }
    std::printf("name\t%s\n", behavior.name.c_str());
    std::printf("version\t%u\n", behavior.version);
    std::printf("inputs\t%zu\n", behavior.input_names.size());
    for (const auto& n : behavior.input_names) std::printf("input\t%s\n", n.c_str());
    std::printf("tasks\t%zu\n", behavior.task_names.size());
    for (const auto& n : behavior.task_names) std::printf("task\t%s\n", n.c_str());
    std::printf("actions\t%zu\n", behavior.actions.size());
    for (const auto& a : behavior.actions) {
        std::printf("action\t%s\t%u\t", a.name.c_str(), a.bucket);
        PrintFloat(a.weight);
        std::printf("\t%d\t%zu\n", a.interruptible ? 1 : 0,
                    a.considerations.size());
    }
    std::printf("buckets\t%zu\n", behavior.actions_in_bucket.size());
    std::printf("policy\t");
    PrintFloat(behavior.policy.evaluation_hz); std::printf("\t");
    PrintFloat(behavior.policy.current_action_bonus); std::printf("\t");
    PrintFloat(behavior.policy.min_commitment_seconds); std::printf("\t");
    PrintFloat(behavior.policy.switch_threshold); std::printf("\t");
    PrintFloat(behavior.policy.bucket_satisfied_threshold); std::printf("\n");
    return 0;
}

int CmdScore(const char* path) {
    CompiledBehavior behavior;
    const LoadStatus status = LoadFromFile(path, behavior);
    if (status != LoadStatus::Ok) {
        std::fprintf(stderr, "load failed: %s\n", LoadStatusName(status));
        return 1;
    }

    InputCache cache;
    while (ReadInputLine(cache, behavior.input_names.size())) {
        const std::vector<float> scores = ScoreAll(behavior, cache);
        for (std::size_t i = 0; i < scores.size(); ++i) {
            if (i) std::printf("\t");
            PrintFloat(scores[i]);
        }
        std::printf("\n");
    }
    return 0;
}

int CmdSelect(const char* path, float dt) {
    CompiledBehavior behavior;
    const LoadStatus status = LoadFromFile(path, behavior);
    if (status != LoadStatus::Ok) {
        std::fprintf(stderr, "load failed: %s\n", LoadStatusName(status));
        return 1;
    }

    SelectionState state;
    InputCache cache;
    while (ReadInputLine(cache, behavior.input_names.size())) {
        const SelectionResult r = Select(behavior, cache, state, dt);
        std::printf("%d\t", r.action);
        PrintFloat(r.score);
        std::printf("\t%d\t%d\t%d\t%d\n", r.switched ? 1 : 0, r.committed ? 1 : 0,
                    r.buckets_evaluated, r.considerations_evaluated);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: aib_probe curves\n"
                     "       aib_probe info   <file.aib>\n"
                     "       aib_probe score  <file.aib>   < inputs.tsv\n"
                     "       aib_probe select <file.aib> [dt] < inputs.tsv\n");
        return 2;
    }

    const std::string cmd = argv[1];
    if (cmd == "curves") return CmdCurves();
    if (argc < 3) {
        std::fprintf(stderr, "%s needs a file\n", cmd.c_str());
        return 2;
    }
    if (cmd == "info") return CmdInfo(argv[2]);
    if (cmd == "score") return CmdScore(argv[2]);
    if (cmd == "select") {
        const float dt = (argc > 3) ? static_cast<float>(std::atof(argv[3])) : 1.0f;
        return CmdSelect(argv[2], dt);
    }

    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 2;
}
