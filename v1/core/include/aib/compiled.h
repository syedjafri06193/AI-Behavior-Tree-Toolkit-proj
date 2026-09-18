// The compiled runtime format (design.md sections 5.4, 6.2, 11.2).
//
// YAML is for humans. The runtime loads this: flat arrays, indices instead of
// names, no parsing at load. Section 11.2 lists "resolve names to indices at
// compile time" among the performance levers, and it is also what makes the
// hot loop free of allocation and of string comparison.
//
// The format is little-endian and fixed-layout. It is produced by the
// TypeScript compiler in editor/src/format/compile.ts and consumed here, so
// the two live in different languages and must agree exactly -- which is what
// core/tests/test_crosscheck.cpp checks.

#ifndef AIB_COMPILED_H
#define AIB_COMPILED_H

#include <cstdint>
#include <string>
#include <vector>

#include "aib/curve.h"

namespace aib {

// Bumped whenever the binary layout changes. A runtime that loads a file it
// does not understand must refuse rather than misread it: a silently
// misinterpreted float array produces an agent that behaves almost correctly,
// which is far worse than one that fails to load.
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kMagic = 0x42494141;  // 'AAIB' little-endian

struct CompiledConsideration {
    std::uint16_t input_index = 0;  // into InputCache::values
    float input_min = 0.0f;
    float input_max = 1.0f;
    Curve curve;
};

struct CompiledAction {
    std::string name;
    std::uint8_t bucket = 0;
    float weight = 1.0f;
    bool interruptible = true;
    std::uint16_t task_index = 0;
    std::vector<CompiledConsideration> considerations;
};

// Selection policy (design.md section 3.5). All three anti-oscillation
// mechanisms are here because any one alone leaves visible flicker in some
// configurations.
struct SelectionPolicy {
    float evaluation_hz = 5.0f;
    float current_action_bonus = 1.1f;
    float min_commitment_seconds = 0.5f;
    float switch_threshold = 0.05f;
    // A bucket producing a winner above this is "satisfied": lower buckets are
    // never scored at all (section 3.6, and it compounds with early-out).
    float bucket_satisfied_threshold = 0.3f;
};

struct CompiledBehavior {
    std::string name;
    std::uint32_t version = 0;  // content hash, recorded with every decision
    std::vector<std::string> input_names;   // index -> name, for the debugger
    std::vector<std::string> task_names;
    std::vector<CompiledAction> actions;
    // actions_in_bucket[b] holds indices into `actions`, in the stable order
    // the compiler emitted. Precomputed so the hot loop never filters.
    std::vector<std::vector<std::uint16_t>> actions_in_bucket;
    SelectionPolicy policy;

    std::uint8_t BucketCount() const {
        return static_cast<std::uint8_t>(actions_in_bucket.size());
    }
    int InputIndex(const std::string& name) const;
    int ActionIndex(const std::string& name) const;
};

// --------------------------------------------------------------- serialize

// Serialize to the .aib binary. Deterministic: the same behavior always
// produces byte-identical output, so a compiled artifact can be content-hashed
// and cached.
std::vector<std::uint8_t> Serialize(const CompiledBehavior& behavior);

enum class LoadStatus {
    Ok = 0,
    BadMagic,
    UnsupportedVersion,
    Truncated,
    Malformed,
};

const char* LoadStatusName(LoadStatus status) noexcept;

// Load from bytes. Every read is bounds-checked against the buffer length:
// this parses data that may have come off a network socket or out of a stale
// cooked asset, and a malformed file must fail cleanly rather than read past
// the end of the buffer.
LoadStatus Deserialize(const std::uint8_t* data, std::size_t size,
                       CompiledBehavior& out);

LoadStatus LoadFromFile(const std::string& path, CompiledBehavior& out);
bool SaveToFile(const std::string& path, const CompiledBehavior& behavior);

// Recompute the content version. Covers everything that changes behavior and
// nothing that does not, so a recorded session can tell whether the behavior
// it was recorded against still matches (section 9.2).
std::uint32_t ComputeVersion(const CompiledBehavior& behavior);

}  // namespace aib

#endif  // AIB_COMPILED_H
