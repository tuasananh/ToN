#include "ton/core/graph.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <numbers>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace ton {

namespace {

std::string trim_ascii(std::string_view sv) {
    size_t first = 0;
    while (first < sv.size() && std::isspace(static_cast<unsigned char>(sv[first])) != 0) {
        ++first;
    }
    size_t last = sv.size();
    while (last > first && std::isspace(static_cast<unsigned char>(sv[last - 1])) != 0) {
        --last;
    }
    return std::string(sv.substr(first, last - first));
}

std::string extract_attribute(std::string_view tag_header, std::string_view attr_name) {
    const std::string key = std::string(attr_name) + "=\"";
    const size_t pos = tag_header.find(key);
    if (pos == std::string_view::npos) {
        return {};
    }
    const size_t val_start = pos + key.size();
    const size_t val_end = tag_header.find('"', val_start);
    if (val_end == std::string_view::npos) {
        return {};
    }
    return trim_ascii(tag_header.substr(val_start, val_end - val_start));
}

std::string extract_tag_value(std::string_view block, std::string_view tag_name) {
    const std::string open_tag = "<" + std::string(tag_name) + ">";
    const std::string close_tag = "</" + std::string(tag_name) + ">";
    const size_t start = block.find(open_tag);
    if (start == std::string_view::npos) {
        throw std::invalid_argument("Missing XML element <" + std::string(tag_name) + ">");
    }
    const size_t val_start = start + open_tag.size();
    const size_t val_end = block.find(close_tag, val_start);
    if (val_end == std::string_view::npos) {
        throw std::invalid_argument("Unclosed XML element <" + std::string(tag_name) + ">");
    }
    return trim_ascii(block.substr(val_start, val_end - val_start));
}

std::string_view extract_section(std::string_view xml, std::string_view section_tag) {
    const std::string open_prefix = "<" + std::string(section_tag);
    const std::string close_tag = "</" + std::string(section_tag) + ">";
    const size_t open_pos = xml.find(open_prefix);
    if (open_pos == std::string_view::npos) {
        throw std::invalid_argument("Missing XML section <" + std::string(section_tag) + ">");
    }
    const size_t gt_pos = xml.find('>', open_pos + open_prefix.size());
    if (gt_pos == std::string_view::npos) {
        throw std::invalid_argument("Malformed XML section <" + std::string(section_tag) + ">");
    }
    const size_t close_pos = xml.find(close_tag, gt_pos + 1);
    if (close_pos == std::string_view::npos) {
        throw std::invalid_argument("Unclosed XML section <" + std::string(section_tag) + ">");
    }
    return xml.substr(gt_pos + 1, close_pos - (gt_pos + 1));
}

double parse_strict_double(const std::string& text, std::string_view context) {
    size_t idx = 0;
    const double val = std::stod(text, &idx);
    if (idx != text.size() || !std::isfinite(val)) {
        throw std::invalid_argument("Invalid numeric value '" + text + "' in " +
                                    std::string(context));
    }
    return val;
}

}  // namespace

double Graph::haversine_km(double lon1_deg, double lat1_deg, double lon2_deg,
                           double lat2_deg) noexcept {
    if (std::tie(lon1_deg, lat1_deg) > std::tie(lon2_deg, lat2_deg)) {
        std::swap(lon1_deg, lon2_deg);
        std::swap(lat1_deg, lat2_deg);
    }
    constexpr double kDegToRad = std::numbers::pi / 180.0;
    const double phi1 = lat1_deg * kDegToRad;
    const double phi2 = lat2_deg * kDegToRad;
    const double dphi = (lat2_deg - lat1_deg) * kDegToRad;
    const double dlam = (lon2_deg - lon1_deg) * kDegToRad;
    const double sin_dphi = std::sin(0.5 * dphi);
    const double sin_dlam = std::sin(0.5 * dlam);
    const double a = sin_dphi * sin_dphi + std::cos(phi1) * std::cos(phi2) * (sin_dlam * sin_dlam);
    return 2.0 * EARTH_RADIUS_KM * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
}

