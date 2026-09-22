#include "ape_envelope.hpp"

#include <cctype>
#include <map>

namespace erpl_rev {
namespace ape {

namespace {

// Strict minimal JSON: objects, arrays, strings (full escapes), integers,
// true/false/null. Anything else -- trailing garbage, comments, floats,
// duplicate keys -- throws. Only what the engine emits, nothing more.
struct Json {
    enum class Type { Null, Bool, Int, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    long long integer = 0;
    std::string str;
    std::vector<Json> items;
    std::map<std::string, Json> props;
};

struct Reader {
    const char *p;
    explicit Reader(const char *s) : p(s) {}

    [[noreturn]] void Fail(const std::string &what) {
        throw ApeDecodeError("APE envelope: invalid JSON (" + what + ")");
    }

    void Space() {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    }

    char Peek() {
        if (*p == '\0') Fail("unexpected end");
        return *p;
    }

    void Expect(char c) {
        if (Peek() != c) Fail(std::string("expected '") + c + "'");
        ++p;
    }

    bool Take(const char *word) {
        for (const char *w = word; *w; ++w, ++p)
            if (*p != *w) Fail("bad literal");
        return true;
    }

    unsigned Hex4() {
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = Peek();
            v *= 16;
            if (c >= '0' && c <= '9') v += c - '0';
            else if (c >= 'a' && c <= 'f') v += c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v += c - 'A' + 10;
            else Fail("bad \\u escape");
            ++p;
        }
        return v;
    }

