// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/Identity.hpp>
#include "crypto_adapter/Crypto.hpp"
#include <array>
#include <charconv>
#include <sstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace aa::helper {
namespace {
struct Fd {
    explicit Fd(int native) : value(native) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int value;
    ~Fd() { if (value >= 0) ::close(value); }
};
core::Result<std::string> contents(const Fd& fd, std::size_t limit) {
    std::string text;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto n = ::read(fd.value, buffer.data(), buffer.size());
        if (n < 0) return Error{ErrorCode::helper_rejected};
        if (n == 0) return text;
        text.append(buffer.data(), static_cast<std::size_t>(n));
        if (text.size() > limit) return Error{ErrorCode::helper_rejected};
    }
}
core::Result<std::string> protected_file(const std::string& path, std::uint32_t owner) {
    if (path.empty() || path.front() != '/') return Error{ErrorCode::config_invalid};
    Fd directory{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    std::size_t begin = 1;
    while (begin < path.size()) {
        const auto slash = path.find('/', begin);
        const auto part = path.substr(begin, slash == std::string::npos ? slash : slash - begin);
        if (part.empty() || part == "." || part == "..") return Error{ErrorCode::config_invalid};
        const bool last = slash == std::string::npos;
        const bool fixture_ancestor = owner != 0 && !last && path.find('/', slash + 1) != std::string::npos;
        Fd next{::openat(directory.value, part.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                                                       O_NONBLOCK | (last ? 0 : O_DIRECTORY))};
        struct stat st{};
        if (next.value < 0 || ::fstat(next.value, &st) != 0 ||
            (st.st_uid != owner && (last || st.st_uid != 0)) ||
            (st.st_mode & (fixture_ancestor && st.st_uid == owner ? 0002 : 0022)) != 0 ||
            (last ? !S_ISREG(st.st_mode) : !S_ISDIR(st.st_mode)))
            return Error{ErrorCode::config_invalid};
        if (last) return contents(next, 128);
        ::close(directory.value);
        directory.value = next.value;
        next.value = -1;
        begin = slash + 1;
    }
    return Error{ErrorCode::config_invalid};
}
template<class T> bool decimal(const std::string& text, T& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
core::Result<std::uint64_t> starttime(int proc) {
    Fd fd{::openat(proc, "stat", O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (fd.value < 0) return Error{ErrorCode::helper_rejected};
    const auto text = contents(fd, 8192);
    if (!text) return text.error();
    const auto end = text.value().rfind(')');
    if (end == std::string::npos) return Error{ErrorCode::helper_rejected};
    std::istringstream fields{text.value().substr(end + 1)};
    std::string field;
    for (int index = 3; index <= 22; ++index) {
        if (!(fields >> field) || (index == 3 && (field == "Z" || field == "X")))
            return Error{ErrorCode::helper_rejected};
    }
    std::uint64_t value{};
    if (!decimal(field, value) || !value) return Error{ErrorCode::helper_rejected};
    return value;
}
bool process_uids(const Fd& proc, std::uint32_t expected) {
    Fd fd{::openat(proc.value, "status", O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (fd.value < 0) return false;
    const auto text = contents(fd, 16384);
    if (!text) return false;
    std::istringstream lines{text.value()};
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.starts_with("Uid:")) continue;
        std::istringstream values{line.substr(4)};
        for (int i = 0; i < 4; ++i) {
            std::uint32_t uid{};
            if (!(values >> uid) || uid != expected) return false;
        }
        return true;
    }
    return false;
}
}
core::Result<IdentityPolicy> IdentityPolicy::load(const std::string& uid_file,
                                                const std::string& digest_file,
                                                std::uint32_t owner) {
    auto uid = protected_file(uid_file, owner);
    auto digest = protected_file(digest_file, owner);
    if (!uid || !digest) return Error{ErrorCode::config_invalid};
    if (!uid.value().empty() && uid.value().back() == '\n') uid.value().pop_back();
    if (!digest.value().empty() && digest.value().back() == '\n') digest.value().pop_back();
    std::uint32_t parsed{};
    if (!decimal(uid.value(), parsed) || digest.value().size() != 64)
        return Error{ErrorCode::config_invalid};
    for (const char c : digest.value())
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return Error{ErrorCode::config_invalid};
    return IdentityPolicy{parsed, digest.value()};
}
core::Result<ReceiverIdentity> inspect_process(const std::string& name, std::uint32_t uid,
                                              std::uint32_t pid, const IdentityPolicy& policy,
                                              std::chrono::steady_clock::time_point deadline) {
    if (uid != policy.session_uid || !pid || name.empty() || name.front() != ':')
        return Error{ErrorCode::helper_rejected};
    const auto path = "/proc/" + std::to_string(pid);
    Fd proc{::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    struct stat before{};
    if (proc.value < 0 || ::fstat(proc.value, &before) != 0 || before.st_uid != uid || !process_uids(proc, uid))
        return Error{ErrorCode::helper_rejected};
    const auto start = starttime(proc.value);
    // /proc/PID/exe is intentionally a kernel symlink. Follow only this leaf
    // relative to the anchored proc directory, and hash the opened executable.
    Fd exe{::openat(proc.value, "exe", O_RDONLY | O_CLOEXEC)};
    struct stat executable{};
    if (!start || exe.value < 0 || ::fstat(exe.value, &executable) != 0 || !S_ISREG(executable.st_mode))
        return Error{ErrorCode::helper_rejected};
    const auto digest = crypto_adapter::hash_executable(exe.value, deadline);
    const auto again = starttime(proc.value);
    Fd current{::openat(proc.value, "exe", O_RDONLY | O_CLOEXEC)};
    struct stat after{}, current_exe{};
    if (!digest || digest.value() != policy.digest || !again || again.value() != start.value() ||
        std::chrono::steady_clock::now() >= deadline ||
        ::fstat(proc.value, &after) != 0 || after.st_uid != uid || !process_uids(proc, uid) ||
        current.value < 0 || ::fstat(current.value, &current_exe) != 0 ||
        current_exe.st_dev != executable.st_dev || current_exe.st_ino != executable.st_ino)
        return Error{ErrorCode::helper_rejected};
    return ReceiverIdentity{name, uid, pid, start.value(), digest.value()};
}
} // namespace aa::helper
