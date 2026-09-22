// APE graph-spec builder: registration knobs -> v6 graph JSON.
//
// One exact document shape for `com.sap.abap.cds.reader.v2` (Initial Load /
// Replication, New / Existing subscription). Key order is pinned by the
// golden test: the engine parses this JSON, so a drifted key is a failed
// graph, not a cosmetic diff.
#pragma once

#include <string>

#include "ape_decode.hpp"

namespace erpl_rev {
namespace ape {

// Engine default package size when registration states none (0).
inline constexpr long long kDefaultChunkSize = 20000;

struct ApeGraphSpec {
    std::string graph_id;            // /Attributes/graphid (becomes APPID)
    std::string cds_name;            // plain CDS entity name
    std::string action;              // "Initial Load" | "Replication"
    std::string subscription_type;   // "New" | "Existing"
    std::string subscription_name;   // New only
    std::string subscription_id;     // Existing only
    long long chunk_size = 0;        // 0 = kDefaultChunkSize
    std::string wireformat;          // "" = omitted (engine default)
    // ADR-1: the v6 reader has no filter path. A spec carrying one is
    // refused here rather than emitted and silently ignored by the engine.
    bool has_filter = false;
};

// Throws ApeDecodeError (ape_decode.hpp) on any refusal.
std::string BuildGraphJson(const ApeGraphSpec &spec);

// Registration row -> graph spec: APE_FULL runs Initial Load, APE_DELTA
// Replication, both on a New subscription (a FULL scan owns its subscription
// and erases it afterwards; DELTA resume-by-existing arrives in Phase 3).
// subscription_name_override lets a FULL scan mint a unique-per-scan name
// (the engine refuses create when the name exists, protocol §11); empty
// keeps subscriber_process. chunk_size 0 = engine default, wireformat "" =
// omitted. Throws ApeDecodeError on any other method or a nameless sub.
// subscription_id resumes an Existing subscription (DELTA): when non-empty
// the spec is Existing + id (the name override is ignored); otherwise New as
// above. Resuming the wrong name/id pair fails in the engine, loudly.
ApeGraphSpec MakeGraphSpec(const std::string &method, const std::string &cds_name,
                           const std::string &subscriber_process,
                           const std::string &subscription_name_override,
                           long long chunk_size, const std::string &wireformat,
                           const std::string &subscription_id = "");

}  // namespace ape
}  // namespace erpl_rev
