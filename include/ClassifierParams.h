// ClassifierParams.h — config-driven classifier parameter loading, following
// FeatureScaler::LoadConfig()'s exact established idiom: compiled-in safe default
// (empty/inert), overwritten in place if config/classifier_params.json is present and
// contains the requested consumer key. Sectioned by consumer name (mirrors
// execution_params.json/hmm_regime_risk_policy.json's provenance convention) so a second
// consumer (e.g. TRAP's own eventual ECTS application) is a new section, not a new file.

#pragma once

#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <vector>

struct ClassifierParams {
    std::vector<float> weights;
    float bias = 0.0f;
    bool isLoaded = false;

    static constexpr const char* kConfigPathWindows = "/mnt/c/Trading/config/classifier_params.json";

    static ClassifierParams LoadConfig(const std::string& consumerKey,
                                        const std::string& path = kConfigPathWindows) {
        ClassifierParams params{};  // safe default: empty weights, isLoaded false

        std::ifstream f(path);
        if (!f.is_open()) {
            return params;  // missing file -> inert default, never a crash
        }

        try {
            nlohmann::json j;
            f >> j;
            if (!j.contains(consumerKey)) {
                return params;  // unknown consumer key -> inert default
            }
            const auto& section = j.at(consumerKey);
            if (!section.contains("weights") || !section.contains("bias")) {
                return params;
            }
            const auto weightsJson = section.at("weights").get<std::vector<float>>();
            if (weightsJson.empty()) {
                return params;  // placeholder/empty config -> stay inert, don't half-load
            }
            params.weights = weightsJson;
            params.bias = section.at("bias").get<float>();
            params.isLoaded = true;
        } catch (const std::exception&) {
            return ClassifierParams{};  // malformed JSON -> safe default, not a partial/garbage state
        }
        return params;
    }
};
