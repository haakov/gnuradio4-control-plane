#include "gr4cp/domain/session_stream.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <optional>
#include <ranges>
#include <set>
#include <string_view>
#include <type_traits>

#include <gnuradio-4.0/YamlPmt.hpp>

namespace gr4cp::domain {

namespace {

std::optional<std::string> value_as_string(const gr::pmt::Value& value) {
    std::optional<std::string> result;
    gr::pmt::ValueVisitor([&result](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::same_as<T, std::pmr::string> || std::same_as<T, std::string>) {
            result = std::string(item);
        } else if constexpr (std::same_as<T, std::string_view>) {
            result = std::string(item);
        }
    }).visit(value);
    return result;
}

const auto& supported_transports() {
    static const auto transports = std::to_array<std::string_view>({
        "http_snapshot",
        "http_poll",
        "websocket",
    });
    return transports;
}

bool transport_supported(std::string_view transport) {
    return std::ranges::find(supported_transports(), transport) != supported_transports().end();
}

std::string make_stream_path(std::string_view session_id, std::string_view stream_id) {
    return "/sessions/" + std::string(session_id) + "/streams/" + std::string(stream_id) + "/http";
}

std::string make_stream_path(std::string_view session_id, std::string_view stream_id, std::string_view transport) {
    if (transport == "websocket") {
        return "/sessions/" + std::string(session_id) + "/streams/" + std::string(stream_id) + "/ws";
    }
    return make_stream_path(session_id, stream_id);
}

struct AuthoredStreamIntent {
    bool declared{false};
    std::optional<std::string> stream_id;
    std::optional<std::string> transport;
    std::optional<std::string> payload_format;
};

struct NormalizedBlock {
    std::string block_type_id;
    std::string instance_name;
    AuthoredStreamIntent stream;
};

std::optional<gr::property_map> value_to_property_map(const gr::pmt::Value& value) {
    if (const auto* map = value.get_if<gr::property_map>(); map != nullptr) {
        return *map;
    }
    return std::nullopt;
}

std::optional<std::string> parameter_string(const gr::property_map& parameters, std::string_view key) {
    const auto it = parameters.find(std::string(key));
    if (it == parameters.end()) {
        return std::nullopt;
    }
    return value_as_string(it->second);
}

AuthoredStreamIntent normalize_stream_intent(const gr::property_map& parameters) {
    AuthoredStreamIntent intent;

    if (const auto it = parameters.find("stream"); it != parameters.end()) {
        intent.declared = true;
        if (const auto stream = value_to_property_map(it->second); stream.has_value()) {
            intent.stream_id = parameter_string(*stream, "id");
            intent.transport = parameter_string(*stream, "transport");
            intent.payload_format = parameter_string(*stream, "payload_format");
        }
        return intent;
    }

    intent.stream_id = parameter_string(parameters, "stream_id");
    intent.transport = parameter_string(parameters, "transport");
    intent.payload_format = parameter_string(parameters, "payload_format");
    intent.declared = intent.stream_id.has_value() || intent.transport.has_value() || intent.payload_format.has_value();
    return intent;
}

std::optional<NormalizedBlock> normalize_stream_block(const gr::property_map& block) {
    std::optional<std::string> studio_node_id;
    if (const auto it = block.find("id"); it != block.end()) {
        studio_node_id = value_as_string(it->second);
    }

    std::optional<std::string> block_type_id;
    if (const auto it = block.find("block"); it != block.end()) {
        block_type_id = value_as_string(it->second);
    }
    if (!block_type_id.has_value()) {
        if (const auto it = block.find("id"); it != block.end()) {
            block_type_id = value_as_string(it->second);
        }
    }
    if (!block_type_id.has_value()) {
        if (const auto it = block.find("block_type"); it != block.end()) {
            block_type_id = value_as_string(it->second);
        }
    }
    if (!block_type_id.has_value() || block_type_id->empty()) {
        return std::nullopt;
    }

    gr::property_map parameters;
    if (const auto it = block.find("parameters"); it != block.end()) {
        if (const auto* existing = it->second.get_if<gr::property_map>(); existing != nullptr) {
            parameters = *existing;
        }
    }
    if (const auto it = block.find("raw_parameters"); it != block.end()) {
        if (const auto* raw = it->second.get_if<gr::property_map>(); raw != nullptr) {
            for (const auto& [key, value] : *raw) {
                if (!parameters.contains(key)) {
                    parameters.insert_or_assign(key, value);
                }
            }
        }
    }

    std::optional<std::string> instance_name;
    if (const auto it = parameters.find("name"); it != parameters.end()) {
        instance_name = value_as_string(it->second);
    }
    if ((!instance_name.has_value() || instance_name->empty()) && studio_node_id.has_value() && !studio_node_id->empty() &&
        block.contains("block")) {
        instance_name = studio_node_id;
    }
    if (!instance_name.has_value() || instance_name->empty()) {
        for (const auto* candidate : {"instance_name", "name"}) {
            const auto it = block.find(candidate);
            if (it == block.end()) {
                continue;
            }
            instance_name = value_as_string(it->second);
            if (instance_name.has_value() && !instance_name->empty()) {
                break;
            }
        }
    }

    return NormalizedBlock{
        .block_type_id = *block_type_id,
        .instance_name = instance_name.value_or(""),
        .stream = normalize_stream_intent(parameters),
    };
}

std::optional<std::string> validate_stream_block(const NormalizedBlock& block,
                                                 std::set<std::string>& seen_stream_ids) {
    if (!block.stream.declared) {
        return std::nullopt;
    }
    if (block.instance_name.empty()) {
        return "managed stream block is missing instance name: " + block.block_type_id;
    }

    const auto stream_id = block.stream.stream_id.has_value() && !block.stream.stream_id->empty()
                               ? *block.stream.stream_id
                               : block.instance_name;
    if (stream_id.empty()) {
        return "managed stream id is missing: " + block.block_type_id;
    }
    if (!block.stream.transport.has_value() || block.stream.transport->empty()) {
        return "managed stream block is missing transport: " + block.block_type_id;
    }
    if (!transport_supported(*block.stream.transport)) {
        return "managed stream transport is not supported: " + *block.stream.transport;
    }
    if (!block.stream.payload_format.has_value() || block.stream.payload_format->empty()) {
        return "managed stream block is missing payload_format: " + block.block_type_id;
    }
    if (!seen_stream_ids.insert(stream_id).second) {
        return "managed stream id is duplicated: " + stream_id;
    }

    return std::nullopt;
}

}  // namespace

