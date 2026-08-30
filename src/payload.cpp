#include "payload.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>

namespace erpl_rev {

namespace {

bool HasDeflateFrame(const std::string &s) {
    return s.size() >= 4 && s.compare(0, 4, kDeflateMagic) == 0;
}

bool HasGzipFrame(const std::string &s) {
    return s.size() >= 2 && static_cast<unsigned char>(s[0]) == 0x1F &&
           static_cast<unsigned char>(s[1]) == 0x8B;
}

} // namespace

bool IsCompressed(const std::string &s) {
    return HasDeflateFrame(s) || HasGzipFrame(s);
}

std::string MaybeInflate(const std::string &s, std::size_t max_output) {
    if (!IsCompressed(s)) return s;

    const bool raw = HasDeflateFrame(s);
    const std::size_t off = raw ? 4 : 0;   // skip our frame; gzip keeps its header

    z_stream zs{};
    // Negative window bits select a headerless raw DEFLATE stream (what
    // cl_abap_gzip emits); 15|16 selects gzip framing.
    if (inflateInit2(&zs, raw ? -15 : 15 + 16) != Z_OK)
        throw std::runtime_error("payload inflate failed: could not initialise zlib");

    // Free the stream on every exit path, including the throws below.
    struct Guard {
        z_stream *z;
        ~Guard() { inflateEnd(z); }
    } guard{&zs};

    std::string out;
    // Compression on this payload runs ~48x, so guessing high saves a dozen
    // reallocations on a real package -- but clamp it, or a hostile input that
    // merely *claims* to be compressed could force a huge allocation before a
    // single byte has been inflated and the cap below could apply.
    const std::size_t in_size = s.size() - off;
    out.reserve(std::min<std::size_t>({in_size * 8, max_output, 256u << 20}));

    // zlib's avail_in/avail_out are 32-bit, so a >4 GiB input must be fed in
    // chunks. Truncating the cast would silently inflate a prefix and drop the
    // rest -- data loss reported as success.
    std::size_t in_pos = off;
    std::vector<char> buf(1 << 20);
    int rc = Z_OK;
    do {
        if (zs.avail_in == 0 && in_pos < s.size()) {
            const std::size_t chunk =
                std::min<std::size_t>(s.size() - in_pos, std::numeric_limits<uInt>::max());
            zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(s.data() + in_pos));
            zs.avail_in = static_cast<uInt>(chunk);
            in_pos += chunk;
        }
        zs.next_out = reinterpret_cast<Bytef *>(buf.data());
        zs.avail_out = static_cast<uInt>(buf.size());
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            const char *why = zs.msg ? zs.msg : "corrupt or truncated stream";
            throw std::runtime_error(std::string("payload inflate failed: ") + why);
        }
        const std::size_t produced = buf.size() - zs.avail_out;
        // Subtraction, not addition: out.size() + produced could overflow.
        if (produced > max_output - out.size())
            throw std::runtime_error("payload inflate failed: inflated size exceeds the limit");
        out.append(buf.data(), produced);
        // Z_BUF_ERROR with no input left and nothing produced means the stream
        // ended mid-member: report it rather than returning a short payload.
        if (rc == Z_BUF_ERROR && produced == 0 && zs.avail_in == 0 && in_pos >= s.size())
            throw std::runtime_error("payload inflate failed: truncated stream");
    } while (rc != Z_STREAM_END);

    // Everything must have been consumed. Stopping at the first Z_STREAM_END and
    // returning would silently drop a second gzip member or any trailing bytes,
    // and the sXML decoder reads to EOF without checking that its element stack
    // closed -- so a short payload would land as missing rows, not as an error.
    if (zs.avail_in != 0 || in_pos != s.size())
        throw std::runtime_error(
            "payload inflate failed: trailing data after the compressed stream");

    return out;
}

} // namespace erpl_rev
