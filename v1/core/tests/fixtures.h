// Behavior fixtures shared by the test suites.

#ifndef AIB_TEST_FIXTURES_H
#define AIB_TEST_FIXTURES_H

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "aib/compiled.h"
#include "aib/selection.h"

namespace fixtures {

using namespace aib;

inline void Rebuild(CompiledBehavior& b) {
    std::uint8_t max_bucket = 0;
    for (const auto& a : b.actions) {
        if (a.bucket > max_bucket) max_bucket = a.bucket;
    }
    b.actions_in_bucket.assign(static_cast<std::size_t>(max_bucket) + 1, {});
    for (std::size_t i = 0; i < b.actions.size(); ++i) {
        b.actions_in_bucket[b.actions[i].bucket].push_back(
            static_cast<std::uint16_t>(i));
    }
    b.version = ComputeVersion(b);
}

// An action whose considerations are all flat curves at `value`, so its score
// is exactly value^n before compensation. Used to make the multiplication bias
// arithmetic checkable by hand.
inline CompiledAction FlatAction(const std::string& name, int n, float value,
                                 std::uint8_t bucket = 0) {
    CompiledAction a;
    a.name = name;
    a.bucket = bucket;
    a.weight = 1.0f;
    for (int i = 0; i < n; ++i) {
        CompiledConsideration c;
        c.input_index = 0;
        c.input_min = 0.0f;
        c.input_max = 1.0f;
        c.curve = Curve{CurveType::Linear, 0.0f, 1.0f, value, 0.0f};
        a.considerations.push_back(c);
    }
    return a;
}

// The behavior from the design document's section 5.1 sample, built directly
// rather than parsed, so the core tests do not depend on the YAML layer.
inline CompiledBehavior Guard() {
    CompiledBehavior b;
    b.name = "GuardBehavior";
    b.input_names = {"DistanceToTarget", "Health", "AmmoRatio",
                     "TimeSinceLastSeen"};
    b.task_names = {"MoveToCover", "FireAtTarget", "Patrol"};

    CompiledAction take_cover;
    take_cover.name = "TakeCover";
    take_cover.bucket = 0;
    take_cover.task_index = 0;
    take_cover.considerations = {
        // Health: low health -> take cover. Falling logistic.
        {1, 0.0f, 1.0f, Curve{CurveType::Logistic, -1.0f, 1.0f, 0.0f, 0.0f}},
        // Distance: closer -> more urgent. Quadratic falloff.
        {0, 0.0f, 3000.0f, Curve{CurveType::Polynomial, -1.0f, 2.0f, 1.0f, 0.0f}},
    };

    CompiledAction attack;
    attack.name = "Attack";
    attack.bucket = 1;
    attack.task_index = 1;
    attack.considerations = {
        {0, 0.0f, 3000.0f, Curve{CurveType::Polynomial, -1.0f, 1.0f, 1.0f, 0.0f}},
        {2, 0.0f, 1.0f, Curve{CurveType::Linear, 1.0f, 1.0f, 0.0f, 0.0f}},
    };

    CompiledAction patrol;
    patrol.name = "Patrol";
    patrol.bucket = 2;
    patrol.task_index = 2;
    patrol.weight = 0.5f;
    patrol.considerations = {
        {3, 0.0f, 30.0f, Curve{CurveType::Linear, 1.0f, 1.0f, 0.0f, 0.0f}},
    };

    b.actions = {take_cover, attack, patrol};
    b.policy.evaluation_hz = 5.0f;
    b.policy.current_action_bonus = 1.1f;
    b.policy.min_commitment_seconds = 0.5f;
    b.policy.switch_threshold = 0.05f;
    b.policy.bucket_satisfied_threshold = 0.3f;
    Rebuild(b);
    return b;
}

// Many actions in one bucket with varied consideration counts and curves --
// the shape that makes early-out actually prune, so the equivalence test has
// something to prove.
inline CompiledBehavior ManyActions(int count = 24) {
    CompiledBehavior b;
    b.name = "ManyActions";
    b.input_names = {"A", "B", "C", "D", "E", "F"};
    b.task_names = {"Noop"};

    const CurveType types[] = {CurveType::Linear, CurveType::Polynomial,
                               CurveType::Logistic, CurveType::Logit};

    for (int i = 0; i < count; ++i) {
        CompiledAction a;
        a.name = "Action" + std::to_string(i);
        a.bucket = static_cast<std::uint8_t>(i % 3);
        a.weight = 0.8f + 0.02f * static_cast<float>(i % 10);
        const int n = 1 + (i % 5);
        for (int j = 0; j < n; ++j) {
            CompiledConsideration c;
            c.input_index = static_cast<std::uint16_t>((i + j) % 6);
            c.input_min = 0.0f;
            c.input_max = 1.0f;
            c.curve = Curve{types[(i + j) % 4],
                            ((i + j) % 2 == 0) ? 1.0f : -1.0f,
                            1.0f + static_cast<float>((i + j) % 3),
                            ((i + j) % 3 == 0) ? 0.1f : 0.0f, 0.0f};
            a.considerations.push_back(c);
        }
        b.actions.push_back(a);
    }
    // Bucket satisfaction off, so every bucket is scored and early-out has to
    // be correct across all of them rather than being masked by a break.
    b.policy.bucket_satisfied_threshold = 2.0f;
    Rebuild(b);
    return b;
}

// Two actions that score within a hair of each other -- the configuration the
// design document says produces visible twitching.
inline CompiledBehavior TwoCloseActions() {
    CompiledBehavior b;
    b.name = "TwoCloseActions";
    b.input_names = {"X", "Y"};
    b.task_names = {"Noop"};

    CompiledAction a;
    a.name = "ActionA";
    a.bucket = 0;
    a.considerations = {
        {0, 0.0f, 1.0f, Curve{CurveType::Linear, 1.0f, 1.0f, 0.0f, 0.0f}}};

    CompiledAction c;
    c.name = "ActionB";
    c.bucket = 0;
    c.considerations = {
        {1, 0.0f, 1.0f, Curve{CurveType::Linear, 1.0f, 1.0f, 0.0f, 0.0f}}};

    b.actions = {a, c};
    b.policy.current_action_bonus = 1.1f;
    b.policy.min_commitment_seconds = 0.5f;
    b.policy.switch_threshold = 0.05f;
    b.policy.bucket_satisfied_threshold = 2.0f;
    Rebuild(b);
    return b;
}

// A tiny deterministic PRNG, so "random inputs" means the same thing on every
// machine and a failure can be reproduced from the seed alone.
struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ull) {}

    std::uint32_t Next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<std::uint32_t>(state >> 32);
    }
    float Unit() {
        return static_cast<float>(Next() % 1000001u) / 1000000.0f;
    }
};

inline InputCache RandomInputs(Rng& rng, std::size_t n) {
    InputCache cache;
    cache.values.resize(n);
    for (std::size_t i = 0; i < n; ++i) cache.values[i] = rng.Unit();
    return cache;
}

}  // namespace fixtures

#endif  // AIB_TEST_FIXTURES_H
