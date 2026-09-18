#include "aib/compiled.h"

#include <cstring>
#include <fstream>

namespace aib {
namespace {

// Explicit little-endian read/write rather than memcpy of the native layout.
// The game process and the editor are frequently different machines, and this
// format travels over the debug socket as well as onto disk.

void PutU8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }

void PutU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

void PutF32(std::vector<std::uint8_t>& out, float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    PutU32(out, bits);
}

void PutString(std::vector<std::uint8_t>& out, const std::string& s) {
    PutU16(out, static_cast<std::uint16_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t pos = 0;
    bool ok = true;

    bool Need(std::size_t n) {
        if (!ok || pos + n > size) {
            ok = false;
            return false;
        }
        return true;
    }
    std::uint8_t U8() {
        if (!Need(1)) return 0;
        return data[pos++];
    }
    std::uint16_t U16() {
        if (!Need(2)) return 0;
        const std::uint16_t v = static_cast<std::uint16_t>(
            data[pos] | (static_cast<std::uint16_t>(data[pos + 1]) << 8));
        pos += 2;
        return v;
    }
    std::uint32_t U32() {
        if (!Need(4)) return 0;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(data[pos + i]) << (8 * i);
        }
        pos += 4;
        return v;
    }
    float F32() {
        const std::uint32_t bits = U32();
        float v = 0.0f;
        std::memcpy(&v, &bits, 4);
        return v;
    }
    std::string Str() {
        const std::uint16_t n = U16();
        if (!Need(n)) return {};
        std::string s(reinterpret_cast<const char*>(data + pos), n);
        pos += n;
        return s;
    }
};

// FNV-1a. Not cryptographic, and does not need to be: this only has to change
// when the behavior changes, so a recorded session can notice it was made
// against different content.
constexpr std::uint32_t kFnvOffset = 2166136261u;
constexpr std::uint32_t kFnvPrime = 16777619u;

inline void HashBytes(std::uint32_t& h, const void* p, std::size_t n) {
    const auto* bytes = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= bytes[i];
        h *= kFnvPrime;
    }
}

inline void HashF32(std::uint32_t& h, float v) {
    // Normalize -0.0 to +0.0 so two behaviors that differ only in the sign of
    // a zero hash the same -- they score identically, so they are the same
    // behavior, and a version that changed there would be a false alarm.
    if (v == 0.0f) v = 0.0f;
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    HashBytes(h, &bits, 4);
}

}  // namespace

int CompiledBehavior::InputIndex(const std::string& name) const {
    for (std::size_t i = 0; i < input_names.size(); ++i) {
        if (input_names[i] == name) return static_cast<int>(i);
    }
    return -1;
}