Graph::Graph(std::string name, std::vector<NodeInfo> nodes, const std::vector<SpanInput>& spans,
             DistanceMetric default_metric)
    : name_(std::move(name)), default_metric_(default_metric), nodes_(std::move(nodes)) {
    if (nodes_.empty() || nodes_.size() >= INVALID_NODE) {
        throw std::invalid_argument("Graph requires between 1 and 65534 nodes");
    }
    if (spans.size() * 2 >= INVALID_ARC) {
        throw std::invalid_argument("Graph exceeds maximum supported arc count");
    }

    for (size_t i = 0; i < nodes_.size(); ++i) {
        nodes_[i].id = static_cast<NodeId>(i);
        if (nodes_[i].name.empty()) {
            nodes_[i].name = "N" + std::to_string(i);
        }
        if (nodes_[i].lon_deg < -180.0 || nodes_[i].lon_deg > 180.0 || nodes_[i].lat_deg < -90.0 ||
            nodes_[i].lat_deg > 90.0 || !std::isfinite(nodes_[i].lon_deg) ||
            !std::isfinite(nodes_[i].lat_deg)) {
            throw std::invalid_argument("Out-of-range geographic coordinates on node " +
                                        nodes_[i].name);
        }
        auto [it, inserted] = name_to_id_.emplace(nodes_[i].name, static_cast<NodeId>(i));
        if (!inserted) {
            throw std::invalid_argument("Duplicate node name: " + nodes_[i].name);
        }
    }

    spans_.reserve(spans.size());
    arcs_.reserve(spans.size() * 2);
    std::set<std::pair<NodeId, NodeId>> seen_spans;
    std::vector<std::vector<ArcId>> per_node_out(nodes_.size());

    for (size_t i = 0; i < spans.size(); ++i) {
        const auto& s_in = spans[i];
        if (s_in.u >= nodes_.size() || s_in.v >= nodes_.size()) {
            throw std::invalid_argument("Span endpoint out of range");
        }
        if (s_in.u == s_in.v) {
            throw std::invalid_argument("Self-loop span prohibited on node " + nodes_[s_in.u].name);
        }
        const auto canon = std::minmax(s_in.u, s_in.v);
        if (!seen_spans.insert(canon).second) {
            throw std::invalid_argument("Duplicate physical span between " +
                                        nodes_[canon.first].name + " and " +
                                        nodes_[canon.second].name);
        }

        const double len_km = s_in.override_length_km > 0.0
                                  ? s_in.override_length_km
                                  : haversine_km(nodes_[s_in.u].lon_deg, nodes_[s_in.u].lat_deg,
                                                 nodes_[s_in.v].lon_deg, nodes_[s_in.v].lat_deg);
        if (!(len_km > 0.0 && std::isfinite(len_km))) {
            throw std::invalid_argument("Non-positive span length between " + nodes_[s_in.u].name +
                                        " and " + nodes_[s_in.v].name);
        }

        const auto sid = static_cast<SpanId>(i);
        const auto arc_uv_id = static_cast<ArcId>(2 * i);
        const auto arc_vu_id = static_cast<ArcId>(2 * i + 1);
        const std::string span_name = s_in.name.empty() ? ("E" + std::to_string(i)) : s_in.name;

        spans_.push_back(Span{
            .id = sid,
            .name = span_name,
            .u = s_in.u,
            .v = s_in.v,
            .arc_uv = arc_uv_id,
            .arc_vu = arc_vu_id,
            .length_km = len_km,
        });

        arcs_.push_back(Arc{
            .id = arc_uv_id,
            .src = s_in.u,
            .dst = s_in.v,
            .span_id = sid,
            .reverse_arc = arc_vu_id,
            .length_km = len_km,
        });
        arcs_.push_back(Arc{
            .id = arc_vu_id,
            .src = s_in.v,
            .dst = s_in.u,
            .span_id = sid,
            .reverse_arc = arc_uv_id,
            .length_km = len_km,
        });

        per_node_out[s_in.u].push_back(arc_uv_id);
        per_node_out[s_in.v].push_back(arc_vu_id);
    }

    adj_offsets_.resize(nodes_.size() + 1, 0);
    adj_arcs_.reserve(arcs_.size());
    for (size_t u = 0; u < nodes_.size(); ++u) {
        auto& out = per_node_out[u];
        std::sort(out.begin(), out.end(),
                  [this](ArcId a, ArcId b) { return arcs_[a].dst < arcs_[b].dst; });
        adj_offsets_[u] = static_cast<uint32_t>(adj_arcs_.size());
        adj_arcs_.insert(adj_arcs_.end(), out.begin(), out.end());
    }
    adj_offsets_[nodes_.size()] = static_cast<uint32_t>(adj_arcs_.size());
}

