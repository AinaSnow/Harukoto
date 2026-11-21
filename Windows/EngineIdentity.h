#pragma once
#include "BaseDictionaryFiles.h"
#include <string>

// WIN-03 step 6a: the data a candidate list was computed from. The engine
// reports it in its Open reply; the TSF service computes the same value from
// the data it is using itself and only takes engine candidates when the two
// are equal (docs/ENGINE_TSF.md). Lowercase hex SHA-256 of length-prefixed
// fields, so no field boundary is ambiguous.
namespace harukoto::engine {
// personal: "absent" for a missing table, else SHA-256 of its exact bytes;
// empty when no personal table path is in use at all.
inline std::string personalIdentity(bool present, const std::string &bytes) {
    return present ? base_dictionary::sha256(bytes) : std::string("absent");
}
inline std::string dataIdentity(const std::string &baseGeneration, const std::string &themeGeneration,
                                const std::string &personal) {
    base_dictionary::Sha256 digest;
    const std::string format = "harukoto-engine-data-1";
    for (const std::string *value : {&format, &baseGeneration, &themeGeneration, &personal}) {
        const auto length = std::to_string(value->size()) + ":";
        digest.add(length.data(), static_cast<ULONG>(length.size()));
        digest.add(value->data(), static_cast<ULONG>(value->size()));
    }
    return digest.finish();
}
} // namespace harukoto::engine
