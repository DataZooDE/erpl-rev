#include "rfc_metadata.hpp"
#include "rfc_contract.hpp"
#include "sap_uc.hpp"

#include <cstring>
#include <stdexcept>

namespace erpl_rev {

namespace {

void AddParam(RFC_FUNCTION_DESC_HANDLE desc, const char *name, RFCTYPE type,
              RFC_DIRECTION dir, unsigned nuc_len = 0, unsigned uc_len = 0,
              bool optional = false) {
    RFC_PARAMETER_DESC p;
    std::memset(&p, 0, sizeof(p));
    auto uname = std2uc(name);
    uccpy(p.name, uname.data(), sizeof(p.name) / sizeof(SAP_UC));
    p.type = type;
    p.direction = dir;
    p.nucLength = nuc_len;
    p.ucLength = uc_len;
    p.optional = optional ? 1 : 0;
    RFC_ERROR_INFO info;
    if (RfcAddParameter(desc, &p, &info) != RFC_OK)
        throw_rfc(std::string("RfcAddParameter(") + name + ")", info);
}

RFCTYPE SdkType(RfcParamType t) {
    switch (t) {
        case RfcParamType::Char:    return RFCTYPE_CHAR;
        case RfcParamType::String:  return RFCTYPE_STRING;
        case RfcParamType::XString: return RFCTYPE_XSTRING;
    }
    return RFCTYPE_STRING;
}

// Build (once, cached) the descriptor for one FM straight from the contract, so
// the descriptor and the FM zcl_erpl_rev_mkfm generates cannot drift apart --
// which used to be a call-time failure on a customer system, not a build error.
RFC_FUNCTION_DESC_HANDLE DescFor(const char *fm_name) {
    const RfcFm *fm = FindFm(fm_name);
    if (!fm) throw std::runtime_error(std::string("no RFC contract entry for ") + fm_name);

    RFC_ERROR_INFO info;
    auto uname = std2uc(fm_name);
    RFC_FUNCTION_DESC_HANDLE desc = RfcCreateFunctionDesc(uname.data(), &info);
    if (!desc) throw_rfc(std::string("RfcCreateFunctionDesc(") + fm_name + ")", info);
    for (const auto &p : fm->params)
        AddParam(desc, p.name, SdkType(p.type),
                 p.dir == RfcDir::Import ? RFC_IMPORT : RFC_EXPORT,
                 p.nuc_len, p.uc_len, p.optional);
    return desc;
}

} // namespace


// One cached descriptor per FM. The SDK does not copy these, so they must
// outlive every call -- hence the function-local statics.
#define ERPL_DESC(fn, name)                                   \
    RFC_FUNCTION_DESC_HANDLE fn() {                           \
        static RFC_FUNCTION_DESC_HANDLE d = DescFor(name);    \
        return d;                                             \
    }

ERPL_DESC(BuildPingDesc,          "STFC_CONNECTION")
ERPL_DESC(BuildQueryDesc,         "Z_DUCKDB_QUERY")
ERPL_DESC(BuildIngestDesc,        "Z_DUCKDB_INGEST")
ERPL_DESC(BuildSnapshotMergeDesc, "Z_DUCKDB_SNAPSHOT_MERGE")
ERPL_DESC(BuildCdcPlanDesc,       "Z_DUCKDB_CDC_PLAN")
ERPL_DESC(BuildCdcApplyDesc,      "Z_DUCKDB_CDC_APPLY")
ERPL_DESC(BuildOpenDesc,          "Z_DUCKDB_OPEN")
ERPL_DESC(BuildFetchDesc,         "Z_DUCKDB_FETCH")
ERPL_DESC(BuildCloseDesc,         "Z_DUCKDB_CLOSE")

#undef ERPL_DESC

} // namespace erpl_rev
