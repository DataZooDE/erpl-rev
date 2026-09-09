#include "cdc_status.hpp"

#include <algorithm>

namespace erpl_rev {
namespace cdc {

namespace {
bool Has(const std::vector<std::string> &v, const std::string &s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
bool IsOurs(const std::string &name) { return name.rfind("ZCDC_", 0) == 0; }
}  // namespace

StatusResult Derive(const Expected &want, const Probe &have, bool deactivated) {
    StatusResult out;

    if (!want.log_table.empty() && !Has(have.tables, want.log_table))
        out.missing.push_back(want.log_table);
    if (!want.seq_name.empty() && !Has(have.sequences, want.seq_name))
        out.missing.push_back(want.seq_name);

    std::vector<std::string> missing_triggers;
    for (const auto &t : want.triggers) {
        // A DISABLED trigger is the worst case: present in the catalogue, so it
        // looks healthy, and capturing nothing. It counts as missing.
        if (!Has(have.enabled_triggers, t)) missing_triggers.push_back(t);
    }

    if (deactivated) {
        // Mass deactivation before an upgrade removes the triggers deliberately
        // and keeps the shadow table, because the position lives with it. That
        // must not read as breakage, or every upgrade raises a false alarm.
        out.status = Status::Disabled;
        out.missing.clear();
        return out;
    }

    out.missing.insert(out.missing.end(), missing_triggers.begin(), missing_triggers.end());
    out.status = out.missing.empty() ? Status::Active : Status::Inconsistent;
    return out;
}

std::vector<std::string> RepairPlan(const StatusResult &s,
                                    const std::vector<std::string> &provision_ddl) {
    std::vector<std::string> plan;
    for (const auto &stmt : provision_ddl)
        for (const auto &m : s.missing)
            if (stmt.find(m) != std::string::npos) { plan.push_back(stmt); break; }
    return plan;
}

std::vector<std::string> FindOrphans(const std::vector<Expected> &registry, const Probe &have) {
    std::vector<std::string> known;
    for (const auto &e : registry) {
        if (!e.log_table.empty()) known.push_back(e.log_table);
        if (!e.seq_name.empty()) known.push_back(e.seq_name);
        known.insert(known.end(), e.triggers.begin(), e.triggers.end());
    }

    std::vector<std::string> orphans;
    auto scan = [&](const std::vector<std::string> &v) {
        for (const auto &n : v)
            if (IsOurs(n) && !Has(known, n)) orphans.push_back(n);
    };
    scan(have.tables);
    scan(have.sequences);
    scan(have.enabled_triggers);
    scan(have.disabled_triggers);
    return orphans;
}

std::string StatusName(Status s) {
    switch (s) {
        case Status::Active: return "ACTIVE";
        case Status::Disabled: return "DISABLED";
        case Status::Inconsistent: return "INCONSISTENT";
    }
    return "INCONSISTENT";
}

std::string Explain(const StatusResult &s) {
    if (s.status == Status::Active) return "trigger set is complete and enabled";
    if (s.status == Status::Disabled)
        return "trigger set is deactivated; the shadow table and its position are kept. "
               "Reactivate to resume, which re-seeds the position and reconciles changes "
               "made while it was down.";
    std::string out = "trigger set is INCONSISTENT. Missing or disabled: ";
    for (size_t i = 0; i < s.missing.size(); ++i) {
        if (i) out += ", ";
        out += s.missing[i];
    }
    out += ". Cycles are refused until this is repaired -- running one would advance "
           "the position past changes that were never captured. Repair recreates only "
           "the missing objects and leaves the position alone.";
    return out;
}

}  // namespace cdc
}  // namespace erpl_rev
