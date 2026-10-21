#include <loopz/macos/route_store.hpp>
#include <loopz/macos/local_file.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace loopz::macos {
namespace {
struct File {
    int descriptor{-1};
    std::string temporaryPath;
    File() = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() {
        if (descriptor >= 0)
            ::close(descriptor);
        if (!temporaryPath.empty())
            ::unlink(temporaryPath.c_str());
    }
};

[[noreturn]] void ioError(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void validatePath(const std::filesystem::path& path) {
    const auto bytes = path.string();
    if (bytes.empty() || path.filename().empty() || bytes.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid routing configuration path");
}

std::string readFile(const std::filesystem::path& path,
                     size_t maximumBytes = kMaximumRouteFileBytes) {
    File file;
    file.descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (file.descriptor < 0)
        ioError("Open routing configuration");
    struct stat attributes{};
    if (::fstat(file.descriptor, &attributes))
        ioError("Inspect routing configuration");
    if (!S_ISREG(attributes.st_mode) || attributes.st_size < 0 ||
        static_cast<uint64_t>(attributes.st_size) > maximumBytes)
        throw std::runtime_error("Configuration is not a regular file within its size limit");
    std::string result;
    result.reserve(static_cast<size_t>(attributes.st_size));
    std::array<char, 8192> bytes{};
    for (;;) {
        const auto count = ::read(file.descriptor, bytes.data(), bytes.size());
        if (count < 0) {
            if (errno == EINTR)
                continue;
            ioError("Read routing configuration");
        }
        if (count == 0)
            return result;
        if (result.size() + static_cast<size_t>(count) > maximumBytes)
            throw std::runtime_error("Configuration grew beyond its size limit");
        result.append(bytes.data(), static_cast<size_t>(count));
    }
}

void writeAtomic(const std::filesystem::path& path, std::string_view bytes,
                 bool exclusive = false) {
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    if (std::filesystem::create_directories(parent)) {
        if (::chmod(parent.c_str(), 0700))
            ioError("Protect routing configuration directory");
    }
    const auto pattern = (parent / (".loopz-" + path.filename().string() + "-XXXXXX")).string();
    std::vector<char> temporary(pattern.begin(), pattern.end());
    temporary.push_back('\0');
    File file;
    file.descriptor = ::mkstemp(temporary.data());
    if (file.descriptor < 0)
        ioError("Create routing configuration replacement");
    file.temporaryPath = temporary.data();
    if (::fchmod(file.descriptor, 0600) || ::fcntl(file.descriptor, F_SETFD, FD_CLOEXEC) < 0)
        ioError("Protect routing configuration replacement");
    size_t written = 0;
    while (written < bytes.size()) {
        const auto count = ::write(file.descriptor, bytes.data() + written, bytes.size() - written);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            ioError("Write routing configuration replacement");
        }
        if (count == 0)
            throw std::runtime_error("Routing configuration write made no progress");
        written += static_cast<size_t>(count);
    }
    if (::fsync(file.descriptor))
        ioError("Flush routing configuration replacement");
    if (exclusive) {
        // Both names refer to the fully written, protected inode. link() fails
        // atomically if another creator published a primary in the meantime.
        if (::link(file.temporaryPath.c_str(), path.c_str()))
            ioError("Create initial routing configuration");
        return; // File's destructor removes only the temporary name.
    }
    if (::rename(file.temporaryPath.c_str(), path.c_str()))
        ioError("Publish routing configuration replacement");
    file.temporaryPath.clear();
}
} // namespace

std::string readLocalFile(const std::filesystem::path& path, size_t maximumBytes) {
    validatePath(path);
    return readFile(path, maximumBytes);
}
void writeLocalFileAtomic(const std::filesystem::path& path, std::string_view bytes) {
    validatePath(path);
    writeAtomic(path, bytes);
}

LoadedRouteDocument loadRouteDocumentFile(const std::filesystem::path& path) {
    validatePath(path);
    std::string primaryError;
    try {
        return {decodeRouteDocumentJSON(readFile(path)), false, {}};
    } catch (const UnsupportedRouteVersion&) {
        throw;
    } catch (const std::exception& error) {
        primaryError = error.what();
    }
    try {
        return {decodeRouteDocumentJSON(readFile(path.string() + ".backup")), true, primaryError};
    } catch (const UnsupportedRouteVersion&) {
        throw;
    } catch (const std::exception&) {
        throw std::runtime_error("Routing configuration and its backup could not be loaded: " +
                                 primaryError);
    }
}

void saveRouteDocumentFile(const std::filesystem::path& path, const RouteDocument& document) {
    validatePath(path);
    // Validate/encode before any file mutation, including the backup.
    const auto encoded = encodeRouteDocumentJSON(document);
    const auto backup = std::filesystem::path(path.string() + ".backup");
    // A newer controller may have left its only readable document in the
    // backup. Do not replace that version with an older schema on save.
    if (std::filesystem::symlink_status(backup).type() != std::filesystem::file_type::not_found) {
        const auto previousBackup = readFile(backup);
        try {
            (void)decodeRouteDocumentJSON(previousBackup);
        } catch (const std::invalid_argument&) {
            // A corrupt backup can be replaced by a valid primary below.
        }
    }
    const auto state = std::filesystem::symlink_status(path);
    if (state.type() != std::filesystem::file_type::not_found) {
        const auto previous = readFile(path);
        bool valid = true;
        try {
            (void)decodeRouteDocumentJSON(previous);
        } catch (const std::invalid_argument&) {
            valid = false;
        }
        if (valid)
            writeAtomic(backup, previous);
    }
    writeAtomic(path, encoded);
}
void createRouteDocumentFile(const std::filesystem::path& path, const RouteDocument& document) {
    validatePath(path);
    const auto encoded = encodeRouteDocumentJSON(document);
    const auto backup = std::filesystem::path(path.string() + ".backup");
    if (std::filesystem::symlink_status(backup).type() != std::filesystem::file_type::not_found)
        throw std::runtime_error(
            "A saved backup exists; recover routing before creating a document");
    writeAtomic(path, encoded, true);
}
} // namespace loopz::macos