BrowserStreamDescriptor to_browser_stream_descriptor(const RuntimeStreamBindingPlan& plan) {
    return BrowserStreamDescriptor{
        .id = plan.stream_id,
        .block_instance_name = plan.block_instance_name,
        .transport = plan.transport,
        .payload_format = plan.payload_format,
        .path = plan.path,
        .ready = plan.ready,
    };
}

std::optional<StreamRuntimePlan> derive_stream_runtime_plan(const Session& session) {
    const auto parsed = gr::pmt::yaml::deserialize(session.grc_content);
    if (!parsed.has_value()) {
        return std::nullopt;
    }

    const auto blocks_it = parsed->find("blocks");
    if (blocks_it == parsed->end()) {
        return std::nullopt;
    }

    const auto* blocks = blocks_it->second.get_if<gr::Tensor<gr::pmt::Value>>();
    if (blocks == nullptr) {
        return std::nullopt;
    }

    StreamRuntimePlan plan;
    std::set<std::string> seen_stream_ids;
    for (const auto& block_value : *blocks) {
        const auto* block = block_value.get_if<gr::property_map>();
        if (block == nullptr) {
            continue;
        }

        const auto normalized = normalize_stream_block(*block);
        if (!normalized.has_value()) {
            continue;
        }
        if (!normalized->stream.declared) {
            continue;
        }
        if (validate_stream_block(*normalized, seen_stream_ids).has_value()) {
            return std::nullopt;
        }
        const auto stream_id = normalized->stream.stream_id.has_value() && !normalized->stream.stream_id->empty()
                                   ? *normalized->stream.stream_id
                                   : normalized->instance_name;

        plan.streams.push_back(RuntimeStreamBindingPlan{
            .stream_id = stream_id,
            .block_instance_name = normalized->instance_name,
            .transport = *normalized->stream.transport,
            .payload_format = *normalized->stream.payload_format,
            .path = make_stream_path(session.id, stream_id, *normalized->stream.transport),
            .ready = true,
        });
    }

    if (plan.streams.empty()) {
        return std::nullopt;
    }

    return plan;
}

std::optional<std::string> validate_stream_runtime_contract(const Session& session) {
    const auto parsed = gr::pmt::yaml::deserialize(session.grc_content);
    if (!parsed.has_value()) {
        return std::nullopt;
    }

    const auto blocks_it = parsed->find("blocks");
    if (blocks_it == parsed->end()) {
        return std::nullopt;
    }

    const auto* blocks = blocks_it->second.get_if<gr::Tensor<gr::pmt::Value>>();
    if (blocks == nullptr) {
        return std::nullopt;
    }

    std::set<std::string> seen_stream_ids;
    for (const auto& block_value : *blocks) {
        const auto* block = block_value.get_if<gr::property_map>();
        if (block == nullptr) {
            continue;
        }

        const auto normalized = normalize_stream_block(*block);
        if (!normalized.has_value()) {
            continue;
        }
        if (!normalized->stream.declared) {
            continue;
        }
        if (const auto error = validate_stream_block(*normalized, seen_stream_ids); error.has_value()) {
            return error;
        }
    }

    return std::nullopt;
}

std::vector<std::optional<StreamRuntimePlan>> derive_stream_runtime_plans(const std::vector<Session>& sessions) {
    std::vector<std::optional<StreamRuntimePlan>> plans;
    plans.reserve(sessions.size());
    for (const auto& session : sessions) {
        plans.push_back(derive_stream_runtime_plan(session));
    }
    return plans;
}

}  // namespace gr4cp::domain