Graph Graph::from_sndlib_xml(const std::filesystem::path& xml_path, DistanceMetric default_metric) {
    std::ifstream in(xml_path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Unable to open SNDlib XML file: " + xml_path.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string xml = buffer.str();

    const std::string_view net_struct = extract_section(xml, "networkStructure");
    const std::string_view nodes_sec = extract_section(net_struct, "nodes");
    const std::string_view links_sec = extract_section(net_struct, "links");

    std::vector<NodeInfo> nodes;
    std::unordered_map<std::string, NodeId> node_lookup;

    size_t cursor = 0;
    while (true) {
        const size_t open_pos = nodes_sec.find("<node ", cursor);
        if (open_pos == std::string_view::npos) {
            break;
        }
        const size_t header_end = nodes_sec.find('>', open_pos);
        const size_t close_pos = nodes_sec.find("</node>", header_end);
        if (header_end == std::string_view::npos || close_pos == std::string_view::npos) {
            throw std::invalid_argument("Malformed <node> element in " + xml_path.string());
        }
        const std::string node_id_str =
            extract_attribute(nodes_sec.substr(open_pos, header_end - open_pos + 1), "id");
        if (node_id_str.empty()) {
            throw std::invalid_argument("Missing node id attribute in " + xml_path.string());
        }
        const std::string_view body =
            nodes_sec.substr(header_end + 1, close_pos - (header_end + 1));
        const std::string_view coords = extract_section(body, "coordinates");
        const double lon = parse_strict_double(extract_tag_value(coords, "x"), "node <x>");
        const double lat = parse_strict_double(extract_tag_value(coords, "y"), "node <y>");

        const auto nid = static_cast<NodeId>(nodes.size());
        if (!node_lookup.emplace(node_id_str, nid).second) {
            throw std::invalid_argument("Duplicate node id '" + node_id_str + "' in " +
                                        xml_path.string());
        }
        nodes.push_back(NodeInfo{
            .id = nid,
            .name = node_id_str,
            .lon_deg = lon,
            .lat_deg = lat,
        });
        cursor = close_pos + 7;
    }

    std::vector<SpanInput> spans;
    cursor = 0;
    while (true) {
        const size_t open_pos = links_sec.find("<link ", cursor);
        if (open_pos == std::string_view::npos) {
            break;
        }
        const size_t header_end = links_sec.find('>', open_pos);
        const size_t close_pos = links_sec.find("</link>", header_end);
        if (header_end == std::string_view::npos || close_pos == std::string_view::npos) {
            throw std::invalid_argument("Malformed <link> element in " + xml_path.string());
        }
        const std::string link_id_str =
            extract_attribute(links_sec.substr(open_pos, header_end - open_pos + 1), "id");
        const std::string_view body =
            links_sec.substr(header_end + 1, close_pos - (header_end + 1));
        const std::string src_name = extract_tag_value(body, "source");
        const std::string dst_name = extract_tag_value(body, "target");

        const auto src_it = node_lookup.find(src_name);
        const auto dst_it = node_lookup.find(dst_name);
        if (src_it == node_lookup.end() || dst_it == node_lookup.end()) {
            throw std::invalid_argument("Unknown link endpoint in " + xml_path.string() + ": (" +
                                        src_name + ", " + dst_name + ")");
        }
        spans.push_back(SpanInput{
            .name = link_id_str,
            .u = src_it->second,
            .v = dst_it->second,
            .override_length_km = 0.0,
        });
        cursor = close_pos + 7;
    }

    return Graph(xml_path.stem().string(), std::move(nodes), spans, default_metric);
}

ArcId Graph::find_arc(NodeId src, NodeId dst) const noexcept {
    if (src >= nodes_.size() || dst >= nodes_.size()) {
        return INVALID_ARC;
    }
    const auto out = out_arcs(src);
    const auto it = std::lower_bound(
        out.begin(), out.end(), dst,
        [this](ArcId arc_id, NodeId target) { return arcs_[arc_id].dst < target; });
    if (it != out.end() && arcs_[*it].dst == dst) {
        return *it;
    }
    return INVALID_ARC;
}

NodeId Graph::find_node(std::string_view node_name) const noexcept {
    const auto it = name_to_id_.find(std::string(node_name));
    return it == name_to_id_.end() ? INVALID_NODE : it->second;
}

}  // namespace ton
