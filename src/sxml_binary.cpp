#include "sxml_binary.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace erpl_rev {
namespace sxml {

namespace {

// Token bytes (same family as erpl/odp/src/odp_xml.cpp BinaryFlags).
constexpr unsigned char PI      = 0x3F;
constexpr unsigned char ENCODE  = 0x2B;  // EncodeItem: intern a name
constexpr unsigned char TAG     = 0x3C;  // TagStart: open element by id
constexpr unsigned char TAG_END = 0x3E;  // close element
constexpr unsigned char ATTR_C  = 0x3A;  // attribute (xmlns:)
constexpr unsigned char ATTR_AT = 0x40;  // attribute (name="val")
constexpr unsigned char TEXT    = 0x54;
constexpr unsigned char BINARY  = 0x42;

// The asXML/BXML preamble the ABAP kernel emits for
// `CALL TRANSFORMATION id SOURCE data = <itab>` — byte-identical across all
// captured fixtures, up to and including the opening of <DATA> (id 7). After
// this, element ids continue: item=8, columns=9,10,... (see test fixtures).
const char *kPrefixHex =
    "42584D4C"                                  // "BXML"
    "3F0356455203302E37"                        // <?VER 0.7?>
    "3F03454E43057574662D38"                    // <?ENC utf-8?>
    "2B036173782B1A687474703A2F2F7777772E7361702E636F6D2F61626170786D6C3A0203" // asx ns
    "2B04616261703C0402"                        // <abap> (id4)
    "2B0776657273696F6E4005014103312E30"        //   version="1.0"
    "2B0676616C7565733C0602"                    // <asx:values> (id6)
    "2B04444154413C0701";                       // <DATA> (id7)

std::string Unhex(const char *h) {
    auto nib = [](char c) {
        return (c >= '0' && c <= '9') ? c - '0'
             : (c >= 'A' && c <= 'F') ? c - 'A' + 10
             : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
    };
    std::string out;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2)
        out.push_back(static_cast<char>((nib(h[i]) << 4) | nib(h[i + 1])));
    return out;
}

// Append a 6-bit varint id (1 byte if < 128, else 2 bytes — matches erpl's
// Parse6BitEncodedInt: result = ((b1&63)<<6) | (b2&63), b1|=0xC0, b2|=0x80).
void PutId(std::string &out, int id) {
    if (id < 128) {
        out.push_back(static_cast<char>(id));
    } else {
        out.push_back(static_cast<char>(0xC0 | ((id >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (id & 0x3F)));
    }
}

void PutName(std::string &out, const std::string &name) {
    out.push_back(static_cast<char>(ENCODE));
    out.push_back(static_cast<char>(name.size() & 0xFF));
    out += name;
}

// A chunk byte-length, written as a single Unicode code point in UTF-8 — the
// exact form the ABAP kernel uses (inverse of Reader::readLen).
void PutLen(std::string &out, size_t n) {
    auto cont = [&](int shift) { out.push_back(static_cast<char>(0x80 | ((n >> shift) & 0x3F))); };
    if (n < 0x80) {
        out.push_back(static_cast<char>(n));
    } else if (n < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (n >> 6)));      cont(0);
    } else if (n < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (n >> 12)));     cont(6); cont(0);
    } else if (n < 0x200000) {                                  // 4-byte: up to 0x1FFFFF
        out.push_back(static_cast<char>(0xF0 | (n >> 18)));     cont(12); cont(6); cont(0);
    } else if (n < 0x4000000) {                                 // 5-byte: up to 0x3FFFFFF (FSS-UTF)
        out.push_back(static_cast<char>(0xF8 | (n >> 24)));     cont(18); cont(12); cont(6); cont(0);
    } else {                                                    // 6-byte: up to 0x7FFFFFFF
        out.push_back(static_cast<char>(0xFC | (n >> 30)));     cont(24); cont(18); cont(12); cont(6); cont(0);
    }
}

// Text content: one or more `0x54<utf8-byte-length><bytes>` chunks, capped at
// 1024 bytes/chunk to mirror the kernel (which caps at 1024 chars — identical
// for ASCII). The value ends at the next non-0x54 token; no terminator is
// emitted. Chunk boundaries never split a UTF-8 multi-byte char (so each chunk
// is independently valid UTF-8). Empty values emit NO text token at all.
constexpr size_t kChunk = 1024;
void PutTextBytes(std::string &out, const char *val, size_t size) {
    if (size == 0) return;
    size_t off = 0;
    while (off < size) {
        size_t seg = std::min<size_t>(kChunk, size - off);
        // back the boundary off any trailing UTF-8 continuation bytes so the
        // chunk ends on a whole character (still byte-exact vs kernel for ASCII).
        while (seg < size - off &&
               (static_cast<unsigned char>(val[off + seg]) & 0xC0) == 0x80)
            seg++;
        out.push_back(static_cast<char>(TEXT));
        PutLen(out, seg);
        out.append(val + off, seg);
        off += seg;
    }
}
inline void PutText(std::string &out, const std::string &val) {
    PutTextBytes(out, val.data(), val.size());
}

