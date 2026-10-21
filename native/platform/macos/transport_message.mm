#include <loopz/macos/transport_message.hpp>

#include <stdexcept>
#include <set>
#include <unordered_set>
#include <utility>
#include <sys/mman.h>

namespace loopz::macos {
namespace {
bool validText(const std::string& text) {
    return !text.empty() && text.size() <= 4096 && text.find('\0') == std::string::npos;
}

std::string textField(xpc_object_t object, const char* name) {
    auto value = xpc_dictionary_get_value(object, name);
    if (!value || xpc_get_type(value) != XPC_TYPE_STRING || xpc_string_get_length(value) > 8192)
        throw std::invalid_argument("Invalid device transport text field");
    return {xpc_string_get_string_ptr(value), xpc_string_get_length(value)};
}

void validateIdentity(const std::string& id, const std::string& uid, const std::string& name) {
    if (!validText(id) || !validText(name) || uid != "com.lexasoft.loopz.device." + id)
        throw std::invalid_argument("Invalid device transport identity");
}
} // namespace

xpc_object_t encodeDeviceTransports(std::span<const DeviceTransport> devices, uint64_t revision) {
    if (!revision || devices.size() > kMaximumVirtualDevices)
        throw std::invalid_argument("Invalid device transport revision or count");
    auto result = xpc_dictionary_create(nullptr, nullptr, 0);
    auto array = xpc_array_create(nullptr, 0);
    std::unordered_set<std::string> ids;
    std::set<std::array<uint64_t, 2>> mappings;
    for (const auto& device : devices) {
        validateIdentity(device.id, device.uid, device.name);
        if (device.inputLatencyFrames > kMaximumInputLatencyFrames)
            throw std::invalid_argument("Device input latency exceeds transport limit");
        if (!ids.insert(device.id).second || !device.region)
            throw std::invalid_argument("Duplicate device or missing audio mapping");
        if (!mappings.insert(device.region->view().mappingIdentity()).second)
            throw std::invalid_argument("Different devices cannot share an audio mapping");
        auto memory = xpc_shmem_create(device.region->data(), device.region->bytes());
        if (!memory)
            throw std::runtime_error("Could not export device audio mapping");
        auto entry = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_string(entry, "id", device.id.c_str());
        xpc_dictionary_set_string(entry, "uid", device.uid.c_str());
        xpc_dictionary_set_string(entry, "name", device.name.c_str());
        xpc_dictionary_set_bool(entry, "hasOutput", device.hasOutput);
        xpc_dictionary_set_uint64(entry, "inputLatencyFrames", device.inputLatencyFrames);
        xpc_dictionary_set_value(entry, "audio", memory);
        xpc_array_append_value(array, entry);
    }
    xpc_dictionary_set_uint64(result, "protocol", kDeviceTransportProtocol);
    xpc_dictionary_set_uint64(result, "revision", revision);
    xpc_dictionary_set_value(result, "devices", array);
    return result;
}

RemoteDeviceTransport::RemoteDeviceTransport(std::string id, std::string uid, std::string name,
                                             bool hasOutput, uint32_t inputLatencyFrames)
    : id_(std::move(id)), uid_(std::move(uid)), name_(std::move(name)), hasOutput_(hasOutput),
      inputLatencyFrames_(inputLatencyFrames) {}
RemoteDeviceTransport::~RemoteDeviceTransport() {
    if (mapping_)
        ::munmap(mapping_, bytes_);
}
RemoteDeviceTransport::RemoteDeviceTransport(RemoteDeviceTransport&& other) noexcept
    : id_(std::move(other.id_)), uid_(std::move(other.uid_)), name_(std::move(other.name_)),
      hasOutput_(other.hasOutput_), inputLatencyFrames_(other.inputLatencyFrames_),
      mapping_(std::exchange(other.mapping_, nullptr)), bytes_(std::exchange(other.bytes_, 0)) {}
RemoteDeviceTransport& RemoteDeviceTransport::operator=(RemoteDeviceTransport&& other) noexcept {
    if (this != &other) {
        if (mapping_)
            ::munmap(mapping_, bytes_);
        id_ = std::move(other.id_);
        uid_ = std::move(other.uid_);
        name_ = std::move(other.name_);
        hasOutput_ = other.hasOutput_;
        inputLatencyFrames_ = other.inputLatencyFrames_;
        mapping_ = std::exchange(other.mapping_, nullptr);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}
const std::string& RemoteDeviceTransport::id() const noexcept {
    return id_;
}
const std::string& RemoteDeviceTransport::uid() const noexcept {
    return uid_;
}
const std::string& RemoteDeviceTransport::name() const noexcept {
    return name_;
}
bool RemoteDeviceTransport::hasOutput() const noexcept {
    return hasOutput_;
}
uint32_t RemoteDeviceTransport::inputLatencyFrames() const noexcept {
    return inputLatencyFrames_;
}
DeviceAudioView RemoteDeviceTransport::view() const {
    return DeviceAudioView::attach(mapping_, bytes_);
}

RemoteDeviceTransport RemoteDeviceTransport::decode(xpc_object_t object) {
    if (!object || xpc_get_type(object) != XPC_TYPE_DICTIONARY ||
        xpc_dictionary_get_count(object) != 6)
        throw std::invalid_argument("Invalid device transport entry");
    auto id = textField(object, "id");
    auto uid = textField(object, "uid");
    auto name = textField(object, "name");
    validateIdentity(id, uid, name);
    auto output = xpc_dictionary_get_value(object, "hasOutput");
    auto latency = xpc_dictionary_get_value(object, "inputLatencyFrames");
    if (!latency || xpc_get_type(latency) != XPC_TYPE_UINT64 ||
        xpc_uint64_get_value(latency) > kMaximumInputLatencyFrames)
        throw std::invalid_argument("Invalid device input latency");
    auto memory = xpc_dictionary_get_value(object, "audio");
    if (!output || xpc_get_type(output) != XPC_TYPE_BOOL || !memory ||
        xpc_get_type(memory) != XPC_TYPE_SHMEM)
        throw std::invalid_argument("Invalid device transport output flag or memory");
    RemoteDeviceTransport result(std::move(id), std::move(uid), std::move(name),
                                 xpc_bool_get_value(output),
                                 static_cast<uint32_t>(xpc_uint64_get_value(latency)));
    result.bytes_ = xpc_shmem_map(memory, &result.mapping_);
    if (!result.bytes_ || !result.mapping_)
        throw std::runtime_error("Could not import device audio mapping");
    (void)result.view();
    return result;
}

DeviceTransportSnapshot DeviceTransportSnapshot::decode(xpc_object_t message) {
    if (!message || xpc_get_type(message) != XPC_TYPE_DICTIONARY ||
        xpc_dictionary_get_count(message) != 3 ||
        xpc_dictionary_get_uint64(message, "protocol") != kDeviceTransportProtocol ||
        !xpc_dictionary_get_uint64(message, "revision"))
        throw std::invalid_argument("Invalid device transport snapshot protocol or revision");
    auto array = xpc_dictionary_get_value(message, "devices");
    if (!array || xpc_get_type(array) != XPC_TYPE_ARRAY ||
        xpc_array_get_count(array) > kMaximumVirtualDevices)
        throw std::invalid_argument("Invalid device transport array");
    DeviceTransportSnapshot result{xpc_dictionary_get_uint64(message, "revision"), {}};
    result.devices.reserve(xpc_array_get_count(array));
    std::unordered_set<std::string> ids;
    std::set<std::array<uint64_t, 2>> mappings;
    for (size_t index = 0; index < xpc_array_get_count(array); ++index) {
        auto device = RemoteDeviceTransport::decode(xpc_array_get_value(array, index));
        if (!ids.insert(device.id()).second)
            throw std::invalid_argument("Duplicate device transport identity");
        if (!mappings.insert(device.view().mappingIdentity()).second)
            throw std::invalid_argument("Different devices cannot share an audio mapping");
        result.devices.push_back(std::move(device));
    }
    return result;
}
} // namespace loopz::macos