int CompiledBehavior::ActionIndex(const std::string& name) const {
    for (std::size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

std::uint32_t ComputeVersion(const CompiledBehavior& behavior) {
    std::uint32_t h = kFnvOffset;
    HashBytes(h, behavior.name.data(), behavior.name.size());

    // Input and task names are hashed because an index that means something
    // different is a behavior change even when every number is identical.
    for (const auto& n : behavior.input_names) {
        HashBytes(h, n.data(), n.size());
    }
    for (const auto& n : behavior.task_names) {
        HashBytes(h, n.data(), n.size());
    }

    for (const auto& a : behavior.actions) {
        HashBytes(h, a.name.data(), a.name.size());
        HashBytes(h, &a.bucket, 1);
        HashF32(h, a.weight);
        const std::uint8_t interruptible = a.interruptible ? 1 : 0;
        HashBytes(h, &interruptible, 1);
        HashBytes(h, &a.task_index, sizeof(a.task_index));
        for (const auto& c : a.considerations) {
            HashBytes(h, &c.input_index, sizeof(c.input_index));
            HashF32(h, c.input_min);
            HashF32(h, c.input_max);
            const std::uint8_t curve_type = static_cast<std::uint8_t>(c.curve.type);
            HashBytes(h, &curve_type, 1);
            HashF32(h, c.curve.m);
            HashF32(h, c.curve.k);
            HashF32(h, c.curve.b);
            HashF32(h, c.curve.c);
        }
    }

    HashF32(h, behavior.policy.evaluation_hz);
    HashF32(h, behavior.policy.current_action_bonus);
    HashF32(h, behavior.policy.min_commitment_seconds);
    HashF32(h, behavior.policy.switch_threshold);
    HashF32(h, behavior.policy.bucket_satisfied_threshold);
    return h;
}

std::vector<std::uint8_t> Serialize(const CompiledBehavior& behavior) {
    std::vector<std::uint8_t> out;
    out.reserve(256 + behavior.actions.size() * 64);

    PutU32(out, kMagic);
    PutU32(out, kFormatVersion);
    PutU32(out, behavior.version != 0 ? behavior.version
                                      : ComputeVersion(behavior));
    PutString(out, behavior.name);

    PutU16(out, static_cast<std::uint16_t>(behavior.input_names.size()));
    for (const auto& n : behavior.input_names) PutString(out, n);

    PutU16(out, static_cast<std::uint16_t>(behavior.task_names.size()));
    for (const auto& n : behavior.task_names) PutString(out, n);

    PutF32(out, behavior.policy.evaluation_hz);
    PutF32(out, behavior.policy.current_action_bonus);
    PutF32(out, behavior.policy.min_commitment_seconds);
    PutF32(out, behavior.policy.switch_threshold);
    PutF32(out, behavior.policy.bucket_satisfied_threshold);

    PutU16(out, static_cast<std::uint16_t>(behavior.actions.size()));
    for (const auto& a : behavior.actions) {
        PutString(out, a.name);
        PutU8(out, a.bucket);
        PutU8(out, a.interruptible ? 1 : 0);
        PutF32(out, a.weight);
        PutU16(out, a.task_index);
        PutU16(out, static_cast<std::uint16_t>(a.considerations.size()));
        for (const auto& c : a.considerations) {
            PutU16(out, c.input_index);
            PutF32(out, c.input_min);
            PutF32(out, c.input_max);
            PutU8(out, static_cast<std::uint8_t>(c.curve.type));
            PutF32(out, c.curve.m);
            PutF32(out, c.curve.k);
            PutF32(out, c.curve.b);
            PutF32(out, c.curve.c);
        }
    }

    // The bucket index is derived, not stored: recomputing it on load is a
    // handful of microseconds and removes a way for the file to be internally
    // inconsistent.
    return out;
}

const char* LoadStatusName(LoadStatus status) noexcept {
    switch (status) {
        case LoadStatus::Ok: return "ok";
        case LoadStatus::BadMagic: return "not an .aib file";
        case LoadStatus::UnsupportedVersion: return "unsupported format version";
        case LoadStatus::Truncated: return "file ends mid-record";
        case LoadStatus::Malformed: return "index out of range";
        default: return "unknown";
    }
}

LoadStatus Deserialize(const std::uint8_t* data, std::size_t size,
                       CompiledBehavior& out) {
    out = CompiledBehavior{};
    Reader r{data, size};

    if (r.U32() != kMagic) return LoadStatus::BadMagic;
    const std::uint32_t format = r.U32();
    if (!r.ok) return LoadStatus::Truncated;
    if (format != kFormatVersion) return LoadStatus::UnsupportedVersion;

    out.version = r.U32();
    out.name = r.Str();

    const std::uint16_t input_count = r.U16();
    out.input_names.reserve(input_count);
    for (std::uint16_t i = 0; i < input_count && r.ok; ++i) {
        out.input_names.push_back(r.Str());
    }

    const std::uint16_t task_count = r.U16();
    out.task_names.reserve(task_count);
    for (std::uint16_t i = 0; i < task_count && r.ok; ++i) {
        out.task_names.push_back(r.Str());
    }

    out.policy.evaluation_hz = r.F32();
    out.policy.current_action_bonus = r.F32();
    out.policy.min_commitment_seconds = r.F32();
    out.policy.switch_threshold = r.F32();
    out.policy.bucket_satisfied_threshold = r.F32();

    const std::uint16_t action_count = r.U16();
    if (!r.ok) return LoadStatus::Truncated;
    out.actions.resize(action_count);

    std::uint8_t max_bucket = 0;
    for (std::uint16_t i = 0; i < action_count; ++i) {
        CompiledAction& a = out.actions[i];
        a.name = r.Str();
        a.bucket = r.U8();
        a.interruptible = r.U8() != 0;
        a.weight = r.F32();
        a.task_index = r.U16();
        const std::uint16_t cons_count = r.U16();
        if (!r.ok) return LoadStatus::Truncated;
        a.considerations.resize(cons_count);
        for (std::uint16_t j = 0; j < cons_count; ++j) {
            CompiledConsideration& c = a.considerations[j];
            c.input_index = r.U16();
            c.input_min = r.F32();
            c.input_max = r.F32();
            const std::uint8_t curve_type = r.U8();
            if (curve_type > static_cast<std::uint8_t>(CurveType::Logit)) {
                return LoadStatus::Malformed;
            }
            c.curve.type = static_cast<CurveType>(curve_type);
            c.curve.m = r.F32();
            c.curve.k = r.F32();
            c.curve.b = r.F32();
            c.curve.c = r.F32();
            if (!r.ok) return LoadStatus::Truncated;
            // An input index past the table would read off the end of the
            // input cache in the hot loop, where there is no bounds check by
            // design. Catch it once, here.
            if (c.input_index >= out.input_names.size()) {
                return LoadStatus::Malformed;
            }
        }
        if (a.task_index >= out.task_names.size()) {
            return LoadStatus::Malformed;
        }
        if (a.bucket > max_bucket) max_bucket = a.bucket;
    }
    if (!r.ok) return LoadStatus::Truncated;

    out.actions_in_bucket.assign(static_cast<std::size_t>(max_bucket) + 1, {});
    for (std::uint16_t i = 0; i < action_count; ++i) {
        out.actions_in_bucket[out.actions[i].bucket].push_back(i);
    }

    return LoadStatus::Ok;
}

LoadStatus LoadFromFile(const std::string& path, CompiledBehavior& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return LoadStatus::Truncated;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    return Deserialize(bytes.data(), bytes.size(), out);
}

bool SaveToFile(const std::string& path, const CompiledBehavior& behavior) {
    const std::vector<std::uint8_t> bytes = Serialize(behavior);
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

}  // namespace aib
