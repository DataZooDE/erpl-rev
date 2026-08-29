#include "sap_uc.hpp"

#include <stdexcept>
#include <cstring>

namespace erpl_rev {

size_t uclen(const SAP_UC *s) {
    if (s == nullptr) return 0;
    size_t n = 0;
    while (s[n] != 0) n++;
    return n;
}

void uccpy(SAP_UC *dst, const SAP_UC *src, size_t cap) {
    if (dst == nullptr || cap == 0) return;
    size_t n = 0;
    // cap counts the terminator, so copy at most cap-1 units and terminate.
    // strncpyU does not guarantee termination on truncation; this does, because
    // every caller here hands the result to an API that reads to NUL.
    while (n + 1 < cap && src != nullptr && src[n] != 0) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}


std::string uc2std(const SAP_UC *uc, unsigned len) {
    if (uc == nullptr || len == 0) return {};
    RFC_ERROR_INFO info;
    std::vector<char> buf(static_cast<size_t>(len) * 4 + 1, '\0');
    unsigned buf_size = static_cast<unsigned>(buf.size());
    unsigned result_len = 0;
    RFC_RC rc = RfcSAPUCToUTF8(uc, len, reinterpret_cast<RFC_BYTE *>(buf.data()),
                               &buf_size, &result_len, &info);
    if (rc != RFC_OK) throw_rfc("RfcSAPUCToUTF8", info);
    return std::string(buf.data(), result_len);
}

std::string uc2std(const SAP_UC *uc) {
    if (uc == nullptr) return {};
    return uc2std(uc, static_cast<unsigned>(uclen(uc)));
}

std::vector<SAP_UC> std2uc(const std::string &s) {
    RFC_ERROR_INFO info;
    std::vector<SAP_UC> buf(s.size() + 1, 0);
    unsigned buf_size = static_cast<unsigned>(buf.size());
    unsigned result_len = 0;
    RFC_RC rc = RfcUTF8ToSAPUC(reinterpret_cast<const RFC_BYTE *>(s.data()),
                               static_cast<unsigned>(s.size()),
                               buf.data(), &buf_size, &result_len, &info);
    if (rc != RFC_OK) throw_rfc("RfcUTF8ToSAPUC", info);
    buf[result_len] = 0;
    buf.resize(result_len + 1);
    return buf;
}

std::string error2std(const RFC_ERROR_INFO &e) {
    std::string msg = "code=" + std::to_string(static_cast<int>(e.code)) +
                      " group=" + std::to_string(static_cast<int>(e.group));
    if (uclen(e.key) > 0)     msg += " key=" + uc2std(e.key);
    if (uclen(e.message) > 0) msg += " message=" + uc2std(e.message);
    return msg;
}

void throw_rfc(const std::string &context, const RFC_ERROR_INFO &e) {
    throw std::runtime_error(context + ": " + error2std(e));
}

std::string GetString(DATA_CONTAINER_HANDLE h, const char *name) {
    RFC_ERROR_INFO info;
    auto uname = std2uc(name);
    // Query the exact length first, then fetch into an exact-sized buffer.
    unsigned str_len = 0;
    RFC_RC rc = RfcGetStringLength(h, uname.data(), &str_len, &info);
    if (rc != RFC_OK) throw_rfc(std::string("RfcGetStringLength(") + name + ")", info);
    std::vector<SAP_UC> buf(str_len + 1, 0);
    unsigned result_len = 0;
    rc = RfcGetString(h, uname.data(), buf.data(),
                      static_cast<unsigned>(buf.size()), &result_len, &info);
    if (rc != RFC_OK) throw_rfc(std::string("RfcGetString(") + name + ")", info);
    return uc2std(buf.data(), result_len);
}

std::string GetChars(DATA_CONTAINER_HANDLE h, const char *name, unsigned len) {
    RFC_ERROR_INFO info;
    auto uname = std2uc(name);
    std::vector<SAP_UC> buf(len + 1, 0);
    RFC_RC rc = RfcGetChars(h, uname.data(), buf.data(), len, &info);
    if (rc != RFC_OK) throw_rfc(std::string("RfcGetChars(") + name + ")", info);
    std::string s = uc2std(buf.data(), len);
    // CHAR fields are blank-padded; trim trailing spaces.
    auto end = s.find_last_not_of(' ');
    return end == std::string::npos ? std::string() : s.substr(0, end + 1);
}

std::string GetXString(DATA_CONTAINER_HANDLE h, const char *name) {
    RFC_ERROR_INFO info;
    auto uname = std2uc(name);
    // RfcGetStringLength returns the byte length for XSTRING fields too.
    unsigned blen = 0;
    RFC_RC rc = RfcGetStringLength(h, uname.data(), &blen, &info);
    if (rc != RFC_OK) throw_rfc(std::string("RfcGetStringLength(") + name + ")", info);
    std::string out;
    out.resize(blen);
    unsigned got = 0;
    rc = RfcGetXString(h, uname.data(), reinterpret_cast<SAP_RAW *>(&out[0]),
                       blen, &got, &info);
    if (rc != RFC_OK) throw_rfc(std::string("RfcGetXString(") + name + ")", info);
    out.resize(got);
    return out;
}

void SetXString(DATA_CONTAINER_HANDLE h, const char *name, const std::string &v) {
    RFC_ERROR_INFO info;
    auto uname = std2uc(name);
    RFC_RC rc = RfcSetXString(h, uname.data(),
                              reinterpret_cast<const SAP_RAW *>(v.data()),
                              static_cast<unsigned>(v.size()), &info);
    if (rc != RFC_OK) throw_rfc(std::string("RfcSetXString(") + name + ")", info);
}

void SetString(DATA_CONTAINER_HANDLE h, const char *name, const std::string &v) {
    RFC_ERROR_INFO info;
    auto uname = std2uc(name);
    auto uval = std2uc(v);
    RFC_RC rc = RfcSetString(h, uname.data(), uval.data(),
                             static_cast<unsigned>(uval.size() - 1), &info);
    if (rc != RFC_OK) throw_rfc(std::string("RfcSetString(") + name + ")", info);
}

void SetChars(DATA_CONTAINER_HANDLE h, const char *name, const std::string &v) {
    RFC_ERROR_INFO info;
    auto uname = std2uc(name);
    auto uval = std2uc(v);
    RFC_RC rc = RfcSetChars(h, uname.data(), uval.data(),
                            static_cast<unsigned>(uval.size() - 1), &info);
    if (rc != RFC_OK) throw_rfc(std::string("RfcSetChars(") + name + ")", info);
}

void SetInt(DATA_CONTAINER_HANDLE h, const char *name, RFC_INT v) {
    RFC_ERROR_INFO info;
    auto uname = std2uc(name);
    RFC_RC rc = RfcSetInt(h, uname.data(), v, &info);
    if (rc != RFC_OK) throw_rfc(std::string("RfcSetInt(") + name + ")", info);
}

} // namespace erpl_rev
