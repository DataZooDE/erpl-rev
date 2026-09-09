// The RFC function-module contract: names, parameters, types, directions.
//
// The same interface used to be written out in three places that had to agree
// but had nothing making them:
//
//   src/rfc_metadata.cpp        the descriptors the server registers
//   abap/zcl_erpl_rev_mkfm.abap the FMs generated inside SAP
//   src/sap_setup.cpp           the name list doctor verifies
//
// A mismatch between the first two is not a build error and not a startup
// error: it surfaces as a parameter-level RFC failure at call time, on a
// customer system, during an upgrade. So the contract lives here, once, and the
// other three read it.
//
// Deliberately free of the NW RFC SDK -- no RFCTYPE, no RFC_DIRECTION, no
// includes from the SDK at all. rfc_metadata.cpp translates these enums into
// SDK constants. That is what lets the unit-test binary, which links no SDK,
// compile and check the contract.
#pragma once

#include <string>
#include <vector>

namespace erpl_rev {

enum class RfcParamType { Char, String, XString };
enum class RfcDir { Import, Export };

struct RfcParam {
    const char *name;
    RfcParamType type;
    RfcDir dir;
    unsigned nuc_len;   // CHAR only
    unsigned uc_len;    // CHAR only
    bool optional;
};

struct RfcFm {
    const char *name;
    std::vector<RfcParam> params;
};

// Every FM the server registers, in registration order.
const std::vector<RfcFm> &RfcContract();

// The Z_DUCKDB_* subset, i.e. what zcl_erpl_rev_mkfm has to generate. Excludes
// STFC_CONNECTION, which SAP already ships.
std::vector<std::string> GeneratedFmNames();

const RfcFm *FindFm(const std::string &name);

// "STRING" / "XSTRING" / "CHAR" -- the spelling mkfm uses in RSIMP/RSEXP.
std::string AbapTypeName(RfcParamType t);

}  // namespace erpl_rev
