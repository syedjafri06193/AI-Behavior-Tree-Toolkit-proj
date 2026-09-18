// The compiled runtime format (design.md sections 5.4, 6.2).

#include <cstring>

#include "aib/compiled.h"
#include "fixtures.h"
#include "test.h"

using namespace aib;
using namespace fixtures;

TEST(serialization_round_trips) {
    const CompiledBehavior original = Guard();
    const std::vector<std::uint8_t> bytes = Serialize(original);

    CompiledBehavior loaded;
    CHECK(Deserialize(bytes.data(), bytes.size(), loaded) == LoadStatus::Ok);

    CHECK_STR(loaded.name, original.name);
    CHECK_EQ(loaded.version, original.version);
    CHECK_EQ(loaded.input_names.size(), original.input_names.size());
    CHECK_EQ(loaded.actions.size(), original.actions.size());

    for (std::size_t i = 0; i < original.actions.size(); ++i) {
        const CompiledAction& a = original.actions[i];
        const CompiledAction& b = loaded.actions[i];
        CHECK_STR(b.name, a.name);
        CHECK_EQ(b.bucket, a.bucket);
        CHECK_NEAR(b.weight, a.weight, 0.0);
        CHECK_EQ(b.considerations.size(), a.considerations.size());
        for (std::size_t j = 0; j < a.considerations.size(); ++j) {
            const CompiledConsideration& x = a.considerations[j];
            const CompiledConsideration& y = b.considerations[j];
            CHECK_EQ(y.input_index, x.input_index);
            // Exact, not near: floats go through the format bit for bit, and a
            // format that quietly rounds would make the cross-language check
            // against the TypeScript compiler meaningless.
            CHECK_NEAR(y.input_min, x.input_min, 0.0);
            CHECK_NEAR(y.input_max, x.input_max, 0.0);
            CHECK_EQ(static_cast<int>(y.curve.type), static_cast<int>(x.curve.type));
            CHECK_NEAR(y.curve.m, x.curve.m, 0.0);
            CHECK_NEAR(y.curve.k, x.curve.k, 0.0);
            CHECK_NEAR(y.curve.b, x.curve.b, 0.0);
            CHECK_NEAR(y.curve.c, x.curve.c, 0.0);
        }
    }
}

TEST(serialization_is_deterministic) {
    // The same behavior must always produce byte-identical output, so a
    // compiled artifact can be content-hashed and a cook step can skip work.
    const CompiledBehavior behavior = Guard();
    const std::vector<std::uint8_t> a = Serialize(behavior);
    const std::vector<std::uint8_t> b = Serialize(behavior);
    CHECK_EQ(a.size(), b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}

TEST(round_tripping_through_the_format_reproduces_decisions) {
    // The property that actually matters: not that the bytes survive, but
    // that the loaded behavior decides the same way.
    const CompiledBehavior original = Guard();
    CompiledBehavior loaded;
    const std::vector<std::uint8_t> bytes = Serialize(original);
    CHECK(Deserialize(bytes.data(), bytes.size(), loaded) == LoadStatus::Ok);

    Rng rng(4242);
    for (int trial = 0; trial < 200; ++trial) {
        InputCache inputs;
        inputs.values = {rng.Unit() * 3000.0f, rng.Unit(), rng.Unit(),
                         rng.Unit() * 30.0f};
        SelectionState sa, sb;
        const SelectionResult ra = Select(original, inputs, sa, 1.0f);
        const SelectionResult rb = Select(loaded, inputs, sb, 1.0f);
        CHECK_EQ(ra.action, rb.action);
        CHECK_NEAR(ra.score, rb.score, 1e-6);
    }
}

TEST(the_bucket_index_is_rebuilt_on_load) {
    // Derived rather than stored: recomputing costs microseconds and removes a
    // way for the file to be internally inconsistent with itself.
    const CompiledBehavior original = Guard();
    CompiledBehavior loaded;
    const std::vector<std::uint8_t> bytes = Serialize(original);
    Deserialize(bytes.data(), bytes.size(), loaded);

    CHECK_EQ(loaded.actions_in_bucket.size(), original.actions_in_bucket.size());
    for (std::size_t b = 0; b < original.actions_in_bucket.size(); ++b) {
        CHECK_EQ(loaded.actions_in_bucket[b].size(),
                 original.actions_in_bucket[b].size());
        for (std::size_t i = 0; i < original.actions_in_bucket[b].size(); ++i) {
            CHECK_EQ(loaded.actions_in_bucket[b][i],
                     original.actions_in_bucket[b][i]);
        }
    }
}

TEST(a_truncated_file_fails_cleanly_at_every_length) {
    // This parses data that may have come off a socket or out of a stale
    // cooked asset. Every prefix must be rejected rather than read past the
    // end of the buffer -- a crash here is a crash in a shipping game.
    const std::vector<std::uint8_t> bytes = Serialize(Guard());

    for (std::size_t len = 0; len < bytes.size(); ++len) {
        CompiledBehavior out;
        const LoadStatus status = Deserialize(bytes.data(), len, out);
        if (status == LoadStatus::Ok) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "a %zu-byte prefix of a %zu-byte file loaded as Ok",
                          len, bytes.size());
            CHECK_MSG(false, buf);
            return;
        }
    }
    CompiledBehavior out;
    CHECK(Deserialize(bytes.data(), bytes.size(), out) == LoadStatus::Ok);
}

