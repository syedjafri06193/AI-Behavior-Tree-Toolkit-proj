#include "aib/curve.h"

#include <cmath>

namespace aib {
namespace {

inline float Clamp01(float v) noexcept {
    // Written as `!(v > 0)` rather than `v < 0` on purpose: NaN compares false
    // against everything, so `v < 0` would let it through and `!(v > 0)`
    // catches it. That single negation is the whole defence against the
    // silent-NaN bug the design document warns about, so it is worth the
    // slightly awkward phrasing.
    if (!(v > 0.0f)) {
        return 0.0f;  // negative, zero, or NaN
    }
    return (v > 1.0f) ? 1.0f : v;
}

// pow() with a negative base and a non-integral exponent is NaN. Designers do
// write c > 0 with k = 0.5, which makes (x - c) negative for small x, so this
// case is reached by ordinary authoring rather than by abuse.
inline float SafePow(float base, float exponent) noexcept {
    if (base < 0.0f) {
        const float rounded = std::nearbyint(exponent);
        if (std::fabs(exponent - rounded) > 1e-6f) {
            // Mirror the positive branch and carry the sign for odd exponents.
            // Continuous through zero, which a bare 0 would not be.
            const float magnitude = std::pow(-base, exponent);
            return -magnitude;
        }
    }
    return std::pow(base, exponent);
}

}  // namespace

float Normalize(float raw, float input_min, float input_max) noexcept {
    const float span = input_max - input_min;
    if (span == 0.0f || span != span) {
        return 0.0f;
    }
    return Clamp01((raw - input_min) / span);
}

float EvaluateCurve(const Curve& curve, float x) noexcept {
    x = Clamp01(x);
    float y = 0.0f;

    switch (curve.type) {
        case CurveType::Linear:
        case CurveType::Polynomial:
            // y = m(x - c)^k + b
            y = curve.m * SafePow(x - curve.c, curve.k) + curve.b;
            break;

        case CurveType::Logistic: {
            // y = k / (1 + e^(-10m(x - 0.5 - c))) + b
            //
            // The exponent is clamped before exp() rather than after: a large
            // |m| with x far from centre overflows to inf, and inf/inf is NaN.
            // +-80 saturates the sigmoid to within a float epsilon of 0 or 1,
            // so clamping there changes no answer anyone can observe.
            float exponent = -10.0f * curve.m * (x - 0.5f - curve.c);
            if (!(exponent > -80.0f)) exponent = -80.0f;
            if (exponent > 80.0f) exponent = 80.0f;
            y = (curve.k / (1.0f + std::exp(exponent))) + curve.b;
            break;
        }

        case CurveType::Logit: {
            // y = m * ln((x - c) / (1 - (x - c))) / 5 + 0.5 + b
            //
            // This is the one the design document calls out: it diverges at
            // both ends of its domain. Shifting the argument off the
            // boundaries is what keeps it finite; the value there is already
            // saturated, so the clamp costs nothing real.
            //
            // The denominator is carried alongside rather than recomputed as
            // (1 - t). At the upper clamp t is 0.9999, and `1.0f - 0.9999f` in
            // float32 is catastrophic cancellation: the true 1e-4 comes back
            // as 1.00017e-4, a relative error of 1.7e-4 in a value the
            // logarithm is about to amplify. Carrying the exact epsilon costs
            // nothing and is what lets the editor's float64 implementation
            // agree with this one to float32 precision -- which matters,
            // because a curve the editor draws differently from the one the
            // runtime evaluates makes every tuning decision made in the editor
            // a decision about the wrong curve.
            constexpr float kEps = 1e-4f;
            float t = x - curve.c;
            float u;
            if (t <= kEps) {
                t = kEps;
                u = 1.0f - kEps;
            } else if (t >= 1.0f - kEps) {
                t = 1.0f - kEps;
                u = kEps;
            } else {
                u = 1.0f - t;
            }
            y = curve.m * std::log(t / u) / 5.0f + 0.5f + curve.b;
            break;
        }

        default:
            y = 0.0f;
            break;
    }

    return Clamp01(y);
}

const char* CurveTypeName(CurveType type) noexcept {
    switch (type) {
        case CurveType::Linear: return "linear";
        case CurveType::Polynomial: return "polynomial";
        case CurveType::Logistic: return "logistic";
        case CurveType::Logit: return "logit";
        default: return "unknown";
    }
}

}  // namespace aib