struct Reader {
    const std::string &s;
    size_t i = 0;
    explicit Reader(const std::string &str) : s(str) {}
    bool eof() const { return i >= s.size(); }
    unsigned char peek() const { return i < s.size() ? (unsigned char)s[i] : 0; }
    unsigned char get() {
        if (i >= s.size()) throw std::runtime_error("sXML: unexpected end of input");
        return (unsigned char)s[i++];
    }
    std::string read(size_t n) {
        if (i + n > s.size()) throw std::runtime_error("sXML: truncated content");
        std::string r = s.substr(i, n);
        i += n;
        return r;
    }
    int readId() {  // inverse of PutId
        unsigned char b = get();
        if (b < 128) return b;
        unsigned char b2 = get();
        return ((b & 0x3F) << 6) | (b2 & 0x3F);
    }
    // A hex window around the current position, for diagnostics.
    std::string context() const {
        size_t from = i > 24 ? i - 24 : 0;
        size_t to = std::min(s.size(), i + 24);
        static const char *d = "0123456789ABCDEF";
        std::string hex;
        for (size_t k = from; k < to; k++) {
            if (k == i) hex += "[";
            unsigned char c = (unsigned char)s[k];
            hex.push_back(d[c >> 4]); hex.push_back(d[c & 0xF]);
            if (k == i) hex += "]";
        }
        return "off=" + std::to_string(i) + "/" + std::to_string(s.size()) + " ctx=" + hex;
    }
    // A chunk length is the byte count of the chunk, written by the kernel as a
    // single Unicode code point in UTF-8 (1-4 bytes). Inverse of PutLen.
    size_t readLen() {
        unsigned char c = get();
        if (c < 0x80) return c;
        // The kernel encodes the length as a code point in the ORIGINAL (FSS-UTF)
        // UTF-8, which extends to 5- and 6-byte forms (up to 2^31) — a 2 MB+ value
        // (length > 0x1FFFFF, the 4-byte ceiling) arrives with a 0xF8 lead.
        size_t extra, cp;
        if      ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else if ((c & 0xFC) == 0xF8) { extra = 4; cp = c & 0x03; }
        else if ((c & 0xFE) == 0xFC) { extra = 5; cp = c & 0x01; }
        else throw std::runtime_error("sXML: bad chunk-length lead byte 0x" +
                                       std::string(1, "0123456789ABCDEF"[c >> 4]) +
                                       std::string(1, "0123456789ABCDEF"[c & 0xF]) +
                                       " " + context());
        for (size_t k = 0; k < extra; k++) {
            unsigned char cc = get();
            if ((cc & 0xC0) != 0x80) throw std::runtime_error("sXML: bad chunk-length continuation " + context());
            cp = (cp << 6) | (cc & 0x3F);
        }
        return cp;
    }
};

struct Elem {
    std::string name;
    std::string text;
    bool has_text = false;
};

} // namespace

std::string Encode(const std::string &node, const Table &table) {
    // `node` is fixed to DATA by the kernel preamble; honor the contract but the
    // captured preamble hard-codes "DATA" (callers pass "DATA").
    (void)node;
    std::string out = Unhex(kPrefixHex);

    for (size_t r = 0; r < table.rows.size(); r++) {
        if (r == 0) PutName(out, "item");
        out.push_back(static_cast<char>(TAG));
        PutId(out, 8);                 // item id = 8
        out.push_back(0x01);           // flag

        const auto &row = table.rows[r];
        for (size_t c = 0; c < table.columns.size(); c++) {
            if (r == 0) PutName(out, table.columns[c]);
            out.push_back(static_cast<char>(TAG));
            PutId(out, 9 + static_cast<int>(c));   // column ids start at 9
            out.push_back(0x01);                   // flag
            if (c < row.size()) PutText(out, row[c]);
            out.push_back(static_cast<char>(TAG_END)); // close column
        }
        out.push_back(static_cast<char>(TAG_END));     // close item
    }

    out.push_back(static_cast<char>(TAG_END));  // close DATA
    out.push_back(static_cast<char>(TAG_END));  // close asx:values
    out.push_back(static_cast<char>(TAG_END));  // close asx:abap
    return out;
}

// StreamEncoder — same token order as Encode(), fed row-by-row. The name
// interning (EncodeItem for "item" and each column) happens on the first row
// only; thereafter just TAG/text/TAG_END, exactly as the kernel emits.
StreamEncoder::StreamEncoder(std::vector<std::string> columns)
    : cols_(std::move(columns)), out_(Unhex(kPrefixHex)) {}

