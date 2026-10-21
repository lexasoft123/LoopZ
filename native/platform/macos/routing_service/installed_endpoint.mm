#import <Foundation/Foundation.h>
#include <loopz/macos/routing_service.hpp>
#include <Security/Security.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace loopz::macos {
namespace {
class Descriptor {
  public:
    explicit Descriptor(int descriptor) : value_(descriptor) {
        if (value_ < 0)
            throw std::runtime_error("Installed routing service configuration unavailable");
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() {
        ::close(value_);
    }
    int get() const noexcept {
        return value_;
    }

  private:
    int value_;
};
void trusted(int descriptor, bool directory) {
    struct stat status{};
    if (fstat(descriptor, &status) || status.st_uid != 0 || (status.st_mode & 0022) ||
        (directory ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode)))
        throw std::runtime_error("Routing service configuration must be root-owned and protected");
}
NSData* readConfiguration(int descriptor) {
    struct stat status{};
    if (fstat(descriptor, &status) || !S_ISREG(status.st_mode) || status.st_size > 16384)
        throw std::runtime_error("Invalid routing service configuration file");
    std::array<char, 16385> bytes{};
    size_t count = 0;
    for (;;) {
        const auto read = ::read(descriptor, bytes.data() + count, bytes.size() - count);
        if (read < 0 && errno == EINTR)
            continue;
        if (read < 0)
            throw std::runtime_error("Cannot read routing service configuration");
        if (!read)
            break;
        count += static_cast<size_t>(read);
        if (count == bytes.size())
            throw std::runtime_error("Routing service configuration too large");
    }
    return [NSData dataWithBytes:bytes.data() length:count];
}
RoutingServiceEndpoint parseConfiguration(NSData* data) {
    id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![object isKindOfClass:[NSDictionary class]] || [object count] != 2)
        throw std::runtime_error("Invalid installed routing service configuration");
    id version = object[@"protocol"];
    id requirement = object[@"brokerRequirement"];
    if (![version isKindOfClass:[NSNumber class]] ||
        CFGetTypeID((__bridge CFTypeRef)version) == CFBooleanGetTypeID() ||
        [version doubleValue] != kDeviceAudioProtocol ||
        ![requirement isKindOfClass:[NSString class]] || ![requirement length] ||
        [requirement lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 4096)
        throw std::runtime_error("Invalid routing service protocol or code pin");
    const char* pin = [requirement UTF8String];
    if (!pin || std::char_traits<char>::length(pin) !=
                    [requirement lengthOfBytesUsingEncoding:NSUTF8StringEncoding])
        throw std::runtime_error("Invalid installed broker code pin");
    SecRequirementRef compiled = nullptr;
    const auto status = SecRequirementCreateWithString((__bridge CFStringRef)requirement,
                                                       kSecCSDefaultFlags, &compiled);
    if (compiled)
        CFRelease(compiled);
    if (status != errSecSuccess)
        throw std::runtime_error("Invalid broker signing requirement syntax");
    RoutingServiceEndpoint endpoint;
    endpoint.serverRequirement = pin;
    return endpoint;
}
class StaticCode {
  public:
    explicit StaticCode(const std::filesystem::path& bundle) {
        const auto url = [NSURL fileURLWithPath:@(bundle.c_str()) isDirectory:YES];
        if (SecStaticCodeCreateWithPath((__bridge CFURLRef)url, kSecCSDefaultFlags, &value_) !=
            errSecSuccess)
            throw std::runtime_error("Cannot inspect routing driver bundle signature");
    }
    ~StaticCode() {
        CFRelease(value_);
    }
    StaticCode(const StaticCode&) = delete;
    StaticCode& operator=(const StaticCode&) = delete;
    SecStaticCodeRef get() const noexcept {
        return value_;
    }

  private:
    SecStaticCodeRef value_{nullptr};
};
} // namespace
RoutingServiceEndpoint installedRoutingServiceEndpoint() {
    @autoreleasepool {
        // Resolve every component without following links. A writable parent
        // must not turn a trusted-looking file into an attacker-selected pin.
        Descriptor library(open("/Library", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        trusted(library.get(), true);
        Descriptor support(openat(library.get(), "Application Support",
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        trusted(support.get(), true);
        Descriptor folder(
            openat(support.get(), "LoopZ", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        trusted(folder.get(), true);
        Descriptor file(
            openat(folder.get(), "routing-service.json", O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        trusted(file.get(), false);
        return parseConfiguration(readConfiguration(file.get()));
    }
}
RoutingServiceEndpoint bundledRoutingServiceEndpoint(const void* imageSymbol) {
    @autoreleasepool {
        Dl_info image{};
        if (!imageSymbol || !dladdr(imageSymbol, &image) || !image.dli_fname)
            throw std::runtime_error("Cannot locate loaded routing driver image");
        const std::filesystem::path binary(image.dli_fname);
        const auto contents = binary.parent_path().parent_path();
        const auto bundle = contents.parent_path();
        if (!binary.is_absolute() || binary.parent_path().filename() != "MacOS" ||
            contents.filename() != "Contents" || bundle.extension() != ".driver")
            throw std::runtime_error("Routing trust pin must belong to a loaded driver bundle");
        StaticCode code(bundle);
        SecRequirementRef identifier = nullptr;
        const auto compiled = SecRequirementCreateWithString(
            CFSTR("identifier \"com.lexasoft.loopz.driver.routing\" or "
                  "identifier \"com.lexasoft.loopz.driver\""),
            kSecCSDefaultFlags, &identifier);
        if (compiled != errSecSuccess)
            throw std::runtime_error("Cannot construct routing driver signature requirement");
        const auto valid = SecStaticCodeCheckValidity(
            code.get(), kSecCSStrictValidate | kSecCSCheckAllArchitectures | kSecCSSingleThreaded,
            identifier);
        CFRelease(identifier);
        if (valid != errSecSuccess)
            throw std::runtime_error("Routing driver bundle signature is invalid");
        Descriptor resources(open((contents / "Resources").c_str(),
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        Descriptor file(openat(resources.get(), "routing-service.json",
                               O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        NSData* data = readConfiguration(file.get());
        if (SecCodeValidateFileResource(code.get(), CFSTR("Resources/routing-service.json"),
                                        (__bridge CFDataRef)data,
                                        kSecCSDefaultFlags) != errSecSuccess)
            throw std::runtime_error("Routing trust pin does not match the bundle resource seal");
        return parseConfiguration(data);
    }
}
} // namespace loopz::macos
