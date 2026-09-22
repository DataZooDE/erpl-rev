// APE envelope extraction: JSON port data -> body + fields + flow flags.
//
// `DHAPE_PORT-PORT_DATA` is NULL when a roundtrip has nothing to hand over,
// a data envelope (Fields + Body) when it does, or a control envelope
// (neither, but still carrying lastBatch). The JSON reader below parses
// exactly the subset the engine emits -- objects, arrays, strings with full
// escapes, integers, booleans, null -- and refuses everything else, so a
// changed wire shape fails loudly at the gate.
#pragma once

#include <string>
#include <vector>

#include "ape_decode.hpp"

namespace erpl_rev {
namespace ape {

struct ApeEnvelope {
    bool has_data = false;    // data envelope (Fields + Body present)
    bool is_control = false;  // valid envelope, neither present
    bool last_batch = false;  // end-of-data signal (either kind)
    long long batch_index = -1;
    std::string body;
    std::vector<ApeField> fields;
};

// nullptr (the engine's NULL handover) or empty input -> no data, no throw.
// Anything else must be a well-formed envelope or throws ApeDecodeError.
ApeEnvelope ExtractEnvelope(const char *port_data);

}  // namespace ape
}  // namespace erpl_rev
