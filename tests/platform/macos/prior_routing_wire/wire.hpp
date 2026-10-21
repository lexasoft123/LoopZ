#include <loopz/sample_rates.hpp>
#pragma once
#include <loopz/macos/routing_service.hpp>
#include <loopz/macos/device_transport.hpp>

#include <climits>
#include <stdexcept>
#include <string_view>

namespace loopz::macos::routing {
inline uint64_t integer(xpc_object_t object, const char* key) {
    const auto value = xpc_dictionary_get_value(object, key);
    if (!value || xpc_get_type(value) != XPC_TYPE_UINT64)
        throw std::invalid_argument("Missing or invalid routing integer");
    return xpc_uint64_get_value(value);
}
inline int32_t resultCode(xpc_object_t object, const char* key) {
    const auto value = xpc_dictionary_get_value(object, key);
    if (!value || xpc_get_type(value) != XPC_TYPE_INT64)
        throw std::invalid_argument("Missing or invalid routing result");
    const auto result = xpc_int64_get_value(value);
    if (result < INT32_MIN || result > INT32_MAX)
        throw std::invalid_argument("Routing result outside int32 range");
    return static_cast<int32_t>(result);
}
inline bool boolean(xpc_object_t object, const char* key) {
    const auto value = xpc_dictionary_get_value(object, key);
    if (!value || xpc_get_type(value) != XPC_TYPE_BOOL)
        throw std::invalid_argument("Missing or invalid routing flag");
    return xpc_bool_get_value(value);
}
inline std::string_view text(xpc_object_t object, const char* key) {
    const auto value = xpc_dictionary_get_value(object, key);
    if (!value || xpc_get_type(value) != XPC_TYPE_STRING || xpc_string_get_length(value) > 4096)
        throw std::invalid_argument("Missing or invalid routing text");
    return {xpc_string_get_string_ptr(value), xpc_string_get_length(value)};
}
inline void dictionary(xpc_object_t object, size_t fields) {
    if (!object || xpc_get_type(object) != XPC_TYPE_DICTIONARY ||
        xpc_dictionary_get_count(object) != fields)
        throw std::invalid_argument("Invalid routing message fields");
}
inline void protocol(xpc_object_t object) {
    if (!object || xpc_get_type(object) != XPC_TYPE_DICTIONARY ||
        integer(object, "protocol") != kDeviceAudioProtocol)
        throw std::invalid_argument("Invalid routing service protocol");
}
inline xpc_object_t message(const char* operation) {
    auto value = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_uint64(value, "protocol", kDeviceAudioProtocol);
    xpc_dictionary_set_string(value, "op", operation);
    return value;
}
inline DriverConfigurationResult configuration(xpc_object_t object) {
    dictionary(object, 3);
    const auto revision = integer(object, "revision");
    const auto phase = integer(object, "phase");
    if (phase > static_cast<uint32_t>(DriverConfigurationPhase::Quarantined))
        throw std::invalid_argument("Invalid routing configuration phase");
    return {revision, static_cast<DriverConfigurationPhase>(phase), resultCode(object, "error")};
}
inline xpc_object_t encode(const DriverConfigurationResult& value) {
    auto object = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_uint64(object, "revision", value.revision);
    xpc_dictionary_set_uint64(object, "phase", static_cast<uint32_t>(value.phase));
    xpc_dictionary_set_int64(object, "error", value.error);
    return object;
}
inline void validateRateRequests(std::span<const DriverRateRequest> requests) {
    validateDriverRateRequests(requests);
}
inline std::vector<DriverRateRequest> rateRequests(xpc_object_t array) {
    if (!array || xpc_get_type(array) != XPC_TYPE_ARRAY ||
        xpc_array_get_count(array) > kMaximumVirtualDevices)
        throw std::invalid_argument("Invalid device rate request array");
    std::vector<DriverRateRequest> result;
    result.reserve(xpc_array_get_count(array));
    for (size_t index = 0; index < xpc_array_get_count(array); ++index) {
        auto object = xpc_array_get_value(array, index);
        dictionary(object, 4);
        const auto rate = integer(object, "sampleRate");
        if (!supportedVirtualSampleRate(rate))
            throw std::invalid_argument("Unsupported requested sample rate");
        result.push_back({std::string(text(object, "device")), integer(object, "sequence"),
                          static_cast<uint32_t>(rate), std::string(text(object, "origin"))});
    }
    validateRateRequests(result);
    return result;
}
inline xpc_object_t encodeRateRequests(std::span<const DriverRateRequest> requests) {
    validateRateRequests(requests);
    auto array = xpc_array_create(nullptr, 0);
    for (const auto& request : requests) {
        auto value = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_string(value, "device", request.device.c_str());
        xpc_dictionary_set_uint64(value, "sequence", request.sequence);
        xpc_dictionary_set_uint64(value, "sampleRate", request.sampleRate);
        xpc_dictionary_set_string(value, "origin", request.origin.c_str());
        xpc_array_append_value(array, value);
    }
    return array;
}
} // namespace loopz::macos::routing
