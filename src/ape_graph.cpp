#include "ape_graph.hpp"

#include "ape_decode.hpp"

namespace erpl_rev {
namespace ape {

namespace {

std::string JsonEscape(const std::string &v, const std::string &field) {
    std::string out;
    for (const unsigned char c : v) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                    throw ApeDecodeError("APE graph: " + field +
                                         " holds a control character");
                out += static_cast<char>(c);
        }
    }
    return out;
}

}  // namespace

std::string BuildGraphJson(const ApeGraphSpec &spec) {
    if (spec.has_filter)
        throw ApeDecodeError(
            "APE graph: a filter is refused, never emitted: the v6 reader "
            "has no filter path and would silently return all rows (ADR-1)");
    if (spec.graph_id.empty()) throw ApeDecodeError("APE graph: graph_id is required");
    if (spec.cds_name.empty()) throw ApeDecodeError("APE graph: cds_name is required");
    if (spec.action != "Initial Load" && spec.action != "Replication")
        throw ApeDecodeError("APE graph: action must be 'Initial Load' or 'Replication'");
    if (spec.subscription_type != "New" && spec.subscription_type != "Existing")
        throw ApeDecodeError("APE graph: subscription_type must be 'New' or 'Existing'");

    std::string sub;
    if (spec.subscription_type == "New") {
        if (spec.subscription_name.empty())
            throw ApeDecodeError(
                "APE graph: a New subscription needs subscription_name");
        sub = "\"subscriptionType\":\"New\",\"subscriptionName\":\"" +
              JsonEscape(spec.subscription_name, "subscription_name") + "\",";
    } else {
        if (spec.subscription_id.empty())
            throw ApeDecodeError(
                "APE graph: an Existing subscription needs subscription_id");
        sub = "\"subscriptionType\":\"Existing\",\"subscriptionID\":\"" +
              JsonEscape(spec.subscription_id, "subscription_id") + "\",";
    }

    const long long chunk = spec.chunk_size > 0 ? spec.chunk_size : kDefaultChunkSize;
    std::string wire;
    if (!spec.wireformat.empty())
        wire = ",\"wireformat\":\"" + JsonEscape(spec.wireformat, "wireformat") + "\"";

    return "{\"Attributes\":{\"graphid\":\"" + JsonEscape(spec.graph_id, "graph_id") +
           "\",\"protocol\":\"v6\",\"graphkind\":\"user\",\"multiplicity\":\"1\"},"
           "\"Processes\":{\"reader\":{\"Component\":\"com.sap.abap.cds.reader.v2\","
           "\"Metadata\":{\"Config\":{" +
           sub + "\"cdsname\":\"" + JsonEscape(spec.cds_name, "cds_name") +
           "\",\"action\":\"" + spec.action + "\",\"chunkSize\":" +
           std::to_string(chunk) + wire + "}}}},\"Connections\":[],\"Outports\":{"
           "\"0\":{\"Process\":\"reader\",\"Port\":\"outMessageData\",\"Metadata\":{"
           "\"portNumber\":0,\"type\":\"message\"}}},\"vTypes\":{}}";
}

ApeGraphSpec MakeGraphSpec(const std::string &method, const std::string &cds_name,
                           const std::string &subscriber_process,
                           const std::string &subscription_name_override,
                           long long chunk_size, const std::string &wireformat,
                           const std::string &subscription_id) {
    ApeGraphSpec s;
    if (method == "APE_FULL")
        s.action = "Initial Load";
    else if (method == "APE_DELTA")
        s.action = "Replication";
    else
        throw ApeDecodeError("APE graph: unknown method '" + method +
                             "'; expected APE_FULL or APE_DELTA");
    if (subscriber_process.empty() && subscription_name_override.empty())
        throw ApeDecodeError("APE graph: a New subscription needs a name: register " +
                             method + " with subscriber_process");
    s.graph_id = "erpl_rev_" + method;
    s.cds_name = cds_name;
    if (!subscription_id.empty()) {
        s.subscription_type = "Existing";
        s.subscription_id = subscription_id;
    } else {
        s.subscription_type = "New";
        s.subscription_name = subscription_name_override.empty() ? subscriber_process
                                                                 : subscription_name_override;
    }
    s.chunk_size = chunk_size;
    s.wireformat = wireformat;
    return s;
}

}  // namespace ape
}  // namespace erpl_rev
