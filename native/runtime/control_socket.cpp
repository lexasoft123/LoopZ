#include <loopz/runtime/control_socket.hpp>
#include <loopz/runtime/control.hpp>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace loopz::runtime {
namespace {
constexpr size_t limit = kMaximumControlRequestBytes;
volatile sig_atomic_t stopping = 0;
void stop(int) {
    stopping = 1;
}
struct FD {
    int value{-1};
    ~FD() {
        if (value >= 0)
            ::close(value);
    }
};
std::runtime_error error(const char* text) {
    return std::runtime_error(text);
}
sockaddr_un address(const std::filesystem::path& path) {
    sockaddr_un result{};
    result.sun_family = AF_UNIX;
    auto name = path.string();
    if (name.empty() || name.size() >= sizeof(result.sun_path) ||
        name.find('\0') != std::string::npos)
        throw error("Control socket path is invalid or too long");
    std::memcpy(result.sun_path, name.c_str(), name.size() + 1);
    return result;
}
void timeout(int fd) {
    timeval duration{2, 0};
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &duration, sizeof(duration)) ||
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &duration, sizeof(duration)))
        throw error("Cannot set control socket deadline");
#ifdef SO_NOSIGPIPE
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
}
void waitReady(int fd, short events, std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0 || stopping)
            throw error("Control operation exceeded its deadline");
        pollfd item{fd, events, 0};
        const auto ready = ::poll(&item, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || !(item.revents & events))
            throw error("Control operation failed or timed out");
        return;
    }
}
void send(int fd, std::string_view bytes) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    size_t offset = 0;
    while (offset < bytes.size()) {
        waitReady(fd, POLLOUT, deadline);
#ifdef MSG_NOSIGNAL
        auto n = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
#else
        auto n = ::send(fd, bytes.data() + offset, bytes.size() - offset, 0);
#endif
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            throw error("Control write failed or timed out");
        offset += static_cast<size_t>(n);
    }
}
std::string receive(int fd, size_t maximumBytes = kMaximumControlRequestBytes) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::string bytes;
    std::array<char, 8192> block{};
    for (;;) {
        waitReady(fd, POLLIN, deadline);
        auto n = ::recv(fd, block.data(), block.size(), 0);
        if (n < 0 && errno == EINTR && !stopping)
            continue;
        if (n <= 0)
            throw error("Control read failed or timed out");
        bytes.append(block.data(), static_cast<size_t>(n));
        if (bytes.size() > maximumBytes)
            throw error("Control frame exceeds its size limit");
        auto newline = bytes.find('\n');
        if (newline != std::string::npos) {
            if (newline + 1 != bytes.size())
                throw error("Exactly one control frame is permitted");
            bytes.resize(newline);
            return bytes;
        }
    }
}
bool sameUser(int fd) {
#ifdef __linux__
    ucred peer{};
    socklen_t length = sizeof(peer);
    return !::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) && peer.uid == ::geteuid();
#else
    uid_t uid{};
    gid_t gid{};
    return !::getpeereid(fd, &uid, &gid) && uid == ::geteuid();
#endif
}
void privateDirectory(const std::filesystem::path& path) {
    if (std::filesystem::create_directories(path) && ::chmod(path.c_str(), 0700))
        throw error("Cannot protect control directory");
    struct stat st{};
    if (::lstat(path.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() ||
        (st.st_mode & 0077))
        throw error("Control directory must be private and owned by the current user");
}
} // namespace
std::filesystem::path defaultSocketPath() {
    const auto* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime)
        throw error("XDG_RUNTIME_DIR is required; alternatively supply --socket");
    return std::filesystem::path(runtime) / "loopz/control.sock";
}
std::string requestSocket(const std::filesystem::path& path, std::string_view request) {
    if (request.size() >= limit || request.find('\n') != std::string_view::npos)
        throw error("Control request must be one bounded JSON line");
    auto addr = address(path);
    FD fd{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (fd.value < 0)
        throw error("Cannot create control client");
    timeout(fd.value);
    if (::connect(fd.value, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)))
        throw error("Cannot connect to LoopZ Engine");
    if (!sameUser(fd.value))
        throw error("LoopZ Engine peer belongs to another user");
    send(fd.value, std::string(request) + "\n");
    return receive(fd.value, kMaximumControlResponseBytes);
}
int serveSocket(const std::filesystem::path& path, const std::filesystem::path& state) {
    auto addr = address(path);
    privateDirectory(path.parent_path());
    privateDirectory(state.parent_path().empty() ? std::filesystem::path(".")
                                                 : state.parent_path());
    FD lock{
        ::open((path.string() + ".lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600)};
    if (lock.value < 0 || ::flock(lock.value, LOCK_EX | LOCK_NB))
        throw error("Another LoopZ Engine owns this socket");
    FD stateLock{::open((state.string() + ".lock").c_str(),
                        O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600)};
    if (stateLock.value < 0 || ::flock(stateLock.value, LOCK_EX | LOCK_NB))
        throw error("Cannot exclusively own saved routing");
    FD listener{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (listener.value < 0)
        throw error("Cannot create control listener");
    struct stat old{};
    if (!::lstat(path.c_str(), &old) && (!S_ISSOCK(old.st_mode) || old.st_uid != ::geteuid()))
        throw error("Refusing to replace an unrelated control path");
    ::unlink(path.c_str());
    if (::bind(listener.value, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ||
        ::chmod(path.c_str(), 0600) || ::listen(listener.value, 8))
        throw error("Cannot bind private control socket");
    struct Cleanup {
        const std::filesystem::path& path;
        ~Cleanup() {
            ::unlink(path.c_str());
        }
    } cleanup{path};
    ::signal(SIGTERM, stop);
    ::signal(SIGINT, stop);
    Control control(state);
    auto nextPoll = std::chrono::steady_clock::now();
    while (!stopping) {
        pollfd pending{listener.value, POLLIN, 0};
        int ready = ::poll(&pending, 1, 100);
        if (ready < 0 && errno != EINTR)
            throw error("Control poll failed");
        if (ready > 0 && (pending.revents & POLLIN)) {
            FD client{::accept(listener.value, nullptr, nullptr)};
            if (client.value >= 0 && sameUser(client.value)) {
                try {
                    timeout(client.value);
                    auto request = parseControlJSON(receive(client.value));
                    auto response = control.request(request).dump();
                    if (response.size() >= kMaximumControlResponseBytes)
                        throw error("Control response exceeds size limit");
                    send(client.value, response + "\n");
                } catch (const std::exception& failure) {
                    try {
                        send(client.value, JSON{{"version", 1},
                                                {"ok", false},
                                                {"code", "invalid-request"},
                                                {"detail", failure.what()}}
                                                   .dump() +
                                               "\n");
                    } catch (...) {
                    }
                }
            }
        }
        if (std::chrono::steady_clock::now() >= nextPoll) {
            control.poll();
            nextPoll = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
    }
    return 0;
}
} // namespace loopz::runtime