TEST(bad_magic_and_wrong_version_are_distinguished) {
    std::vector<std::uint8_t> bytes = Serialize(Guard());
    CompiledBehavior out;

    std::vector<std::uint8_t> bad = bytes;
    bad[0] ^= 0xFF;
    CHECK(Deserialize(bad.data(), bad.size(), out) == LoadStatus::BadMagic);

    // A runtime that loads a format it does not understand must refuse rather
    // than misread it: a silently misinterpreted float array produces an agent
    // that behaves almost correctly, which is worse than one that fails.
    std::vector<std::uint8_t> future = bytes;
    future[4] = 99;
    CHECK(Deserialize(future.data(), future.size(), out) ==
          LoadStatus::UnsupportedVersion);
}

TEST(an_out_of_range_input_index_is_rejected_at_load) {
    // The hot loop indexes the input cache without a bounds check, by design.
    // That is only safe because this check exists, once, here.
    CompiledBehavior behavior = Guard();
    behavior.actions[0].considerations[0].input_index = 99;
    const std::vector<std::uint8_t> bytes = Serialize(behavior);

    CompiledBehavior out;
    CHECK(Deserialize(bytes.data(), bytes.size(), out) == LoadStatus::Malformed);
}

TEST(an_out_of_range_task_index_is_rejected_at_load) {
    CompiledBehavior behavior = Guard();
    behavior.actions[1].task_index = 77;
    const std::vector<std::uint8_t> bytes = Serialize(behavior);

    CompiledBehavior out;
    CHECK(Deserialize(bytes.data(), bytes.size(), out) == LoadStatus::Malformed);
}

TEST(an_unknown_curve_type_is_rejected_rather_than_defaulted) {
    // Defaulting to Linear would produce an agent that runs and is wrong,
    // which is the failure mode this format exists to avoid.
    std::vector<std::uint8_t> bytes = Serialize(Guard());

    // Find the first curve type byte by re-serializing with a marker value.
    CompiledBehavior marked = Guard();
    marked.actions[0].considerations[0].curve.type =
        static_cast<CurveType>(3);  // Logit, a valid value
    const std::vector<std::uint8_t> reference = Serialize(marked);

    std::size_t offset = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != reference[i]) {
            offset = i;
            break;
        }
    }
    CHECK(offset > 0);

    bytes[offset] = 42;  // not a curve type
    CompiledBehavior out;
    CHECK(Deserialize(bytes.data(), bytes.size(), out) == LoadStatus::Malformed);
}

TEST(the_content_version_changes_when_behavior_changes) {
    // Recorded sessions carry the version they were made against, so replay
    // can tell "the behavior was edited" from "the core is non-deterministic"
    // (section 9.2). A version that missed a change would make that useless.
    const CompiledBehavior base = Guard();
    const std::uint32_t v = ComputeVersion(base);

    CompiledBehavior curve_changed = base;
    curve_changed.actions[0].considerations[0].curve.k = 2.5f;
    CHECK(ComputeVersion(curve_changed) != v);

    CompiledBehavior range_changed = base;
    range_changed.actions[0].considerations[1].input_max = 2000.0f;
    CHECK(ComputeVersion(range_changed) != v);

    CompiledBehavior bucket_changed = base;
    bucket_changed.actions[1].bucket = 0;
    CHECK(ComputeVersion(bucket_changed) != v);

    CompiledBehavior policy_changed = base;
    policy_changed.policy.switch_threshold = 0.2f;
    CHECK(ComputeVersion(policy_changed) != v);

    CompiledBehavior renamed_input = base;
    renamed_input.input_names[0] = "DistanceToCover";
    CHECK_MSG(ComputeVersion(renamed_input) != v,
              "an index that means something different is a behavior change");

    // And is stable across an identical rebuild.
    CHECK_EQ(ComputeVersion(Guard()), v);
}

TEST(the_content_version_ignores_the_sign_of_zero) {
    // -0.0 and +0.0 score identically, so a version that changed between them
    // would be a false alarm on a recording that is still valid.
    CompiledBehavior base = Guard();
    base.actions[0].considerations[0].curve.b = 0.0f;
    const std::uint32_t v = ComputeVersion(base);

    CompiledBehavior negative_zero = base;
    negative_zero.actions[0].considerations[0].curve.b = -0.0f;
    CHECK_EQ(ComputeVersion(negative_zero), v);
}

TEST(file_io_round_trips) {
    const CompiledBehavior original = Guard();
    const std::string path = "aib_roundtrip_test.aib";
    CHECK(SaveToFile(path, original));

    CompiledBehavior loaded;
    CHECK(LoadFromFile(path, loaded) == LoadStatus::Ok);
    CHECK_STR(loaded.name, original.name);
    CHECK_EQ(loaded.actions.size(), original.actions.size());

    CompiledBehavior missing;
    CHECK(LoadFromFile("definitely-not-here.aib", missing) !=
          LoadStatus::Ok);
}

TEST_MAIN("compiled", {
    RUN(serialization_round_trips);
    RUN(serialization_is_deterministic);
    RUN(round_tripping_through_the_format_reproduces_decisions);
    RUN(the_bucket_index_is_rebuilt_on_load);
    RUN(a_truncated_file_fails_cleanly_at_every_length);
    RUN(bad_magic_and_wrong_version_are_distinguished);
    RUN(an_out_of_range_input_index_is_rejected_at_load);
    RUN(an_out_of_range_task_index_is_rejected_at_load);
    RUN(an_unknown_curve_type_is_rejected_rather_than_defaulted);
    RUN(the_content_version_changes_when_behavior_changes);
    RUN(the_content_version_ignores_the_sign_of_zero);
    RUN(file_io_round_trips);
})
