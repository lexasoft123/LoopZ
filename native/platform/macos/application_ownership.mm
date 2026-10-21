#include <loopz/macos/application_ownership.hpp>
#import <AppKit/AppKit.h>
#include <libproc.h>
#include <dlfcn.h>
#include <unistd.h>

#include <array>
#include <limits>
#include <map>
#include <string_view>

namespace loopz::macos {
namespace {
using Responsible = pid_t (*)(pid_t);
Responsible responsibility() noexcept {
    static const auto result = [] {
        static void* library =
            dlopen("/usr/lib/system/libquarantine.dylib", RTLD_LOCAL | RTLD_LAZY);
        const auto function = reinterpret_cast<Responsible>(
            library ? dlsym(library, "responsibility_get_pid_responsible_for_pid") : nullptr);
        Dl_info image{};
        // This optional, undocumented SPI is observed in libquarantine on the
        // tested host. Never accept an implementation outside Apple's system
        // library directory. Missing SPI leaves explicit/path matches usable.
        if (!function || !dladdr(reinterpret_cast<const void*>(function), &image) ||
            !image.dli_fname || !std::string_view(image.dli_fname).starts_with("/usr/lib/system/"))
            return Responsible{};
        return function;
    }();
    return result;
}
struct Identity {
    ProcessInstance process;
    std::string executable;
    bool operator==(const Identity&) const = default;
};
std::optional<Identity> identity(pid_t pid) {
    if (pid <= 1)
        return std::nullopt;
    proc_bsdinfo before{}, after{};
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> path{};
    const auto read = [&](proc_bsdinfo& info) {
        return proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info) &&
               info.pbi_pid == static_cast<uint32_t>(pid) && info.pbi_uid == geteuid() &&
               info.pbi_start_tvsec && info.pbi_start_tvusec < 1000000 &&
               info.pbi_start_tvsec <= (UINT64_MAX - info.pbi_start_tvusec) / 1000000;
    };
    if (!read(before) || proc_pidpath(pid, path.data(), path.size()) <= 0 || !read(after) ||
        before.pbi_start_tvsec != after.pbi_start_tvsec ||
        before.pbi_start_tvusec != after.pbi_start_tvusec)
        return std::nullopt;
    return Identity{
        {static_cast<uint64_t>(pid), before.pbi_start_tvsec * 1000000 + before.pbi_start_tvusec},
        path.data()};
}
std::string canonicalPath(NSString* path) {
    // AppKit may retain /var or another bundle-path alias while libproc
    // reports its physical path. Compare the same filesystem identity spelling.
    const char* value = path.stringByStandardizingPath.stringByResolvingSymlinksInPath.UTF8String;
    return value ? value : "";
}
} // namespace
struct ApplicationOwnership::Impl {
    std::string application;
    std::vector<ApplicationProcessRoot> roots;
    std::vector<std::string> directories;
    std::map<pid_t, Identity> observed;
    std::optional<Identity> observe(pid_t pid) {
        if (const auto found = observed.find(pid); found != observed.end())
            return found->second;
        auto value = identity(pid);
        if (value)
            observed.emplace(pid, *value);
        return value;
    }
};
ApplicationOwnership::ApplicationOwnership(const std::string& application)
    : impl_(std::make_unique<Impl>()) {
    impl_->application = application;
    for (NSRunningApplication* app in
         [NSRunningApplication runningApplicationsWithBundleIdentifier:@(application.c_str())]) {
        if (![app.bundleIdentifier isEqualToString:@(application.c_str())])
            continue;
        const auto value = impl_->observe(app.processIdentifier);
        if (!value || app.terminated ||
            canonicalPath(@(value->executable.c_str())) != canonicalPath(app.executableURL.path))
            continue;
        impl_->roots.push_back({value->process, application});
        const auto directory = canonicalPath(app.bundleURL.path);
        if (!directory.empty() && directory.front() == '/' && directory.ends_with(".app"))
            impl_->directories.push_back(directory + "/");
    }
}
ApplicationOwnership::~ApplicationOwnership() = default;
const std::vector<ApplicationProcessRoot>& ApplicationOwnership::roots() const noexcept {
    return impl_->roots;
}
bool ApplicationOwnership::responsibilityAvailable() noexcept {
    return responsibility() != nullptr;
}
std::optional<AudioProcessAttribution> ApplicationOwnership::inspect(uint64_t object, int32_t pid,
                                                                     std::string application) {
    // Expanding a selected parent's cohort must not pull LoopZ's renderer
    // into its own tap, including another Engine process of the same identity.
    if (pid == getpid() || application == "com.lexasoft.loopz.engine")
        return std::nullopt;
    const auto value = impl_->observe(pid);
    if (!value)
        return std::nullopt;
    AudioProcessAttribution result{object, value->process, std::move(application), {}, {}, false};
    const auto path = canonicalPath(@(value->executable.c_str()));
    for (const auto& directory : impl_->directories)
        if (path.starts_with(directory)) {
            result.containingApplication = impl_->application;
            break;
        }
    NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
    result.independentApplication =
        app && app.activationPolicy != NSApplicationActivationPolicyProhibited &&
        canonicalPath(app.bundleURL.path).ends_with(".app");
    if (const auto function = responsibility();
        function && result.application != impl_->application &&
        result.containingApplication.empty() && !result.independentApplication) {
        const auto owner = function(pid);
        if (const auto responsible = impl_->observe(owner))
            result.responsible = responsible->process;
    }
    return result;
}
bool ApplicationOwnership::coherent(std::span<const AudioProcessAttribution> selected) const {
    // Unrelated process churn must not tear down this application's capture.
    // Only selected processes and the ownership evidence actually used to
    // include them participate in this final snapshot check.
    for (const auto& process : selected) {
        if (process.process.id > static_cast<uint64_t>(std::numeric_limits<pid_t>::max()))
            return false;
        const auto pid = static_cast<pid_t>(process.process.id);
        const auto before = impl_->observed.find(pid);
        if (before == impl_->observed.end() || identity(pid) != std::optional(before->second))
            return false;
        if (process.application != impl_->application &&
            process.containingApplication != impl_->application) {
            const auto function = responsibility();
            if (!function || !process.responsible ||
                process.responsible->id > static_cast<uint64_t>(std::numeric_limits<pid_t>::max()))
                return false;
            const auto owner = static_cast<pid_t>(process.responsible->id);
            if (function(pid) != owner)
                return false;
            const auto known = impl_->observed.find(owner);
            if (known == impl_->observed.end() || identity(owner) != std::optional(known->second))
                return false;
        }
    }
    return true;
}
} // namespace loopz::macos
