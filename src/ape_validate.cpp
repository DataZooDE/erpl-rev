#include "ape_validate.hpp"

namespace erpl_rev {
namespace ape {

bool IsApeMethod(const std::string &method) {
    return method == "APE_FULL" || method == "APE_DELTA";
}

namespace {

// Inherited from erpl_ape: at most 30 characters, no control characters.
// The name is operator-chosen and travels to SAP, so a bad one is refused
// here rather than failing inside graph creation.
bool ValidSubscriberName(const std::string &s) {
    if (s.empty() || s.size() > 30) return false;
    for (const unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return false;
    return true;
}

}  // namespace

std::string ValidateRegistration(const ApeRegistration &r) {
    if (!IsApeMethod(r.method))
        return "unknown APE method '" + r.method + "'; expected APE_FULL or APE_DELTA";
    if (r.source.empty())
        return "APE registration requires --source (the CDS entity)";
    if (r.keys.empty())
        return "APE registration requires --keys: every merge and delete is keyed";
    if (!ValidSubscriberName(r.subscriber_process))
        return "invalid subscriber_process '" + r.subscriber_process +
               "': 1-30 characters, no control characters";
    if (r.cadence.rfind("micro:", 0) == 0)
        return "cadence '" + r.cadence +
               "' is refused for APE methods: graph preparation alone takes tens of "
               "seconds (NFR-2); use hourly, nightly or manual";
    if (r.has_filter)
        return "filter refused for APE methods: the v6 reader has no filter path, "
               "so filtering would silently return all rows (ADR-1)";
    return {};
}

}  // namespace ape
}  // namespace erpl_rev
