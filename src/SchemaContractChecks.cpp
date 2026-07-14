#include "generated/indicator_binding_policy_generated.h"
#include "generated/indicator_key_registry_generated.h"

#include <cstddef>

namespace {

constexpr bool IsHexLower(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

template <std::size_t N>
constexpr bool IsFixedHexString(const char (&value)[N], std::size_t expected_len) {
    if (N != expected_len + 1) {
        return false;
    }
    for (std::size_t i = 0; i < expected_len; ++i) {
        if (!IsHexLower(value[i])) {
            return false;
        }
    }
    return value[expected_len] == '\0';
}

template <std::size_t N>
constexpr bool IsIsoUtcStamp(const char (&value)[N]) {
    // Expect YYYY-MM-DDTHH:MM:SSZ + null terminator
    if (N != 21) {
        return false;
    }
    return value[4] == '-' && value[7] == '-' && value[10] == 'T' &&
           value[13] == ':' && value[16] == ':' && value[19] == 'Z' &&
           value[20] == '\0';
}

constexpr bool RegistryAndPolicyNamesAlign() {
    if (mts::schema_contract::kIndicatorKeyRegistryRowCount !=
        mts::schema_contract::kIndicatorBindingPolicyRowCount) {
        return false;
    }

    for (std::size_t i = 0; i < mts::schema_contract::kIndicatorKeyRegistryRowCount; ++i) {
        if (!mts::schema_contract::CStrEq(
                mts::schema_contract::kIndicatorKeyRegistryRows[i].indicator_key,
                mts::schema_contract::kIndicatorBindingPolicyRows[i].indicator_key)) {
            return false;
        }
    }

    return true;
}

}  // namespace

namespace mts::schema_contract {

// WS-03 scaffold: compile-time contract checks that ensure regeneration emitted
// structurally valid metadata artifacts consumed by C++ build.
static_assert(IsFixedHexString(kIndicatorBindingPolicySchemaSha256, 64),
              "indicator_binding_policy_generated.h has invalid schema SHA-256 metadata");

static_assert(IsIsoUtcStamp(kIndicatorBindingPolicyGeneratedUtc),
              "indicator_binding_policy_generated.h has invalid generation timestamp metadata");

static_assert(kIndicatorBindingPolicyRowCount == kExpectedManagedIndicatorKeyCount,
              "Indicator binding policy count no longer matches expected managed key count");

static_assert(HasAllExpectedKeys(),
              "Indicator binding policy no longer covers all expected managed keys");

static_assert(HasUniqueKeys(),
              "Indicator binding policy contains duplicate key rows");

static_assert(SharedRowsHaveDualWriters(),
              "shared_wire policy rows must always declare live and training writers");

static_assert(NonWireRowsAreExplicitInternal(),
              "non_wire_internal policy rows must remain sink=none with no writers");

static_assert(kIndicatorKeyRegistryRowCount == kExpectedManagedIndicatorKeyCount,
              "Indicator key registry row count must equal managed key count");

static_assert(RegistryAndPolicyNamesAlign(),
              "Indicator key registry and policy rows must align by key name and order");

}  // namespace mts::schema_contract