void StreamEncoder::StartRow() {
    in_first_ = (rows_ == 0);
    if (in_first_) PutName(out_, "item");
    out_.push_back(static_cast<char>(TAG));
    PutId(out_, 8);              // item id = 8
    out_.push_back(0x01);        // flag
}

void StreamEncoder::Cell(size_t col, const char *bytes, size_t len) {
    if (in_first_) PutName(out_, cols_[col]);
    out_.push_back(static_cast<char>(TAG));
    PutId(out_, 9 + static_cast<int>(col));   // column ids start at 9
    out_.push_back(0x01);                     // flag
    if (len) PutTextBytes(out_, bytes, len);
    out_.push_back(static_cast<char>(TAG_END));
}

void StreamEncoder::EndRow() {
    out_.push_back(static_cast<char>(TAG_END));  // close item
    rows_++;
    in_first_ = false;
}

std::string StreamEncoder::Finish() {
    out_.push_back(static_cast<char>(TAG_END));  // close DATA
    out_.push_back(static_cast<char>(TAG_END));  // close asx:values
    out_.push_back(static_cast<char>(TAG_END));  // close asx:abap
    return std::move(out_);
}

Table Decode(const std::string &bytes) {
    Reader r(bytes);
    if (r.read(4) != "BXML") throw std::runtime_error("sXML: missing BXML magic");

    std::unordered_map<int, std::string> id_name;
    std::vector<Elem> stack;
    Table out;
    bool columns_locked = false;
    std::vector<std::string> cur_cols;   // names seen in the current <item>
    std::vector<std::string> cur_vals;

    // A name interned by EncodeItem that becomes an element only if immediately
    // followed by a TagStart (otherwise it's a namespace prefix / attribute name).
    std::string pending;
    bool have_pending = false;

    auto open_elem = [&](const std::string &name) {
        stack.push_back({name, "", false});
        size_t depth = stack.size();
        if (depth == 4) {                 // <item> — a new row
            cur_cols.clear();
            cur_vals.clear();
        }
    };

    while (!r.eof()) {
        unsigned char b = r.peek();
        switch (b) {
            case PI: {                    // <?name value?>
                r.get();
                int n1 = r.get(); r.read(n1);
                int n2 = r.get(); r.read(n2);
                break;
            }
            case ENCODE: {                // intern a name
                r.get();
                int len = r.get();
                pending = r.read(len);
                have_pending = true;
                break;
            }
            case TAG: {                   // open element by id
                r.get();
                int id = r.readId();
                r.get();                  // flag byte (unused)
                std::string name;
                if (have_pending) { name = pending; id_name[id] = name; have_pending = false; }
                else              { name = id_name.count(id) ? id_name[id] : std::string(); }
                open_elem(name);
                break;
            }
            case ATTR_C: {                // xmlns: attribute
                r.get(); r.get(); r.get();          // <id1><byte>
                have_pending = false;               // the prior EncodeItem was a prefix
                break;
            }
            case ATTR_AT: {               // name="value" attribute
                r.get();
                r.get();                  // attr id
                r.get();                  // '1'
                r.get();                  // attr type
                int len = r.get();
                r.read(len);
                have_pending = false;     // the prior EncodeItem was an attr name
                break;
            }
            case TEXT:                    // text content
            case BINARY: {                // binary content (xstring columns)
                // A value is one or more chunks of the SAME token, each
                // `<tok><utf8-byte-length><bytes>` (the kernel caps text chunks
                // at 1024 chars; binary larger). The value ends at the first
                // byte that is not another chunk of this token — there is no
                // peek-terminator, so a chunk-length byte of 0x3E is read as a
                // length, never mistaken for TagEnd (the old REPOSRC bug).
                unsigned char tok = b;
                std::string val;
                while (!r.eof() && r.peek() == tok) {
                    r.get();                       // consume the chunk token
                    val += r.read(r.readLen());    // length is a byte count
                }
                if (!stack.empty()) { stack.back().text = val; stack.back().has_text = true; }
                break;
            }
            case TAG_END: {               // close element
                r.get();
                if (stack.empty()) break;
                size_t depth = stack.size();
                Elem e = stack.back();
                stack.pop_back();
                if (depth == 5) {                       // column close
                    if (!columns_locked) cur_cols.push_back(e.name);
                    cur_vals.push_back(e.has_text ? e.text : std::string());
                } else if (depth == 4) {                // item close -> row
                    if (!columns_locked) { out.columns = cur_cols; columns_locked = true; }
                    out.rows.push_back(cur_vals);
                }
                break;
            }
            default:
                r.get();                  // tolerate unknown header bytes
                break;
        }
    }
    return out;
}

} // namespace sxml
} // namespace erpl_rev
