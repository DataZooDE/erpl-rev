#include "payload.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>

namespace erpl_rev {

bool IsGzip(const std::string &s) {
    return s.size() >= 2 && static_cast<unsigned char>(s[0]) == 0x1F &&
           static_cast<unsigned char>(s[1]) == 0x8B;
}

std::string MaybeInflate(const std::string &s, std::size_t max_output) {
    if (!IsGzip(s)) return s;

    z_stream zs{};
    // 15 window bits | 16 selects gzip framing (rather than raw deflate or zlib).
    if (inflateInit2(&zs, 15 + 16) != Z_OK)
        throw std::runtime_error("payload inflate failed: could not initialise zlib");

    // Free the stream on every exit path, including the throws below.
    struct Guard {
        z_stream *z;
        ~Guard() { inflateEnd(z); }
    } guard{&zs};

    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(s.data()));
    zs.avail_in = static_cast<uInt>(s.size());

    std::string out;
    // gzip on this payload runs ~48x, so guessing high costs one reallocation at
    // worst and saves a dozen on a 180 MB package.
    out.reserve(s.size() * 8);

    std::vector<char> buf(1 << 20);
    int rc = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef *>(buf.data());
        zs.avail_out = static_cast<uInt>(buf.size());
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            const char *why = zs.msg ? zs.msg : "corrupt or truncated stream";
            throw std::runtime_error(std::string("payload inflate failed: ") + why);
        }
        const std::size_t produced = buf.size() - zs.avail_out;
        if (out.size() + produced > max_output)
            throw std::runtime_error("payload inflate failed: inflated size exceeds the limit");
        out.append(buf.data(), produced);
        // Z_BUF_ERROR with no input left and nothing produced means the stream
        // ended mid-member: report it rather than returning a short payload.
        if (rc == Z_BUF_ERROR && produced == 0)
            throw std::runtime_error("payload inflate failed: truncated stream");
    } while (rc != Z_STREAM_END);

    return out;
}

} // namespace erpl_rev
