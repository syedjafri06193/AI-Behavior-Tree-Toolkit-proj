// Response curves (design.md section 3.2).
//
// A consideration takes a raw input, normalizes it to [0,1], and passes it
// through one of four parameterized curve families. Four families cover
// essentially everything designers need, and giving them four knobs each
// (m, k, b, c) is what makes the curve editor the main design surface rather
// than a list of presets.
//
// THE CLAMP IS NOT COSMETIC. A logit at x=0 or x=1 diverges, and pow() with a
// negative base and a fractional exponent returns NaN. An unclamped NaN
// propagates through the score multiply and produces an action that never
// wins, with no error anywhere -- it presents as "this behavior never fires"
// and costs a day to find. Every path through Evaluate() returns a finite
// value in [0,1], and curve_never_produces_nan asserts that across the whole
// parameter grid.

#ifndef AIB_CURVE_H
#define AIB_CURVE_H

#include <cstdint>

namespace aib {

enum class CurveType : std::uint8_t {
    Linear = 0,
    Polynomial = 1,
    Logistic = 2,  // S-curve: slow, fast, slow. The "more is better" default.
    Logit = 3,     // inverse-S: fast, slow, fast. For thresholds.
};

struct Curve {
    CurveType type = CurveType::Linear;
    float m = 1.0f;  // slope
    float k = 1.0f;  // exponent (Linear/Polynomial) or amplitude (Logistic)
    float b = 0.0f;  // vertical shift
    float c = 0.0f;  // horizontal shift
};

// Evaluate at x, which is clamped to [0,1] on the way in. The result is
// always finite and always in [0,1].
float EvaluateCurve(const Curve& curve, float x) noexcept;

// Normalize a raw input onto [0,1].
//
// An inverted range (max < min) is legal and useful: it flips the input, so
// "closer is better" needs no separate curve. A zero-width range would divide
// by zero, so it returns 0 -- and the validator rejects it at author time
// (section 10.4) rather than letting it reach here.
float Normalize(float raw, float input_min, float input_max) noexcept;

const char* CurveTypeName(CurveType type) noexcept;

}  // namespace aib

#endif  // AIB_CURVE_H
