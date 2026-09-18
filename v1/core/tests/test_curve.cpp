// Response curves (design.md sections 3.2, 16.1).

#include <vector>

#include "aib/curve.h"
#include "test.h"

using namespace aib;

TEST(curves_never_produce_nan) {
    // The design document singles this out: "That third test matters more than
    // it looks. The logit curve goes to infinity at the domain boundaries, and
    // an unclamped NaN silently zeroes an action forever -- a bug that presents
    // as 'this behavior never fires' with no error anywhere."
    //
    // The grid below is wider than the document's, because the bad cases are
    // at the extremes: negative bases with fractional exponents, huge slopes
    // that overflow exp(), and shifts that push the logit past its asymptotes.
    const CurveType types[] = {CurveType::Linear, CurveType::Polynomial,
                               CurveType::Logistic, CurveType::Logit};
    const float ms[] = {-100.0f, -2.0f, -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 2.0f, 100.0f};
    const float ks[] = {0.0f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 0.333f};
    const float bs[] = {-1.0f, -0.25f, 0.0f, 0.25f, 1.0f};
    const float cs[] = {-1.0f, -0.5f, 0.0f, 0.25f, 0.5f, 1.0f};
    const float xs[] = {0.0f, 1e-7f, 0.001f, 0.25f, 0.5f, 0.75f, 0.999f,
                        1.0f - 1e-7f, 1.0f};

    long checked = 0;
    for (CurveType type : types) {
        for (float m : ms) {
            for (float k : ks) {
                for (float b : bs) {
                    for (float c : cs) {
                        for (float x : xs) {
                            const Curve curve{type, m, k, b, c};
                            const float y = EvaluateCurve(curve, x);
                            ++checked;
                            if (!std::isfinite(y) || y < 0.0f || y > 1.0f) {
                                char buf[256];
                                std::snprintf(buf, sizeof(buf),
                                              "%s m=%g k=%g b=%g c=%g x=%g -> %g",
                                              CurveTypeName(type), m, k, b, c, x, y);
                                CHECK_MSG(false, buf);
                                return;  // one report is enough
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK(checked > 50000);
}

TEST(out_of_domain_inputs_are_clamped_not_rejected) {
    // A raw input can arrive outside its declared range -- health above max
    // after a buff, distance beyond the normalization ceiling. Clamping is the
    // right answer; propagating is not.
    const Curve linear{CurveType::Linear, 1.0f, 1.0f, 0.0f, 0.0f};
    CHECK_NEAR(EvaluateCurve(linear, -5.0f), 0.0f, 1e-6);
    CHECK_NEAR(EvaluateCurve(linear, 5.0f), 1.0f, 1e-6);

    const float nan_value = std::nan("");
    CHECK_NEAR(EvaluateCurve(linear, nan_value), 0.0f, 1e-6);
    CHECK(std::isfinite(EvaluateCurve(linear, nan_value)));
}

TEST(linear_is_the_identity_at_default_parameters) {
    const Curve curve{CurveType::Linear, 1.0f, 1.0f, 0.0f, 0.0f};
    for (float x = 0.0f; x <= 1.0f; x += 0.1f) {
        CHECK_NEAR(EvaluateCurve(curve, x), x, 1e-5);
    }
}

TEST(polynomial_shapes_match_the_formula) {
    // y = m(x - c)^k + b. The guard behavior in the samples uses
    // m=-1, k=2, b=1 for "closer is better, and sharply so".
    const Curve falloff{CurveType::Polynomial, -1.0f, 2.0f, 1.0f, 0.0f};
    CHECK_NEAR(EvaluateCurve(falloff, 0.0f), 1.0f, 1e-5);
    CHECK_NEAR(EvaluateCurve(falloff, 0.5f), 0.75f, 1e-5);
    CHECK_NEAR(EvaluateCurve(falloff, 1.0f), 0.0f, 1e-5);

    // Quadratic falls off more slowly near zero than linear, which is the
    // whole reason a designer reaches for it.
    const Curve linear{CurveType::Polynomial, -1.0f, 1.0f, 1.0f, 0.0f};
    CHECK(EvaluateCurve(falloff, 0.3f) > EvaluateCurve(linear, 0.3f));
}

TEST(logistic_is_monotonic_and_centred) {
    const Curve rising{CurveType::Logistic, 1.0f, 1.0f, 0.0f, 0.0f};
    CHECK_NEAR(EvaluateCurve(rising, 0.5f), 0.5f, 1e-4);

    float previous = -1.0f;
    for (float x = 0.0f; x <= 1.0f; x += 0.05f) {
        const float y = EvaluateCurve(rising, x);
        CHECK(y >= previous - 1e-6f);
        previous = y;
    }

    // Negative slope inverts it. "Low health means take cover" is exactly
    // this, and it is how the sample behavior is authored.
    const Curve falling{CurveType::Logistic, -1.0f, 1.0f, 0.0f, 0.0f};
    CHECK(EvaluateCurve(falling, 0.1f) > EvaluateCurve(falling, 0.9f));
}

TEST(logit_is_finite_at_both_asymptotes) {
    // The specific failure the design document warns about: a logit at x=0 or
    // x=1 diverges. Both ends must come back saturated, not infinite.
    const Curve curve{CurveType::Logit, 1.0f, 1.0f, 0.0f, 0.0f};
    const float at_zero = EvaluateCurve(curve, 0.0f);
    const float at_one = EvaluateCurve(curve, 1.0f);

    CHECK(std::isfinite(at_zero));
    CHECK(std::isfinite(at_one));
    CHECK_NEAR(at_zero, 0.0f, 0.05);
    CHECK_NEAR(at_one, 1.0f, 0.05);
    CHECK_NEAR(EvaluateCurve(curve, 0.5f), 0.5f, 1e-4);
}

TEST(normalization_handles_inverted_and_degenerate_ranges) {
    CHECK_NEAR(Normalize(500.0f, 0.0f, 1000.0f), 0.5f, 1e-6);
    CHECK_NEAR(Normalize(-10.0f, 0.0f, 1000.0f), 0.0f, 1e-6);
    CHECK_NEAR(Normalize(5000.0f, 0.0f, 1000.0f), 1.0f, 1e-6);

    // Inverted range flips the input, which is how "closer is better" is
    // authored without needing a mirrored curve.
    CHECK_NEAR(Normalize(0.0f, 1000.0f, 0.0f), 1.0f, 1e-6);
    CHECK_NEAR(Normalize(1000.0f, 1000.0f, 0.0f), 0.0f, 1e-6);

    // Zero width would divide by zero. The validator rejects it at author
    // time; this is the belt to that braces.
    CHECK_NEAR(Normalize(5.0f, 3.0f, 3.0f), 0.0f, 1e-6);
    CHECK(std::isfinite(Normalize(5.0f, 3.0f, 3.0f)));
}

TEST(zero_slope_produces_a_constant_which_the_validator_should_flag) {
    // m = 0 is legal arithmetic and almost always an authoring mistake: the
    // consideration contributes a constant and can never discriminate. The
    // core evaluates it without complaint; catching it is section 10.4's job,
    // and the TypeScript validator has the matching rule.
    const Curve flat{CurveType::Linear, 0.0f, 1.0f, 0.5f, 0.0f};
    CHECK_NEAR(EvaluateCurve(flat, 0.0f), 0.5f, 1e-6);
    CHECK_NEAR(EvaluateCurve(flat, 1.0f), 0.5f, 1e-6);
}

TEST_MAIN("curve", {
    RUN(curves_never_produce_nan);
    RUN(out_of_domain_inputs_are_clamped_not_rejected);
    RUN(linear_is_the_identity_at_default_parameters);
    RUN(polynomial_shapes_match_the_formula);
    RUN(logistic_is_monotonic_and_centred);
    RUN(logit_is_finite_at_both_asymptotes);
    RUN(normalization_handles_inverted_and_degenerate_ranges);
    RUN(zero_slope_produces_a_constant_which_the_validator_should_flag);
})
