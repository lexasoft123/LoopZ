#pragma once

#include <loopz/macos/editor_service.hpp>

#include <chrono>
#include <cstring>

namespace loopz::macos::editor {
inline uint64_t milliseconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

inline void dictionary(xpc_object_t value, size_t fields) {
    if (!value || xpc_get_type(value) != XPC_TYPE_DICTIONARY ||
        xpc_dictionary_get_count(value) != fields)
        throw std::invalid_argument("Invalid editor message fields");
}

inline void protocol(xpc_object_t value) {
    if (!value || xpc_get_type(value) != XPC_TYPE_DICTIONARY)
        throw std::invalid_argument("Editor message must be a dictionary");
    const auto version = xpc_dictionary_get_value(value, "protocol");
    if (!version || xpc_get_type(version) != XPC_TYPE_UINT64 ||
        xpc_uint64_get_value(version) != kEditorProtocolVersion)
        throw std::invalid_argument("Unsupported editor transport protocol");
}

inline std::string text(xpc_object_t object, const char* key, size_t maximum = 4096) {
    const auto value = xpc_dictionary_get_value(object, key);
    if (!value || xpc_get_type(value) != XPC_TYPE_STRING || !xpc_string_get_length(value) ||
        xpc_string_get_length(value) > maximum)
        throw std::invalid_argument("Invalid editor message text");
    const auto bytes = xpc_string_get_string_ptr(value);
    const auto size = xpc_string_get_length(value);
    if (std::strlen(bytes) != size)
        throw std::invalid_argument("Editor message text contains a null character");
    return {bytes, size};
}

inline std::string data(xpc_object_t object, size_t maximum) {
    const auto value = xpc_dictionary_get_value(object, "payload");
    if (!value || xpc_get_type(value) != XPC_TYPE_DATA || !xpc_data_get_length(value) ||
        xpc_data_get_length(value) > maximum)
        throw std::invalid_argument("Invalid editor message payload");
    return {static_cast<const char*>(xpc_data_get_bytes_ptr(value)), xpc_data_get_length(value)};
}

inline const char* failureName(EditorFailure value) {
    switch (value) {
    case EditorFailure::Busy:
        return "busy";
    case EditorFailure::Expired:
        return "expired";
    case EditorFailure::Conflict:
        return "conflict";
    case EditorFailure::Invalid:
        return "invalid";
    case EditorFailure::Unavailable:
        return "unavailable";
    case EditorFailure::Failed:
        return "failed";
    }
    return "failed";
}

inline EditorFailure failureCode(const std::string& name) {
    for (auto code : {EditorFailure::Busy, EditorFailure::Expired, EditorFailure::Conflict,
                      EditorFailure::Invalid, EditorFailure::Unavailable, EditorFailure::Failed})
        if (name == failureName(code))
            return code;
    throw std::invalid_argument("Unknown editor error code");
}

inline xpc_object_t message(const char* operation) {
    auto result = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_uint64(result, "protocol", kEditorProtocolVersion);
    xpc_dictionary_set_string(result, "op", operation);
    return result;
}

inline void success(xpc_connection_t peer, xpc_object_t reply, std::string_view payload) {
    if (payload.empty() || payload.size() > kMaximumEditorReplyBytes)
        throw std::runtime_error("Editor response exceeds its payload bound");
    xpc_dictionary_set_uint64(reply, "protocol", kEditorProtocolVersion);
    xpc_dictionary_set_bool(reply, "ok", true);
    xpc_dictionary_set_data(reply, "payload", payload.data(), payload.size());
    xpc_connection_send_message(peer, reply);
}

inline void failure(xpc_connection_t peer, xpc_object_t reply, EditorFailure code,
                    std::string_view detail) {
    if (!reply)
        return;
    const std::string bounded(detail.substr(0, 4096));
    xpc_dictionary_set_uint64(reply, "protocol", kEditorProtocolVersion);
    xpc_dictionary_set_bool(reply, "ok", false);
    xpc_dictionary_set_string(reply, "code", failureName(code));
    xpc_dictionary_set_string(reply, "detail",
                              bounded.empty() ? "Editor operation failed" : bounded.c_str());
    xpc_connection_send_message(peer, reply);
}
} // namespace loopz::macos::editor
