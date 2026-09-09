#include "cds_delta.hpp"

#include <cctype>
#include <stdexcept>

namespace erpl_rev {
namespace cds {

namespace {
std::string Upper(const std::string &s) {
    std::string r = s;
    for (char &c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

// ADT and the DDIC APIs disagree about the case of annotation names, so the same
// view must derive the same delta whichever produced the values.
std::string Get(const Annotations &a, const std::string &key) {
    const auto want = Upper(key);
    for (const auto &kv : a.values)
        if (Upper(kv.first) == want) return kv.second;
    return {};
}

std::string TypeOf(const Annotations &a, const std::string &element) {
    const auto want = Upper(element);
    for (const auto &kv : a.element_types)
        if (Upper(kv.first) == want) return kv.second;
    return {};
}
}  // namespace

std::string WmKindForType(const std::string &ddic_type) {
    const auto t = Upper(ddic_type);
    // Getting this wrong repeats the bug where wm_kind was decorative: a DATS
    // element treated as a timestamp silently compares as a number.
    if (t == "DATS") return "DATE";
    if (t == "TIMESTAMPL" || t == "TIMESTAMP") return "TIMESTAMPL";
    if (t == "DEC" || t == "INT1" || t == "INT2" || t == "INT4" || t == "INT8" ||
        t == "NUMC" || t == "QUAN")
        return "INT";
    return "NUMTS";
}

Derived Derive(const Annotations &a) {
    Derived d;

    const auto enabled = Get(a, "Analytics.dataExtraction.enabled");
    if (!enabled.empty() && Upper(enabled) == "FALSE")
        // The model says explicitly not to extract this view. Deriving a delta
        // anyway would override a decision someone made on purpose.
        throw std::runtime_error(
            "CDS " + a.entity + " sets Analytics.dataExtraction.enabled: false; "
            "it is not marked for extraction.");

    const auto cdc = Get(a, "Analytics.dataExtraction.delta.changeDataCapture.automatic");
    if (!cdc.empty() && Upper(cdc) == "TRUE") {
        if (a.mappings.empty())
            // Registering nothing would look like a working delta-enabled view
            // that never delivers a change.
            throw std::runtime_error(
                "CDS " + a.entity + " declares changeDataCapture but maps no base tables; "
                "there is nothing to put triggers on.");
        for (const auto &m : a.mappings) {
            if (m.second.empty())
                // Triggers log keys. With no key there is nothing to log and
                // nothing to re-read by.
                throw std::runtime_error(
                    "CDS " + a.entity + ": base table " + m.first +
                    " has no key mapping, so its trigger would have nothing to log.");
            // KEYS_IUD: the values are re-read through the VIEW, which is the
            // point of a CDS delta -- the view's projection is what lands.
            d.trigger_targets.push_back({m.first, m.second, "KEYS_IUD"});
        }
        d.kind = DeltaKind::Trigger;
        return d;
    }

    const auto elem = Get(a, "Analytics.dataExtraction.delta.byElement.name");
    if (!elem.empty()) {
        d.kind = DeltaKind::Watermark;
        d.chg_col = elem;
        d.wm_kind = WmKindForType(TypeOf(a, elem));
        // The model states how late a record may arrive; that is precisely what
        // the safety window is for, so it should not be configured twice.
        const auto delay = Get(a, "Analytics.dataExtraction.delta.byElement.maxDelayInSeconds");
        if (!delay.empty()) d.safety_secs = std::stoll(delay);
        return d;
    }

    return d;   // no delta annotation: still replicable by full load or snapshot
}

}  // namespace cds
}  // namespace erpl_rev
