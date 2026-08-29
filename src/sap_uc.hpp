// SAP_UC <-> UTF-8 helpers and small RFC field accessors.
//
// Self-contained (depends only on the NW RFC SDK), so the Step-1 ping skeleton
// links without DuckDB. Richer duckdb::Value <-> RFC marshalling (later steps)
// can reuse erpl's sap_type_conversion.
#pragma once

#include <string>
#include <vector>
#include "sapnwrfc.h"

namespace erpl_rev {

// SAP_UC (UTF-16) string primitives.
//
// The SDK supplies these as `strlenU` / `strncpyU`, but those resolve to
// `strlenU16` / `strncpyU16` in **libsapucum** — a second SAP shared object,
// separate from libsapnwrfc, and one a pure-Rust RFC backend does not replace
// (it implements the RFC ABI, not SAP's string library). Two functions are not
// worth a dependency that would outlive the SDK removal, so they are spelled
// out here. Backend-neutral: nothing about them is SAP-specific beyond the type.
size_t uclen(const SAP_UC *s);
// Copies at most `cap` units *including* the terminator, always terminating.
void   uccpy(SAP_UC *dst, const SAP_UC *src, size_t cap);


// Decode a SAP_UC buffer to a UTF-8 std::string.
std::string uc2std(const SAP_UC *uc);                       // null-terminated
std::string uc2std(const SAP_UC *uc, unsigned len);

// Encode a UTF-8 std::string to a null-terminated SAP_UC buffer.
std::vector<SAP_UC> std2uc(const std::string &s);

// Render an RFC_ERROR_INFO as a readable one-liner (code/group/key/message).
std::string error2std(const RFC_ERROR_INFO &e);

// Throw std::runtime_error(context + ": " + error2std(e)) on RFC failures.
[[noreturn]] void throw_rfc(const std::string &context, const RFC_ERROR_INFO &e);

// Convenience field accessors on any DATA_CONTAINER_HANDLE (function/struct/row).
// GetString reads RFCTYPE_STRING fields; GetChars reads fixed RFCTYPE_CHAR
// fields of the given capacity (trailing blanks trimmed).
std::string GetString(DATA_CONTAINER_HANDLE h, const char *name);
std::string GetChars (DATA_CONTAINER_HANDLE h, const char *name, unsigned len);
// XSTRING (raw bytes) accessors — used for the binary BXML payload.
std::string GetXString(DATA_CONTAINER_HANDLE h, const char *name);
void        SetXString(DATA_CONTAINER_HANDLE h, const char *name, const std::string &v);
void        SetString(DATA_CONTAINER_HANDLE h, const char *name, const std::string &v);
void        SetChars (DATA_CONTAINER_HANDLE h, const char *name, const std::string &v);
void        SetInt   (DATA_CONTAINER_HANDLE h, const char *name, RFC_INT v);

} // namespace erpl_rev
