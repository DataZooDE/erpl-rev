// Ingest payload framing: transparently accept a compressed package.
//
// A replication package is binary sXML, which begins with the ASCII magic
// "BXML". A compressed one is framed by the ABAP side as the ASCII magic "ERPZ"
// followed by a raw DEFLATE stream. Neither can be confused with the other, so
// the wire needs no encoding flag and no change to the Z_DUCKDB_INGEST
// signature — the server looks at the first four bytes. That also lets a server
// be upgraded independently of the ABAP transport, in either order.
//
// Why our own frame rather than gzip: `cl_abap_gzip=>compress_binary` emits a
// RAW DEFLATE stream (RFC 1951) with no header — not gzip (1F 8B) and not zlib
// (78 ..), verified on the A4H trial. Raw deflate carries nothing to detect it
// by, so the framing has to come from us. Real gzip is accepted too, so a
// producer that does emit it keeps working.
//
// Why it is worth doing: on a wide table the BXML wraps every cell in tag
// scaffolding and most cells are initial, so a 50,000-row x 420-column package
// measured 180,522,994 bytes raw against 3,768,726 compressed — 47.9x — while
// the payload path is ~60% of a wide load's wall clock. See erpl-rev#68.
#pragma once

#include <cstddef>
#include <string>

namespace erpl_rev {

// The ABAP-side frame: these four bytes, then a raw DEFLATE stream.
inline constexpr char kDeflateMagic[] = "ERPZ";

// True if `s` carries a payload this module must decompress.
bool IsCompressed(const std::string &s);

// Decompress `s` if it is framed ("ERPZ" + raw deflate, or gzip), else return it
// unchanged.
//
// Throws std::runtime_error naming *inflation* on a corrupt or truncated
// stream — a bad payload must not surface later as a confusing
// "sXML: missing BXML magic" from the decoder, which would send whoever debugs
// it to the wrong layer entirely.
//
// `max_output` caps the inflated size so a malformed or hostile package cannot
// exhaust memory (a few hundred bytes can encode gigabytes of zeros). The
// default is generous next to a real package: our largest measured one is
// ~180 MB raw.
std::string MaybeInflate(const std::string &s,
                         std::size_t max_output = 8ULL * 1024 * 1024 * 1024);

} // namespace erpl_rev
