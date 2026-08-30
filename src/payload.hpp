// Ingest payload framing: transparently accept a gzip-compressed package.
//
// A replication package is binary sXML, which begins with the ASCII magic
// "BXML". gzip begins with 1F 8B. The two cannot be confused, so the wire needs
// no encoding flag and no change to the Z_DUCKDB_INGEST signature — the server
// looks at the first two bytes and inflates when it has to. That also means a
// server can be upgraded independently of the ABAP transport, in either order.
//
// Why it is worth doing at all: on a wide table the BXML wraps every cell in tag
// scaffolding and most cells are initial, so a 50,000-row x 420-column package
// measured 180,522,994 bytes raw and 3,768,726 gzipped — 47.9x — while the
// payload path (transfer + ingest completion) is ~60% of a wide load's wall
// clock. See DataZooDE/erpl-rev#68.
#pragma once

#include <cstddef>
#include <string>

namespace erpl_rev {

// True if `s` starts with the gzip magic (1F 8B).
bool IsGzip(const std::string &s);

// Inflate `s` if it is gzip-framed, otherwise return it unchanged.
//
// Throws std::runtime_error with a message naming *inflation* on a corrupt or
// truncated stream — a bad payload must not surface later as a confusing
// "sXML: missing BXML magic" from the decoder.
//
// `max_output` caps the inflated size so a malformed or hostile package cannot
// exhaust memory (a few hundred bytes of gzip can encode gigabytes of zeros).
// The default is generous next to a real package: our largest measured one is
// ~180 MB raw.
std::string MaybeInflate(const std::string &s,
                         std::size_t max_output = 8ULL * 1024 * 1024 * 1024);

} // namespace erpl_rev
