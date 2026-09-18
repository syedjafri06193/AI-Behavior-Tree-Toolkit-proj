#include "aib/record.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>

namespace aib {
namespace {

constexpr std::uint32_t kRecMagic = 0x43455241;  // 'AREC'
constexpr std::uint32_t kRecVersion = 1;

void PutU32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void PutU64(std::vector<std::uint8_t>& o, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void PutF32(std::vector<std::uint8_t>& o, float v) {
    std::uint32_t b;
    std::memcpy(&b, &v, 4);
    PutU32(o, b);
}
void PutF64(std::vector<std::uint8_t>& o, double v) {
    std::uint64_t b;
    std::memcpy(&b, &v, 8);
    PutU64(o, b);
}
void PutStr(std::vector<std::uint8_t>& o, const std::string& s) {
    PutU32(o, static_cast<std::uint32_t>(s.size()));
    o.insert(o.end(), s.begin(), s.end());
}

struct Rd {
    const std::uint8_t* d;
    std::size_t n;
    std::size_t p = 0;
    bool ok = true;

    bool Need(std::size_t k) {
        if (!ok || p + k > n) { ok = false; return false; }
        return true;
    }
    std::uint32_t U32() {
        if (!Need(4)) return 0;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(d[p + i]) << (8 * i);
        p += 4;
        return v;
    }
    std::uint64_t U64() {
        if (!Need(8)) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(d[p + i]) << (8 * i);
        p += 8;
        return v;
    }
    float F32() {
        const std::uint32_t b = U32();
        float v = 0.0f;
        std::memcpy(&v, &b, 4);
        return v;
    }
    double F64() {
        const std::uint64_t b = U64();
        double v = 0.0;
        std::memcpy(&v, &b, 8);
        return v;
    }
    std::string Str() {
        const std::uint32_t k = U32();
        if (!Need(k)) return {};
        std::string s(reinterpret_cast<const char*>(d + p), k);
        p += k;
        return s;
    }
};

std::vector<ActionShare> Shares(const std::vector<int>& decisions) {
    std::map<int, std::size_t> counts;
    for (int a : decisions) ++counts[a];

    std::vector<ActionShare> out;
    out.reserve(counts.size());
    for (const auto& [action, frames] : counts) {
        ActionShare s;
        s.action = action;
        s.frames = frames;
        s.fraction = decisions.empty()
                         ? 0.0
                         : static_cast<double>(frames) /
                               static_cast<double>(decisions.size());
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const ActionShare& a, const ActionShare& b) {
        // Frames descending, then action index, so the output is stable for
        // two actions that happened to be chosen equally often.
        if (a.frames != b.frames) return a.frames > b.frames;
        return a.action < b.action;
    });
    return out;
}

// Replay a recording and return the decision per frame. Shared by Replay()
// and DiffBehaviors() so the two cannot drift apart.
std::vector<std::pair<int, float>> RunThrough(const Recording& recording,
                                              const CompiledBehavior& behavior,
                                              std::size_t* switches) {
    std::vector<std::pair<int, float>> out;
    out.reserve(recording.frames.size());

    SelectionState state;
    std::size_t switch_count = 0;
    InputCache cache;

    for (const RecordedFrame& frame : recording.frames) {
        cache.values = frame.inputs;
        // frame.dt, not a wall clock: replay must not depend on how fast the
        // machine replaying it happens to be (section 9.3).
        const SelectionResult r = Select(behavior, cache, state, frame.dt);
        if (r.switched) ++switch_count;
        out.emplace_back(r.action, r.score);
    }

    if (switches) *switches = switch_count;
    return out;
}

}  // namespace

std::vector<std::uint8_t> SerializeRecording(const Recording& recording) {
    std::vector<std::uint8_t> out;
    PutU32(out, kRecMagic);
    PutU32(out, kRecVersion);
    PutStr(out, recording.behavior_name);
    PutU32(out, recording.behavior_version);
    PutU64(out, recording.rng_seed);
    PutU32(out, static_cast<std::uint32_t>(recording.frames.size()));

    for (const RecordedFrame& f : recording.frames) {
        PutF64(out, f.time);
        PutU32(out, f.agent_id);
        PutF32(out, f.dt);
        PutU32(out, static_cast<std::uint32_t>(f.inputs.size()));
        for (float v : f.inputs) PutF32(out, v);
        PutU32(out, static_cast<std::uint32_t>(f.selected_action));
        PutF32(out, f.selected_score);
        out.push_back(f.switched ? 1 : 0);
    }
    return out;
}

bool DeserializeRecording(const std::uint8_t* data, std::size_t size,
                          Recording& out) {
    out = Recording{};
    Rd r{data, size};
    if (r.U32() != kRecMagic) return false;
    if (r.U32() != kRecVersion) return false;

    out.behavior_name = r.Str();
    out.behavior_version = r.U32();
    out.rng_seed = r.U64();

    const std::uint32_t frame_count = r.U32();
    if (!r.ok) return false;
    out.frames.resize(frame_count);

    for (std::uint32_t i = 0; i < frame_count; ++i) {
        RecordedFrame& f = out.frames[i];
        f.time = r.F64();
        f.agent_id = r.U32();
        f.dt = r.F32();
        const std::uint32_t input_count = r.U32();
        if (!r.ok) return false;
        f.inputs.resize(input_count);
        for (std::uint32_t j = 0; j < input_count; ++j) f.inputs[j] = r.F32();
        f.selected_action = static_cast<int>(r.U32());
        f.selected_score = r.F32();
        if (!r.Need(1)) return false;
        f.switched = data[r.p++] != 0;
    }
    return r.ok;
}

bool SaveRecording(const std::string& path, const Recording& recording) {
    const std::vector<std::uint8_t> bytes = SerializeRecording(recording);
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

bool LoadRecording(const std::string& path, Recording& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    return DeserializeRecording(bytes.data(), bytes.size(), out);
}

ReplayResult Replay(const Recording& recording,
                    const CompiledBehavior& behavior) {
    ReplayResult result;
    result.frames = recording.frames.size();
    result.version_mismatch =
        recording.behavior_version != 0 && behavior.version != 0 &&
        recording.behavior_version != behavior.version;

    const std::vector<std::pair<int, float>> replayed =
        RunThrough(recording, behavior, nullptr);

    for (std::size_t i = 0; i < recording.frames.size(); ++i) {
        const RecordedFrame& f = recording.frames[i];
        if (replayed[i].first == f.selected_action) {
            ++result.matching;
            continue;
        }
        ReplayDivergence d;
        d.frame_index = i;
        d.time = f.time;
        d.recorded_action = f.selected_action;
        d.replayed_action = replayed[i].first;
        d.recorded_score = f.selected_score;
        d.replayed_score = replayed[i].second;
        result.divergences.push_back(d);
    }
    return result;
}

BehaviorDiff DiffBehaviors(const Recording& recording,
                           const CompiledBehavior& before,
                           const CompiledBehavior& after) {
    BehaviorDiff diff;
    diff.frames = recording.frames.size();

    std::size_t switches_before = 0;
    std::size_t switches_after = 0;
    const auto a = RunThrough(recording, before, &switches_before);
    const auto b = RunThrough(recording, after, &switches_after);

    diff.switches_before = switches_before;
    diff.switches_after = switches_after;

    std::vector<int> decisions_before;
    std::vector<int> decisions_after;
    decisions_before.reserve(a.size());
    decisions_after.reserve(b.size());

    for (std::size_t i = 0; i < a.size(); ++i) {
        decisions_before.push_back(a[i].first);
        decisions_after.push_back(b[i].first);
        if (a[i].first != b[i].first) ++diff.decisions_changed;
    }

    diff.changed_fraction =
        diff.frames == 0 ? 0.0
                         : static_cast<double>(diff.decisions_changed) /
                               static_cast<double>(diff.frames);
    diff.before = Shares(decisions_before);
    diff.after = Shares(decisions_after);
    return diff;
}

}  // namespace aib
