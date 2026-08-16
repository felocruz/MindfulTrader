// TurtleSoupFusion.h — Turtle Soup Predator-ization, Option A (tick-reactive geometric
// heuristic), per docs/superpowers/specs/2026-08-16-turtle-soup-predator-ization-spec.md.
//
// Reuses the same tail-to-body-ratio/close-position-in-range shape already proven and
// audited for Kangaroo Tail (TripleScreen3.cpp:849-877), applied against the 20-bar
// reference extreme instead of a single bar's own range, evaluated on the CURRENT,
// still-forming bar (not sc.Index-1) -- this is what makes it tick-reactive instead of
// the historian-gated once-per-closed-bar check it replaces.

#pragma once

#include "PredatorContext.h"
#include "ClassifierParams.h"

#include <algorithm>
#include <cmath>

struct TurtleSoupMicroSignal {
    float score = 0.0f;       // [-1.0, +1.0]; positive = bullish setup forming, negative = bearish
    float confidence = 0.0f;  // [0.0, 1.0]
    float earliness = 0.0f;   // elapsed-bar-fraction at evaluation time, [0.0, 1.0]
    bool  isValid = false;    // false until a real penetration-then-reject shape is forming
};

// low/high/closeSoFar: the current, still-forming bar's range and last price.
// twentyBarLow/twentyBarHigh: the prior 20-bar reference extremes (unchanged from the
// existing DetectTurtleSoup() call site's prevHighest/prevLowest computation).
inline TurtleSoupMicroSignal EvaluateTurtleSoupOptionA(
    float low, float high, float closeSoFar,
    float twentyBarLow, float twentyBarHigh,
    float atr, float elapsedFraction
) {
    TurtleSoupMicroSignal sig{};
    sig.earliness = elapsedFraction;

    if (high <= low || atr <= 0.0f) {
        return sig;  // isValid stays false
    }
    const float barRangeSoFar = high - low;
    const float closePosition = (barRangeSoFar > 0.001f) ? ((closeSoFar - low) / barRangeSoFar) : 0.5f;

    // Bullish: low penetrated below the 20-bar low, close-so-far has recovered back above it,
    // and the close sits in the upper part of the bar's range-so-far (rejection shape).
    const float bullishPenetration = twentyBarLow - low;  // positive if low broke below
    if (bullishPenetration > 0.0f && closeSoFar > twentyBarLow && closePosition >= 0.55f) {
        const float penetrationToAtr = std::min(1.0f, bullishPenetration / atr);
        sig.score = std::min(1.0f, 0.3f + 0.7f * penetrationToAtr);
        sig.confidence = std::min(1.0f, closePosition);
        sig.isValid = true;
        return sig;
    }

    // Bearish: high penetrated above the 20-bar high, close-so-far has rejected back below it,
    // and the close sits in the lower part of the bar's range-so-far.
    const float bearishPenetration = high - twentyBarHigh;  // positive if high broke above
    if (bearishPenetration > 0.0f && closeSoFar < twentyBarHigh && closePosition <= 0.45f) {
        const float penetrationToAtr = std::min(1.0f, bearishPenetration / atr);
        sig.score = -std::min(1.0f, 0.3f + 0.7f * penetrationToAtr);
        sig.confidence = std::min(1.0f, 1.0f - closePosition);
        sig.isValid = true;
        return sig;
    }

    return sig;  // isValid stays false — no penetrate-then-reject shape forming yet
}

// Option B: hand-crafted logistic regression inference (not m2cgen-generated, not a
// linked ML runtime library -- per docs/superpowers/specs/2026-08-16-ects-prefix-training-infrastructure-spec.md's
// deployment guidance). Weights/bias come from ClassifierParams, loaded from
// config/classifier_params.json's "turtle_soup_option_b" section. Feature order MUST match
// that section's "feature_names": [penetration_atr, close_position, elapsed_fraction].
//
// Golden-vector regression test requirement (mandatory before this is ever wired live):
// this function's output must be verified against the Python model's own predict_proba()
// for a comprehensive set of real inputs -- not just the synthetic weights used here to
// prove the inference math compiles and runs correctly.
inline TurtleSoupMicroSignal EvaluateTurtleSoupOptionB(
    const ClassifierParams& params,
    float penetrationAtr, float closePosition, float elapsedFraction
) {
    TurtleSoupMicroSignal sig{};
    sig.earliness = elapsedFraction;

    if (!params.isLoaded || params.weights.size() != 3) {
        return sig;  // inert until a real trained model is loaded — isValid stays false
    }

    const float logit = params.weights[0] * penetrationAtr
                       + params.weights[1] * closePosition
                       + params.weights[2] * elapsedFraction
                       + params.bias;
    const float probability = 1.0f / (1.0f + std::exp(-logit));

    // Map probability [0,1] to score [-1,+1] the same way Option A's score is signed:
    // > 0.5 is bullish-leaning, < 0.5 is bearish-leaning.
    sig.score = (probability - 0.5f) * 2.0f;
    sig.confidence = std::fabs(probability - 0.5f) * 2.0f;
    sig.isValid = true;
    return sig;
}