    // UTF-16 value -> UTF-8 bytes (BMP only; the engine emits text, and
    // anything outside BMP fails rather than decodes wrong).
    static void PutUtf8(std::string &out, unsigned v) {
        if (v < 0x80) {
            out += static_cast<char>(v);
        } else if (v < 0x800) {
            out += static_cast<char>(0xc0 | (v >> 6));
            out += static_cast<char>(0x80 | (v & 0x3f));
        } else if (v < 0xd800 || (v >= 0xe000 && v <= 0xffff)) {
            out += static_cast<char>(0xe0 | (v >> 12));
            out += static_cast<char>(0x80 | ((v >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (v & 0x3f));
        } else {
            throw ApeDecodeError("APE envelope: no surrogate pairs supported");
        }
    }

    std::string ParseString() {
        Expect('"');
        std::string out;
        for (;;) {
            const char c = Peek();
            if (c == '"') {
                ++p;
                return out;
            }
            if (c == '\\') {
                ++p;
                switch (Peek()) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': ++p; PutUtf8(out, Hex4()); continue;
                    default: Fail("bad escape");
                }
                ++p;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                Fail("control character in string");
            } else {
                out += c;
                ++p;
            }
        }
    }

    long long ParseInt() {
        bool neg = false;
        if (Peek() == '-') {
            neg = true;
            ++p;
        }
        if (!std::isdigit(static_cast<unsigned char>(Peek()))) Fail("bad number");
        long long v = 0;
        while (std::isdigit(static_cast<unsigned char>(*p))) v = v * 10 + (*p++ - '0');
        return neg ? -v : v;
    }

    Json ParseValue() {
        Space();
        Json j;
        switch (Peek()) {
            case '{': {
                ++p;
                j.type = Json::Type::Object;
                Space();
                if (Peek() == '}') {
                    ++p;
                    return j;
                }
                for (;;) {
                    Space();
                    if (Peek() != '"') Fail("object key must be a string");
                    std::string key = ParseString();
                    Space();
                    Expect(':');
                    if (!j.props.emplace(key, ParseValue()).second)
                        Fail("duplicate key '" + key + "'");
                    Space();
                    if (Peek() == ',') {
                        ++p;
                        continue;
                    }
                    Expect('}');
                    return j;
                }
            }
            case '[': {
                ++p;
                j.type = Json::Type::Array;
                Space();
                if (Peek() == ']') {
                    ++p;
                    return j;
                }
                for (;;) {
                    j.items.push_back(ParseValue());
                    Space();
                    if (Peek() == ',') {
                        ++p;
                        continue;
                    }
                    Expect(']');
                    return j;
                }
            }
            case '"':
                j.type = Json::Type::String;
                j.str = ParseString();
                return j;
            case 't':
                Take("true");
                j.type = Json::Type::Bool;
                j.boolean = true;
                return j;
            case 'f':
                Take("false");
                j.type = Json::Type::Bool;
                return j;
            case 'n':
                Take("null");
                return j;
            default:
                j.type = Json::Type::Int;
                j.integer = ParseInt();
                return j;
        }
    }

    const Json *Find(const Json &obj, const std::string &key) {
        if (obj.type != Json::Type::Object) return nullptr;
        const auto it = obj.props.find(key);
        return it == obj.props.end() ? nullptr : &it->second;
    }

    static std::string UpperAscii(const std::string &s) {
        std::string r = s;
        for (char &c : r)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return r;
    }
};

}  // namespace

namespace {

// The first bytes of the offending envelope, so a shape refusal names what
// the engine actually sent instead of what the reader wanted.
std::string Head(const char *port_data) {
    std::string h(port_data == nullptr ? "" : port_data);
    if (h.size() > 200) h.resize(200);
    return h;
}

std::string TypeName(Json::Type t) {
    switch (t) {
        case Json::Type::Null: return "null";
        case Json::Type::Bool: return "bool";
        case Json::Type::Int: return "int";
        case Json::Type::String: return "string";
        case Json::Type::Array: return "array";
        case Json::Type::Object: return "object";
    }
    return "?";
}

}  // namespace

ApeEnvelope ExtractEnvelope(const char *port_data) {
    ApeEnvelope e;
    if (port_data == nullptr || *port_data == '\0') return e;

    Reader r(port_data);
    const Json top = r.ParseValue();
    r.Space();
    if (*r.p != '\0') r.Fail("trailing data after envelope");
    if (top.type != Json::Type::Object)
        throw ApeDecodeError("APE envelope: top level must be an object");

    const Json *enc = r.Find(top, "Encoding");
    if (enc == nullptr || enc->type != Json::Type::String || enc->str != "csv")
        throw ApeDecodeError("APE envelope: only Encoding csv is supported");

    const Json *attrs = r.Find(top, "Attributes");
    const Json *batch = attrs != nullptr ? r.Find(*attrs, "message.batchIndex") : nullptr;
    if (batch == nullptr || batch->type != Json::Type::Int)
        throw ApeDecodeError("APE envelope: message.batchIndex is required");
    e.batch_index = batch->integer;
    const Json *last = attrs != nullptr ? r.Find(*attrs, "message.lastBatch") : nullptr;
    if (last != nullptr) {
        if (last->type != Json::Type::Bool)
            throw ApeDecodeError("APE envelope: message.lastBatch must be boolean");
        e.last_batch = last->boolean;
    }

    const Json *abap = attrs != nullptr ? r.Find(*attrs, "ABAP") : nullptr;
    // Only Kind = "Table" carries stageable rows. A scalar/status descriptor
    // (live: Kind = "Element" on the lastBatch terminator, still carrying a
    // Body) skips after the flags above (HLD T-3) -- mapping it by name would
    // be decoding a summary as rows.
    const Json *kind = abap != nullptr ? r.Find(*abap, "Kind") : nullptr;
    if (kind != nullptr && kind->type == Json::Type::String && kind->str != "Table") {
        e.is_control = true;
        return e;
    }
    const Json *fields = abap != nullptr ? r.Find(*abap, "Fields") : nullptr;
    const Json *body = r.Find(top, "Body");

    const bool has_fields = fields != nullptr;
    const bool has_body = body != nullptr;
    if (!has_fields && !has_body) {
        e.is_control = true;  // skip, but the flags above still count
        return e;
    }
    if (has_body && !has_fields)
        throw ApeDecodeError("APE envelope: Body without Fields cannot map by name (ABAP node " +
                             (abap == nullptr ? "absent" : TypeName(abap->type)) +
                             "): " + Head(port_data));
    if (has_fields && !has_body)
        throw ApeDecodeError("APE envelope: Fields without Body carry no rows");
    if (fields->type != Json::Type::Array)
        throw ApeDecodeError("APE envelope: ABAP.Fields must be an array");
    if (body->type != Json::Type::String)
        throw ApeDecodeError("APE envelope: Body must be a string");

    // Key flags live in the sibling metadata list, matched by column name.
    std::map<std::string, bool> keys;
    const Json *meta = r.Find(*attrs, "metadata");
    if (meta != nullptr) {
        if (meta->type != Json::Type::Array)
            throw ApeDecodeError("APE envelope: metadata must be an array");
        for (const Json &m : meta->items) {
            const Json *f = r.Find(m, "Field");
            const Json *cn = f != nullptr ? r.Find(*f, "ColumnName") : nullptr;
            const Json *kf = f != nullptr ? r.Find(*f, "IsKeyField") : nullptr;
            if (cn != nullptr && cn->type == Json::Type::String)
                keys[Reader::UpperAscii(cn->str)] =
                    kf != nullptr && kf->type == Json::Type::String && kf->str == "X";
        }
    }

    for (const Json &f : fields->items) {
        if (f.type != Json::Type::Object)
            throw ApeDecodeError("APE envelope: field entries must be objects");
        const Json *name = r.Find(f, "Name");
        const Json *type = r.Find(f, "Type");
        const Json *kind = r.Find(f, "Kind");
        const Json *len = r.Find(f, "Length");
        const Json *dec = r.Find(f, "Decimals");
        if (name == nullptr || name->type != Json::Type::String || name->str.empty() ||
            type == nullptr || type->type != Json::Type::String ||
            kind == nullptr || kind->type != Json::Type::String ||
            kind->str.size() != 1 ||
            len == nullptr || len->type != Json::Type::Int ||
            dec == nullptr || dec->type != Json::Type::Int)
            throw ApeDecodeError("APE envelope: field needs Name/Type/Kind/Length/Decimals");
        ApeField af;
        af.name = name->str;
        af.type = type->str;
        af.kind = kind->str[0];
        af.length = static_cast<int>(len->integer);
        af.decimals = static_cast<int>(dec->integer);
        const auto it = keys.find(Reader::UpperAscii(af.name));
        af.is_key = it != keys.end() && it->second;
        e.fields.push_back(af);
    }

    e.has_data = true;
    e.body = body->str;
    return e;
}

}  // namespace ape
}  // namespace erpl_rev
