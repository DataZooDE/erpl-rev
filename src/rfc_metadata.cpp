#include "rfc_metadata.hpp"
#include "sap_uc.hpp"

#include <cstring>

namespace erpl_rev {

namespace {

void AddParam(RFC_FUNCTION_DESC_HANDLE desc, const char *name, RFCTYPE type,
              RFC_DIRECTION dir, unsigned nuc_len = 0, unsigned uc_len = 0,
              bool optional = false) {
    RFC_PARAMETER_DESC p;
    std::memset(&p, 0, sizeof(p));
    auto uname = std2uc(name);
    strncpyU(p.name, uname.data(), sizeof(p.name) / sizeof(SAP_UC));
    p.type = type;
    p.direction = dir;
    p.nucLength = nuc_len;
    p.ucLength = uc_len;
    p.optional = optional ? 1 : 0;
    RFC_ERROR_INFO info;
    if (RfcAddParameter(desc, &p, &info) != RFC_OK)
        throw_rfc(std::string("RfcAddParameter(") + name + ")", info);
}

} // namespace

RFC_FUNCTION_DESC_HANDLE BuildPingDesc() {
    static RFC_FUNCTION_DESC_HANDLE desc = nullptr;
    if (desc) return desc;
    // STFC_CONNECTION: standard FM present in every backend (CHAR(255) params).
    RFC_ERROR_INFO info;
    auto uname = std2uc("STFC_CONNECTION");
    desc = RfcCreateFunctionDesc(uname.data(), &info);
    if (!desc) throw_rfc("RfcCreateFunctionDesc(STFC_CONNECTION)", info);
    AddParam(desc, "REQUTEXT", RFCTYPE_CHAR, RFC_IMPORT, 255, 510, true);
    AddParam(desc, "ECHOTEXT", RFCTYPE_CHAR, RFC_EXPORT, 255, 510);
    AddParam(desc, "RESPTEXT", RFCTYPE_CHAR, RFC_EXPORT, 255, 510);
    return desc;
}

RFC_FUNCTION_DESC_HANDLE BuildQueryDesc() {
    static RFC_FUNCTION_DESC_HANDLE desc = nullptr;
    if (desc) return desc;
    RFC_ERROR_INFO info;
    auto uname = std2uc("Z_DUCKDB_QUERY");
    desc = RfcCreateFunctionDesc(uname.data(), &info);
    if (!desc) throw_rfc("RfcCreateFunctionDesc(Z_DUCKDB_QUERY)", info);
    AddParam(desc, "IV_SQL",       RFCTYPE_STRING, RFC_IMPORT, 0, 0, true);
    AddParam(desc, "EV_COLUMNS",   RFCTYPE_STRING, RFC_EXPORT);
    AddParam(desc, "EV_ROWS",      RFCTYPE_STRING, RFC_EXPORT);
    AddParam(desc, "EV_ROW_COUNT", RFCTYPE_STRING, RFC_EXPORT);
    AddParam(desc, "EV_ERROR",     RFCTYPE_STRING, RFC_EXPORT);
    return desc;
}

RFC_FUNCTION_DESC_HANDLE BuildIngestDesc() {
    static RFC_FUNCTION_DESC_HANDLE desc = nullptr;
    if (desc) return desc;
    RFC_ERROR_INFO info;
    auto uname = std2uc("Z_DUCKDB_INGEST");
    desc = RfcCreateFunctionDesc(uname.data(), &info);
    if (!desc) throw_rfc("RfcCreateFunctionDesc(Z_DUCKDB_INGEST)", info);
    AddParam(desc, "IV_TARGET",        RFCTYPE_STRING, RFC_IMPORT, 0, 0, true);
    AddParam(desc, "IV_MODE",          RFCTYPE_STRING, RFC_IMPORT, 0, 0, true);
    AddParam(desc, "IV_KEYS",          RFCTYPE_STRING, RFC_IMPORT, 0, 0, true);
    AddParam(desc, "IV_PARQUET_OUT",   RFCTYPE_STRING, RFC_IMPORT, 0, 0, true);
    AddParam(desc, "IV_INIT_SQL",      RFCTYPE_STRING,  RFC_IMPORT, 0, 0, true);
    AddParam(desc, "IV_DDL",           RFCTYPE_STRING,  RFC_IMPORT, 0, 0, true);
    AddParam(desc, "IV_DATA",          RFCTYPE_STRING,  RFC_IMPORT, 0, 0, true);
    AddParam(desc, "IV_XDATA",         RFCTYPE_XSTRING, RFC_IMPORT, 0, 0, true);
    AddParam(desc, "IV_OP_COL",        RFCTYPE_STRING,  RFC_IMPORT, 0, 0, true);
    AddParam(desc, "EV_ROWS_AFFECTED", RFCTYPE_STRING,  RFC_EXPORT);
    AddParam(desc, "EV_ERROR",         RFCTYPE_STRING, RFC_EXPORT);
    return desc;
}

RFC_FUNCTION_DESC_HANDLE BuildSnapshotMergeDesc() {
    static RFC_FUNCTION_DESC_HANDLE desc = nullptr;
    if (desc) return desc;
    RFC_ERROR_INFO info;
    auto uname = std2uc("Z_DUCKDB_SNAPSHOT_MERGE");
    desc = RfcCreateFunctionDesc(uname.data(), &info);
    if (!desc) throw_rfc("RfcCreateFunctionDesc(Z_DUCKDB_SNAPSHOT_MERGE)", info);
    AddParam(desc, "IV_TARGET",  RFCTYPE_STRING, RFC_IMPORT, 0, 0, true);
    AddParam(desc, "IV_STAGING", RFCTYPE_STRING, RFC_IMPORT, 0, 0, true);
    AddParam(desc, "IV_KEYS",    RFCTYPE_STRING, RFC_IMPORT, 0, 0, true);
    AddParam(desc, "EV_INS",     RFCTYPE_STRING, RFC_EXPORT);
    AddParam(desc, "EV_UPD",     RFCTYPE_STRING, RFC_EXPORT);
    AddParam(desc, "EV_DEL",     RFCTYPE_STRING, RFC_EXPORT);
    AddParam(desc, "EV_ERROR",   RFCTYPE_STRING, RFC_EXPORT);
    return desc;
}

RFC_FUNCTION_DESC_HANDLE BuildOpenDesc() {
    static RFC_FUNCTION_DESC_HANDLE desc = nullptr;
    if (desc) return desc;
    RFC_ERROR_INFO info;
    auto uname = std2uc("Z_DUCKDB_OPEN");
    desc = RfcCreateFunctionDesc(uname.data(), &info);
    if (!desc) throw_rfc("RfcCreateFunctionDesc(Z_DUCKDB_OPEN)", info);
    AddParam(desc, "IV_SQL",     RFCTYPE_STRING, RFC_IMPORT, 0, 0, true);
    AddParam(desc, "EV_HANDLE",  RFCTYPE_STRING, RFC_EXPORT);
    AddParam(desc, "EV_COLUMNS", RFCTYPE_STRING, RFC_EXPORT);
    AddParam(desc, "EV_ERROR",   RFCTYPE_STRING, RFC_EXPORT);
    return desc;
}

RFC_FUNCTION_DESC_HANDLE BuildFetchDesc() {
    static RFC_FUNCTION_DESC_HANDLE desc = nullptr;
    if (desc) return desc;
    RFC_ERROR_INFO info;
    auto uname = std2uc("Z_DUCKDB_FETCH");
    desc = RfcCreateFunctionDesc(uname.data(), &info);
    if (!desc) throw_rfc("RfcCreateFunctionDesc(Z_DUCKDB_FETCH)", info);
    AddParam(desc, "IV_HANDLE",    RFCTYPE_STRING,  RFC_IMPORT, 0, 0, true);
    AddParam(desc, "IV_PAGE_ROWS", RFCTYPE_STRING,  RFC_IMPORT, 0, 0, true);
    AddParam(desc, "EV_XDATA",     RFCTYPE_XSTRING, RFC_EXPORT);
    AddParam(desc, "EV_FETCHED",   RFCTYPE_STRING,  RFC_EXPORT);
    AddParam(desc, "EV_DONE",      RFCTYPE_STRING,  RFC_EXPORT);
    AddParam(desc, "EV_ERROR",     RFCTYPE_STRING,  RFC_EXPORT);
    return desc;
}

RFC_FUNCTION_DESC_HANDLE BuildCloseDesc() {
    static RFC_FUNCTION_DESC_HANDLE desc = nullptr;
    if (desc) return desc;
    RFC_ERROR_INFO info;
    auto uname = std2uc("Z_DUCKDB_CLOSE");
    desc = RfcCreateFunctionDesc(uname.data(), &info);
    if (!desc) throw_rfc("RfcCreateFunctionDesc(Z_DUCKDB_CLOSE)", info);
    AddParam(desc, "IV_HANDLE", RFCTYPE_STRING, RFC_IMPORT, 0, 0, true);
    AddParam(desc, "EV_ERROR",  RFCTYPE_STRING, RFC_EXPORT);
    return desc;
}

} // namespace erpl_rev
