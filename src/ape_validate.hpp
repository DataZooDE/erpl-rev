// Static APE registration validation (BRD FR-1/FR-2, NFR-2; HLD ADR-1).
//
// Pure and SAP-free by contract: every refusal here fires before anything
// contacts SAP. The live capability probe (FR-2: DHAPE/DHAMB presence, CDS
// kind, annotation, release gate) is ABAP-side at activation time; this unit
// pins the rules that need no system.
#pragma once

#include <string>

namespace erpl_rev {
namespace ape {

// APE_FULL (one-shot seed) or APE_DELTA (resumable replication). Exact,
// uppercase-only: a near-miss method must fail loudly, never run as delta.
bool IsApeMethod(const std::string &method);

struct ApeRegistration {
    std::string method;               // APE_FULL | APE_DELTA
    std::string source;               // CDS entity
    std::string keys;                 // merge/delete key columns
    std::string subscriber_process;   // SAP-side subscription name (FR-1: required)
    std::string cadence;              // NFR-2: micro:* refused for APE
    std::string wireformat;           // optional graph knob
    long long chunk_size = 0;         // 0 = engine default
    bool has_filter = false;          // ADR-1: v6 has no filter path -> refuse
};

// Empty when valid, else the telling error for the operator.
std::string ValidateRegistration(const ApeRegistration &r);

}  // namespace ape
}  // namespace erpl_rev
