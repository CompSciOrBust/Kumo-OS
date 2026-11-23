#include "ftp_server.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <optional>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace ftp {
namespace {

constexpr int ControlTimeout = 300000;
constexpr int DataTimeout = 15000;
constexpr size_t MaxLine = 512;
constexpr size_t MaxPath = 240;
constexpr size_t BufferSize = 8192;
constexpr std::string_view StagingEntry = "HZFTPTMP";

std::string VolumePath(const Volume &volume, std::string_view relative) {
    const auto separator = volume.root.ends_with('/') ? "" : "/";
    return volume.root + separator + std::string(relative);
}

std::int64_t MonotonicMs() {
#ifdef __SWITCH__
    return armTicksToNs(svcGetSystemTick()) / 1000000;
#else
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
#endif
}

class Fd {
    int fd = -1;
public:
    Fd() = default;
    explicit Fd(int value) : fd(value) {}
    ~Fd() { Reset(); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&other) : fd(std::exchange(other.fd, -1)) {}
    Fd &operator=(Fd &&other) {
        Reset(std::exchange(other.fd, -1));
        return *this;
    }
    int Get() const { return fd; }
    explicit operator bool() const { return fd >= 0; }
    bool Reset(int value = -1) {
        const int old = std::exchange(fd, value);
        return old < 0 || close(old) == 0;
    }
};

sockaddr *Address(sockaddr_in &address) { return reinterpret_cast<sockaddr *>(&address); }

sockaddr_in Endpoint(in_addr address, unsigned port) {
    sockaddr_in endpoint{};
#if defined(__SWITCH__) || defined(__APPLE__)
    endpoint.sin_len = sizeof(endpoint);
#endif
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    endpoint.sin_addr = address;
    return endpoint;
}

Fd Listen(sockaddr_in address, int backlog) {
    Fd fd(socket(AF_INET, SOCK_STREAM, 0));
    if (!fd) return {};
    const int enable = 1;
    if (setsockopt(fd.Get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0 ||
        bind(fd.Get(), Address(address), sizeof(address)) < 0 || listen(fd.Get(), backlog) < 0) return {};
    return fd;
}

bool SendAll(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t count = send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        data.remove_prefix(count);
    }
    return true;
}

bool WriteAll(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t count = write(fd, data.data(), data.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        data.remove_prefix(count);
    }
    return true;
}

std::string Upper(std::string_view value) {
    std::string result(value);
    for (char &c : result) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    return result;
}

bool PrivateEntry(const Volume &volume, std::string_view entry) {
    const auto name = Upper(entry);
    return name == StagingEntry || name == Upper(volume.private_entry);
}

bool Number(std::string_view input, unsigned &number) {
    const auto result = std::from_chars(input.data(), input.data() + input.size(), number);
    return result.ec == std::errc{} && result.ptr == input.data() + input.size();
}

bool SafeComponent(std::string_view part) {
    if (part.empty() || part.back() == ' ' || part.back() == '.') return false;
    for (unsigned char c : part) {
        if (c < 32 || c == 127 || std::string_view("/\\:\"<>|?*").find(c) != std::string_view::npos) return false;
    }
    return true;
}

std::optional<std::string> Resolve(std::string_view cwd, std::string_view input) {
    std::string path = input.starts_with('/') ? "/" : std::string(cwd);
    while (!input.empty()) {
        const auto slash = input.find('/');
        const auto part = input.substr(0, slash);
        if (part == "..") {
            path.resize(std::max<size_t>(1, path.rfind('/')));
        } else if (!part.empty() && part != ".") {
            if (!SafeComponent(part)) return {};
            if (path.size() > 1) path += '/';
            path += part;
            if (path.size() > MaxPath) return {};
        }
        if (slash == std::string_view::npos) break;
        input.remove_prefix(slash + 1);
    }
    return path;
}

// Parse Telnet framing before interpreting the FTP command line. Refuse options
// and consume subnegotiations, including the in-band IP and DM used with ABOR.
class Control {
    enum class Telnet { Text, Command, Option, Subnegotiation, Subcommand };
    Telnet state = Telnet::Text;
    unsigned char option = 0;
    std::array<unsigned char, 1024> bytes{};
    size_t offset = 0, length = 0;
    bool invalid = false;
public:
    std::string line;
    bool ready = false;

    bool Read(int fd, bool receive) {
        while (!ready) {
            if (offset == length) {
                if (!receive) break;
                const ssize_t count = recv(fd, bytes.data(), bytes.size(), MSG_DONTWAIT);
                if (count < 0 && errno == EINTR) continue;
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (count <= 0) return false;
                offset = 0;
                length = count;
                receive = false;
            }
            const auto c = bytes[offset++];
            switch (state) {
                case Telnet::Text:
                    if (c == 255) { state = Telnet::Command; continue; }
                    break;
                case Telnet::Command:
                    state = Telnet::Text;
                    if (c == 255) break;
                    if (c == 250) state = Telnet::Subnegotiation;
                    if (c >= 251 && c <= 254) { option = c; state = Telnet::Option; }
                    if (c == 244 || c == 242) { line.clear(); invalid = false; }
                    continue;
                case Telnet::Option: {
                    state = Telnet::Text;
                    const char response[]{char(255), char(option == 251 ? 254 : 252), char(c)};
                    if ((option == 251 || option == 253) && !SendAll(fd, {response, sizeof(response)})) return false;
                    continue;
                }
                case Telnet::Subnegotiation:
                    if (c == 255) state = Telnet::Subcommand;
                    continue;
                case Telnet::Subcommand:
                    state = c == 240 ? Telnet::Text : Telnet::Subnegotiation;
                    continue;
            }
            if (c == '\n') {
                if (invalid || line.empty() || line.back() != '\r') line.clear();
                else line.pop_back();
                ready = true;
                invalid = false;
            } else if (c == 0 || line.size() >= MaxLine - 1 ||
                       (!line.empty() && line.back() == '\r')) {
                invalid = true;
            } else if (!invalid) {
                line += static_cast<char>(c);
            }
        }
        return true;
    }

    std::string Take() {
        ready = false;
        return std::exchange(line, {});
    }
};

enum class Transfer { Complete, Connection, Network, File, Aborted };

struct VolumeMutex {
    Mutex value{};
    void lock() { mutexLock(&value); }
    void unlock() { mutexUnlock(&value); }
};

using VolumeGuard = std::unique_lock<VolumeMutex>;

class Session {
    const Config &config;
    VolumeMutex *volume_locks;
    int control;
    sockaddr_in peer, local;
    void (*log)(const char *);
    Control input;
    Fd passive;
    std::optional<sockaddr_in> active;
    std::string cwd = "/";
    std::string rename_from;
    bool user_matches = false, expect_password = false, logged_in = false;
    bool binary = false, epsv_all = false, alive = true;
    bool fact_type = true, fact_size = true;
    unsigned failed_logins = 0;

    void Reply(std::string_view response) {
        if (!alive) return;
        std::string wire;
        // Double literal IAC bytes in pathnames echoed over the Telnet channel.
        for (char c : response) {
            wire += c;
            if (static_cast<unsigned char>(c) == 255) wire += c;
        }
        wire += "\r\n";
        alive = SendAll(control, wire);
    }

    void PathReply(const std::string &path) {
        std::string message = "257 \"";
        for (char c : path) {
            message += c;
            if (c == '"') message += c;
        }
        Reply(message + "\"");
    }

    struct Location {
        std::string disk;
        const Volume *volume = nullptr;
        bool volume_root = false;
    };

    std::optional<Location> Locate(std::string_view path) const {
        if (path == "/") return Location{};
        const auto slash = path.find('/', 1);
        const auto name = Upper(path.substr(1, slash == path.npos ? path.npos : slash - 1));
        for (const auto &volume : config.volumes) {
            if (Upper(volume.name) != name) continue;
            if (slash == path.npos) return Location{volume.root, &volume, true};
            const auto relative = path.substr(slash + 1);
            if (PrivateEntry(volume, relative.substr(0, relative.find('/')))) return {};
            return Location{VolumePath(volume, relative), &volume};
        }
        return {};
    }

    VolumeGuard LockVolume(const Location &location) const {
        if (!location.volume) return {};
        return VolumeGuard(volume_locks[location.volume - config.volumes.data()]);
    }

    bool Stat(const Location &location, struct stat &info) {
        if (location.disk.empty() || location.volume_root) {
            info = {};
            info.st_mode = S_IFDIR;
            return true;
        }
        const auto lock = LockVolume(location);
        return stat(location.disk.c_str(), &info) == 0;
    }

    void CheckWorkingDirectory() {
        struct stat info{};
        const auto location = Locate(cwd);
        if (!location || !Stat(*location, info) || !S_ISDIR(info.st_mode)) cwd = "/";
    }

    void ResetData() {
        passive.Reset();
        active.reset();
    }

    void SetPassive(bool extended) {
        ResetData();
        for (unsigned port = config.passive_min_port; port <= config.passive_max_port; ++port) {
            passive = Listen(Endpoint(local.sin_addr, port), 4);
            if (!passive) continue;
            if (fcntl(passive.Get(), F_SETFL, O_NONBLOCK) < 0) {
                ResetData();
                break;
            }
            if (extended) {
                Reply("229 Entering Extended Passive Mode (|||" + std::to_string(port) + "|)");
            } else {
                const auto ip = ntohl(config.passive_address ? config.passive_address : local.sin_addr.s_addr);
                Reply("227 Entering Passive Mode (" + std::to_string(ip >> 24) + "," +
                      std::to_string(ip >> 16 & 255) + "," + std::to_string(ip >> 8 & 255) + "," +
                      std::to_string(ip & 255) + "," + std::to_string(port >> 8) + "," +
                      std::to_string(port & 255) + ")");
            }
            return;
        }
        Reply("425 Cannot listen for a data connection");
    }

    void SetActive(std::string_view arg, bool extended) {
        ResetData();
        in_addr address{};
        unsigned port = 0;
        if (extended) {
            if (arg.size() < 5 || arg.front() < 33 || arg.front() > 126 || arg.back() != arg.front()) {
                Reply("501 Invalid EPRT address");
                return;
            }
            const char delimiter = arg.front();
            arg.remove_prefix(1);
            const auto family = arg.find(delimiter);
            const auto host = family == arg.npos ? arg.npos : arg.find(delimiter, family + 1);
            if (host == arg.npos || arg.find(delimiter, host + 1) != arg.size() - 1) {
                Reply("501 Invalid EPRT address");
                return;
            }
            if (arg.substr(0, family) != "1") {
                Reply("522 Network protocol not supported, use (1)");
                return;
            }
            const std::string ip(arg.substr(family + 1, host - family - 1));
            if (inet_pton(AF_INET, ip.c_str(), &address) != 1 ||
                !Number(arg.substr(host + 1, arg.size() - host - 2), port)) {
                Reply("501 Invalid EPRT address");
                return;
            }
        } else {
            unsigned fields[6]{};
            for (size_t i = 0; i < std::size(fields); ++i) {
                const auto comma = arg.find(',');
                if ((comma == arg.npos) != (i == 5) || !Number(arg.substr(0, comma), fields[i]) || fields[i] > 255) {
                    Reply("501 Invalid PORT address");
                    return;
                }
                if (comma != arg.npos) arg.remove_prefix(comma + 1);
            }
            address.s_addr = htonl(fields[0] << 24 | fields[1] << 16 | fields[2] << 8 | fields[3]);
            port = fields[4] * 256 + fields[5];
        }
        if (address.s_addr != peer.sin_addr.s_addr || port < 1024 || port > 65535) {
            Reply("501 Data address must match the control peer and use an unprivileged port");
            return;
        }
        active = Endpoint(address, port);
        Reply("200 Data address accepted");
    }

    bool TransferControl(bool receive, Transfer &status) {
        if (!input.Read(control, receive)) {
            alive = false;
            status = Transfer::Aborted;
            return false;
        }
        if (!input.ready) return true;
        const auto command = Upper(input.line);
        if (command == "ABOR") {
            input.Take();
            status = Transfer::Aborted;
            return false;
        }
        if (command == "STAT") {
            input.Take();
            Reply("213 Transfer in progress");
        }
        return alive;
    }

    bool WaitData(int fd, short events, Transfer &status) {
        if (!alive || !TransferControl(false, status)) return false;
        const auto deadline = MonotonicMs() + DataTimeout;
        while (alive) {
            const auto remaining = deadline - MonotonicMs();
            if (remaining <= 0) return false;
            pollfd fds[]{{fd, events, 0}, {input.ready ? -1 : control, POLLIN, 0}};
            const int count = poll(fds, std::size(fds), static_cast<int>(remaining));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            if (fds[1].revents && !TransferControl(true, status)) return false;
            if (fds[0].revents) return true;
        }
        return false;
    }

    Fd OpenData(Transfer &status) {
        Fd listener = std::move(passive);
        auto target = std::exchange(active, {});
        if (listener) {
            // Reject the first foreign peer and require a fresh PASV/EPSV.
            if (!WaitData(listener.Get(), POLLIN, status)) return {};
            sockaddr_in source{};
            socklen_t length = sizeof(source);
            Fd data(accept(listener.Get(), Address(source), &length));
            if (!data || source.sin_addr.s_addr != peer.sin_addr.s_addr ||
                fcntl(data.Get(), F_SETFL, O_NONBLOCK) < 0) return {};
            return data;
        }

        if (!target && !epsv_all && ntohs(peer.sin_port) >= 1024) target = peer;
        if (!target) return {};
        Fd data(socket(AF_INET, SOCK_STREAM, 0));
        auto source = Endpoint(local.sin_addr, config.port - 1);
        const int reuse = 1;
        if (!data || setsockopt(data.Get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
            bind(data.Get(), Address(source), sizeof(source)) < 0 ||
            fcntl(data.Get(), F_SETFL, O_NONBLOCK) < 0) return {};
        if (connect(data.Get(), Address(*target), sizeof(*target)) < 0) {
            if (errno != EINPROGRESS || !WaitData(data.Get(), POLLOUT, status)) return {};
            int error = 0;
            socklen_t size = sizeof(error);
            if (getsockopt(data.Get(), SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error) return {};
        }
        return data;
    }

    bool SendData(int fd, std::string_view bytes, Transfer &status) {
        while (!bytes.empty()) {
            if (!WaitData(fd, POLLOUT, status)) return false;
            const ssize_t count = send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
            if (count <= 0) return false;
            bytes.remove_prefix(count);
        }
        return true;
    }

    template <typename Function>
    Transfer RunTransfer(Function perform) {
        if (!passive && !active && epsv_all) return Transfer::Connection;
        Reply("150 Opening data connection");
        Transfer status = Transfer::Connection;
        Fd fd = OpenData(status);
        if (fd) {
            status = Transfer::Network;
            perform(fd.Get(), status);
            if (!fd.Reset() && status == Transfer::Complete) status = Transfer::Network;
        }
        return status;
    }

    void Finish(Transfer status) {
        switch (status) {
            case Transfer::Complete: Reply("226 Transfer complete"); break;
            case Transfer::Connection: Reply("425 Cannot open data connection"); break;
            case Transfer::Network: Reply("426 Data connection closed; transfer aborted"); break;
            case Transfer::File: Reply("451 Local file error"); break;
            case Transfer::Aborted:
                Reply("426 Transfer aborted");
                Reply("226 Abort successful");
                break;
        }
    }

    void Retrieve(const Location &location) {
        Fd file;
        {
            const auto lock = LockVolume(location);
            file.Reset(open(location.disk.c_str(), O_RDONLY));
        }
        struct stat info{};
        if (!file || fstat(file.Get(), &info) < 0 || !S_ISREG(info.st_mode)) {
            Reply("550 File unavailable");
            return;
        }
        Finish(RunTransfer([&](int data, Transfer &status) {
            char buffer[BufferSize], ascii[BufferSize * 2];
            for (;;) {
                const ssize_t count = read(file.Get(), buffer, sizeof(buffer));
                if (count < 0 && errno == EINTR) continue;
                if (count < 0) { status = Transfer::File; return; }
                if (!count) { status = Transfer::Complete; return; }
                std::string_view bytes(buffer, count);
                if (!binary) {
                    size_t length = 0;
                    for (char c : bytes) {
                        if (c == '\n' || c == '\r') ascii[length++] = '\r';
                        ascii[length++] = c == '\r' ? '\0' : c;
                    }
                    bytes = {ascii, length};
                }
                if (!SendData(data, bytes, status)) return;
            }
        }));
    }

    bool Publish(const std::string &temporary, const std::string &path) {
        if (rename(temporary.c_str(), path.c_str()) == 0) return true;

        // FAT cannot rename over an existing file. Preserve it until the new
        // upload is in place, and restore it if the second rename fails.
        struct stat info{};
        const std::string backup = temporary + ".bak";
        if (stat(path.c_str(), &info) < 0 || !S_ISREG(info.st_mode) ||
            stat(backup.c_str(), &info) == 0 || errno != ENOENT ||
            rename(path.c_str(), backup.c_str()) < 0) return false;

        if (rename(temporary.c_str(), path.c_str()) == 0) {
            if (unlink(backup.c_str()) < 0) log("[ftpd] could not remove upload backup\n");
            return true;
        }
        if (rename(backup.c_str(), path.c_str()) < 0) {
            log(("[ftpd] restore " + backup + " to " + path + "\n").c_str());
        }
        return false;
    }

    static Fd CreateTemporary(const std::string &staging, std::string &path) {
        static std::atomic<unsigned> sequence{0};
        struct stat info{};
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            path = staging + "/" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
            if (stat((path + ".bak").c_str(), &info) == 0) continue;
            if (errno != ENOENT) break;
            Fd file(open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600));
            if (file) return file;
            if (errno != EEXIST) break;
        }
        return {};
    }

    static bool CopyFile(const std::string &source, int destination) {
        const Fd input_file(open(source.c_str(), O_RDONLY));
        if (!input_file) return false;
        char buffer[BufferSize];
        for (;;) {
            const ssize_t count = read(input_file.Get(), buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return count == 0;
            if (!WriteAll(destination, {buffer, static_cast<size_t>(count)})) return false;
        }
    }

    bool CommitUpload(const Location &location, const std::string &temporary, bool append) {
        const auto lock = LockVolume(location);
        const auto &path = location.disk;
        struct stat info{};
        const bool exists = stat(path.c_str(), &info) == 0;
        if ((exists && !S_ISREG(info.st_mode)) || (!exists && errno != ENOENT)) return false;
        if (!append || !exists) return Publish(temporary, path);

        // Merge only after receiving the suffix. Other sessions can upload while
        // this session waits for the data connection or for bytes from its peer.
        std::string combined;
        Fd file = CreateTemporary(VolumePath(*location.volume, StagingEntry), combined);
        if (!file) return false;
        const bool copied = CopyFile(path, file.Get()) && CopyFile(temporary, file.Get());
        const bool closed = file.Reset();
        if (copied && closed && Publish(combined, path)) {
            unlink(temporary.c_str());
            return true;
        }
        unlink(combined.c_str());
        return false;
    }

    void Store(const Location &location, bool append) {
        const auto staging = VolumePath(*location.volume, StagingEntry);
        struct stat info{};
        {
            const auto lock = LockVolume(location);
            const bool exists = stat(location.disk.c_str(), &info) == 0;
            if ((exists && !S_ISREG(info.st_mode)) || (!exists && errno != ENOENT)) {
                Reply("550 File unavailable");
                return;
            }
        }

        if (mkdir(staging.c_str(), 0700) < 0 &&
            (stat(staging.c_str(), &info) < 0 || !S_ISDIR(info.st_mode))) {
            Reply("451 Cannot prepare staging directory");
            return;
        }
        std::string temporary;
        Fd file = CreateTemporary(staging, temporary);
        if (!file) { Reply("451 Cannot create staging file"); return; }

        Transfer status = RunTransfer([&](int data, Transfer &result) {
            char buffer[BufferSize], ascii[BufferSize + 1];
            bool cr = false;
            for (;;) {
                if (!WaitData(data, POLLIN, result)) return;
                const ssize_t count = recv(data, buffer, sizeof(buffer), 0);
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
                if (count < 0) return;
                std::string_view bytes(buffer, count);
                if (!binary) {
                    size_t length = 0;
                    for (char c : bytes) {
                        if (cr) {
                            cr = false;
                            if (c == '\n') { ascii[length++] = '\n'; continue; }
                            ascii[length++] = '\r';
                            if (c == '\0') continue;
                        }
                        if (c == '\r') cr = true;
                        else ascii[length++] = c;
                    }
                    if (count == 0 && cr) ascii[length++] = '\r';
                    bytes = {ascii, length};
                }
                if (!WriteAll(file.Get(), bytes)) { result = Transfer::File; return; }
                if (count == 0) { result = Transfer::Complete; return; }
            }
        });
        if (!file.Reset() && status == Transfer::Complete) status = Transfer::File;
        if (status == Transfer::Complete && !CommitUpload(location, temporary, append)) status = Transfer::File;
        if (status != Transfer::Complete) unlink(temporary.c_str());
        Finish(status);
    }

    std::string Facts(const struct stat &info) const {
        std::string facts;
        const bool directory = S_ISDIR(info.st_mode);
        if (fact_type) facts = directory ? "type=dir;" : "type=file;";
        if (fact_size && !directory) facts += "size=" + std::to_string(info.st_size) + ";";
        return facts;
    }

    void Options(std::string_view arg) {
        const auto separator = arg.find(' ');
        if (Upper(arg.substr(0, separator)) != "MLST") {
            Reply("501 Unsupported option");
            return;
        }

        std::string_view list = separator == arg.npos ? std::string_view{} : arg.substr(separator + 1);
        bool type = false, size = false;
        while (!list.empty()) {
            const auto end = list.find(';');
            const auto name = list.substr(0, end);
            if (!end || end == list.npos || name.find_first_of(" \t") != name.npos) {
                Reply("501 Invalid MLST options");
                return;
            }
            type |= Upper(name) == "TYPE";
            size |= Upper(name) == "SIZE";
            list.remove_prefix(end + 1);
        }

        fact_type = type;
        fact_size = size;
        const std::string facts = std::string(type ? "type;" : "") + (size ? "size;" : "");
        Reply(facts.empty() ? "200 MLST OPTS" : "200 MLST OPTS " + facts);
    }

    std::string ListingLine(std::string_view command, std::string_view name, const struct stat &info) {
        if (command == "NLST") return std::string(name) + "\r\n";
        if (command == "MLSD") return Facts(info) + " " + std::string(name) + "\r\n";
        const char *mode = S_ISDIR(info.st_mode) ? "drwx------" : "-rw-------";
        char stamp[32] = "Jan 01  1970";
        // FAT timestamps can be unavailable. Keep the listing parseable in that case.
        std::tm date{};
        if (gmtime_r(&info.st_mtime, &date)) std::strftime(stamp, sizeof(stamp), "%b %d  %Y", &date);
        return std::string(mode) + " 1 ftp ftp " + std::to_string(info.st_size) + " " +
            stamp + " " + std::string(name) + "\r\n";
    }

    template <typename Emit>
    Transfer ListDirectory(DIR *directory, std::string_view command, const std::string &path,
                           const Location &location, Emit emit) {
        if (!directory) {
            // NLST enumerates file names. LIST and MLSD include directories.
            if (command == "NLST") return Transfer::Complete;
            struct stat info{};
            info.st_mode = S_IFDIR;
            for (const auto &volume : config.volumes) {
                if (!emit(ListingLine(command, volume.name, info))) return Transfer::Network;
            }
            return Transfer::Complete;
        }

        const auto prefix = location.disk.ends_with('/') ? location.disk : location.disk + "/";

        for (;;) {
            errno = 0;
            dirent *entry = readdir(directory);
            if (!entry) return errno ? Transfer::File : Transfer::Complete;
            const std::string_view name(entry->d_name);
            if (location.volume_root && PrivateEntry(*location.volume, name)) continue;
            if (!SafeComponent(name) || path.size() + name.size() + (path == "/" ? 0 : 1) > MaxPath) continue;
            struct stat item{};
            {
                const auto lock = LockVolume(location);
                if (stat((prefix + entry->d_name).c_str(), &item) < 0) {
                    if (errno == ENOENT) continue;
                    return Transfer::File;
                }
            }
            if (!S_ISREG(item.st_mode) && !S_ISDIR(item.st_mode)) continue;
            if (command == "NLST" && !S_ISREG(item.st_mode)) continue;
            const auto listed_name = command == "NLST" && path != cwd
                ? (path == "/" ? path : path + "/") + std::string(name) : std::string(name);
            if (!emit(ListingLine(command, listed_name, item))) return Transfer::Network;
        }
    }

    void List(std::string_view command, const std::string &path, const Location &location) {
        struct stat info{};
        if (!Stat(location, info)) { Reply("550 Path unavailable"); return; }
        if (S_ISREG(info.st_mode) && command != "MLSD") {
            const auto name = command == "NLST" ? std::string_view(path) :
                std::string_view(path).substr(path.rfind('/') + 1);
            if (command == "STAT") {
                Reply("213-File status");
                const auto line = ListingLine(command, name, info);
                Reply(" " + line.substr(0, line.size() - 2));
                Reply("213 End");
            } else {
                Finish(RunTransfer([&](int data, Transfer &status) {
                    if (SendData(data, ListingLine(command, name, info), status)) status = Transfer::Complete;
                }));
            }
            return;
        }
        DIR *directory = nullptr;
        if (!location.disk.empty()) {
            if (S_ISDIR(info.st_mode)) directory = opendir(location.disk.c_str());
            if (!directory) { Reply("550 Directory unavailable"); return; }
        }

        if (command == "STAT") {
            Reply("212-Directory status");
            const auto result = ListDirectory(directory, command, path, location, [&](const std::string &line) {
                Reply(" " + line.substr(0, line.size() - 2));
                return alive;
            });
            Reply(result == Transfer::Complete ? "212 End" : "212 End (listing incomplete)");
        } else {
            const Transfer result = RunTransfer([&](int data, Transfer &status) {
                const auto listed = ListDirectory(directory, command, path, location, [&](const std::string &line) {
                    return SendData(data, line, status);
                });
                if (status != Transfer::Aborted) status = listed;
            });
            Finish(result);
        }
        if (directory) closedir(directory);
    }

    void Login(std::string_view command, const std::string &arg) {
        const bool anonymous = config.user.empty() && config.password.empty();
        if (command == "USER") {
            if (arg.empty()) { Reply("501 Missing username"); return; }
            logged_in = false;
            user_matches = anonymous ? Upper(arg) == "ANONYMOUS" : arg == config.user;
            expect_password = true;
            cwd = "/";
            binary = false;
            epsv_all = false;
            ResetData();
            Reply("331 Password required");
        } else if (!expect_password) {
            Reply("503 Send USER first");
        } else {
            expect_password = false;
            logged_in = user_matches && (anonymous || arg == config.password);
            if (logged_in) {
                failed_logins = 0;
                Reply("230 Logged in");
            } else if (++failed_logins == 5) {
                Reply("421 Too many failed logins");
                alive = false;
            } else {
                Reply("530 Login incorrect");
            }
        }
    }

    void FileCommand(std::string_view command, std::string_view arg, const std::string &rename_source) {
        const auto path = Resolve(cwd, command == "CDUP" ? ".." : arg);
        if (!path) { Reply("553 Invalid pathname"); return; }
        const auto location = Locate(*path);
        if (!location) { Reply("550 Path unavailable"); return; }
        const auto &disk = location->disk;

        if (command == "LIST" || command == "NLST" || command == "MLSD" || command == "STAT") {
            List(command, *path, *location);
            return;
        }
        if (arg.empty() && command != "CDUP" && command != "MLST") {
            Reply("501 Missing pathname");
            return;
        }

        struct stat info{};
        if (command == "CWD" || command == "CDUP") {
            if (Stat(*location, info) && S_ISDIR(info.st_mode)) {
                cwd = *path;
                Reply("250 Directory changed");
            } else Reply("550 Directory unavailable");
        } else if (command == "MLST") {
            if (Stat(*location, info) && (S_ISDIR(info.st_mode) || S_ISREG(info.st_mode))) {
                Reply("250-Listing");
                Reply(" " + Facts(info) + " " + *path);
                Reply("250 End");
            } else Reply("550 Path unavailable");
        } else if (command == "SIZE") {
            if (!binary) Reply("550 SIZE requires TYPE I");
            else if (Stat(*location, info) && S_ISREG(info.st_mode)) {
                Reply("213 " + std::to_string(info.st_size));
            } else Reply("550 File unavailable");
        } else if (command == "RETR") {
            if (disk.empty() || location->volume_root) Reply("550 File unavailable");
            else Retrieve(*location);
        } else if (!location->volume || location->volume_root) {
            Reply("550 Mount entries and the FTP root are read-only");
        } else if (command == "STOR" || command == "APPE") {
            Store(*location, command == "APPE");
        } else if (command == "MKD") {
            bool created;
            {
                const auto lock = LockVolume(*location);
                created = mkdir(disk.c_str(), 0700) == 0;
            }
            if (created) PathReply(*path);
            else Reply("550 Cannot create directory");
        } else if (command == "RMD" || command == "DELE") {
            bool removed;
            {
                const auto lock = LockVolume(*location);
                removed = (command == "RMD" ? rmdir(disk.c_str()) : unlink(disk.c_str())) == 0;
            }
            if (removed) {
                if (command == "RMD") CheckWorkingDirectory();
                Reply("250 Removed");
            } else Reply("550 Cannot remove path");
        } else if (command == "RNFR") {
            if (Stat(*location, info)) {
                rename_from = *path;
                Reply("350 Send destination with RNTO");
            } else Reply("550 Path unavailable");
        } else if (command == "RNTO") {
            const auto source = rename_source.empty() ? std::optional<Location>{} : Locate(rename_source);
            if (!source) Reply("503 Send RNFR first");
            else if (source->volume != location->volume) Reply("550 Cannot rename across volumes");
            else {
                bool renamed;
                {
                    const auto lock = LockVolume(*location);
                    renamed = rename(source->disk.c_str(), disk.c_str()) == 0;
                }
                if (renamed) {
                    CheckWorkingDirectory();
                    Reply("250 Renamed");
                } else Reply("550 Cannot rename path");
            }
        }
    }

    void Command(const std::string &line) {
        const auto split = line.find(' ');
        auto command = Upper(std::string_view(line).substr(0, split));
        if (command == "XCUP") command = "CDUP";
        if (command == "XCWD" || command == "XMKD" || command == "XRMD" || command == "XPWD") command.erase(0, 1);
        const auto arg = split == line.npos ? std::string{} : line.substr(split + 1);
        const auto rename_source = std::exchange(rename_from, {});

        if (command.empty()) Reply("500 Invalid command line");
        else if (command == "QUIT") { Reply("221 Goodbye"); alive = false; }
        else if (command == "NOOP") Reply("200 OK");
        else if (command == "SYST") Reply("215 UNIX Type: L8");
        else if (command == "HELP") {
            Reply("214-Commands");
            Reply(" ABOR ACCT APPE CDUP CWD DELE EPRT EPSV FEAT HELP LIST MKD MLSD MLST");
            Reply(" MODE NLST NOOP OPTS PASS PASV PORT PWD QUIT RETR RMD RNFR RNTO SIZE STAT STOR STRU SYST TYPE USER");
            Reply("214 End");
        }
        else if (command == "FEAT") {
            Reply("211-Features");
            Reply(" EPRT");
            Reply(" EPSV");
            Reply(std::string(" MLST type") + (fact_type ? "*;size" : ";size") + (fact_size ? "*;" : ";"));
            Reply(" SIZE");
            Reply("211 End");
        } else if (command == "USER" || command == "PASS") Login(command, arg);
        else if (command == "AUTH") Reply("502 Security mechanism unsupported");
        else if (!logged_in) Reply("530 Log in with USER and PASS");
        else if (command == "PWD") PathReply(cwd);
        else if (command == "ACCT") Reply("202 Account information is unnecessary");
        else if (command == "OPTS") Options(arg);
        else if (command == "TYPE") {
            const auto type = Upper(arg);
            if (type == "A" || type == "A N" || type == "I" || type == "L 8" || type == "L8") {
                binary = type == "I" || type == "L 8" || type == "L8";
                Reply("200 Representation set");
            } else Reply("504 Representation unsupported");
        } else if (command == "MODE" || command == "STRU") {
            if (Upper(arg) == (command == "MODE" ? "S" : "F")) Reply("200 Parameter accepted");
            else Reply("504 Parameter unsupported");
        } else if (command == "EPSV") {
            if (Upper(arg) == "ALL") {
                epsv_all = true;
                ResetData();
                Reply("200 EPSV ALL accepted");
            } else if (arg.empty() || arg == "1") SetPassive(true);
            else Reply("522 Network protocol not supported, use (1)");
        } else if (command == "PASV" || command == "PORT" || command == "EPRT") {
            if (epsv_all) Reply("501 EPSV ALL is in effect");
            else if (command == "PASV") {
                if (arg.empty()) SetPassive(false);
                else Reply("501 PASV takes no argument");
            } else SetActive(arg, command == "EPRT");
        } else if (command == "ABOR") { ResetData(); Reply("225 No transfer in progress"); }
        else if (command == "STAT" && arg.empty()) Reply("211 FTP session active");
        else if (command == "CWD" || command == "CDUP" || command == "LIST" || command == "NLST" ||
                 command == "MLST" || command == "MLSD" || command == "SIZE" || command == "STAT" || command == "RETR" ||
                 command == "STOR" || command == "APPE" || command == "MKD" || command == "RMD" ||
                 command == "DELE" || command == "RNFR" || command == "RNTO") {
            FileCommand(command, arg, rename_source);
        } else Reply("502 Command not implemented");
    }

public:
    Session(const Config &config, VolumeMutex *volume_locks, int control,
            sockaddr_in peer, sockaddr_in local, void (*log)(const char *))
        : config(config), volume_locks(volume_locks), control(control), peer(peer), local(local), log(log) {}

    void Run() {
        Reply("220 Horizon FTP ready");
        auto deadline = MonotonicMs() + ControlTimeout;
        while (alive && input.Read(control, false)) {
            if (input.ready) {
                Command(input.Take());
                deadline = MonotonicMs() + ControlTimeout;
                continue;
            }
            const auto remaining = deadline - MonotonicMs();
            if (remaining <= 0) {
                Reply("421 Control connection timed out");
                break;
            }
            pollfd fd{control, POLLIN, 0};
            const int result = poll(&fd, 1, static_cast<int>(remaining));
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) {
                if (result == 0) Reply("421 Control connection timed out");
                break;
            }
            if (!input.Read(control, true)) break;
        }
    }
};

class SessionPool {
    struct Worker {
        SessionPool *pool = nullptr;
        Thread thread{};
        Fd client;
        sockaddr_in peer{}, local{};
        bool busy = false;
    };

    const Config &config;
    void (*log)(const char *);
    std::vector<VolumeMutex> volume_locks;
    std::array<Worker, SessionLimit> workers{};
    VolumeMutex lock;
    CondVar wake{};
    size_t started = 0;
    bool stopped = false;

    static void Run(void *argument) {
        auto &worker = *static_cast<Worker *>(argument);
        auto &pool = *worker.pool;
        std::unique_lock guard(pool.lock);
        for (;;) {
            while (!worker.busy && !pool.stopped) condvarWait(&pool.wake, &pool.lock.value);
            if (pool.stopped) return;

            guard.unlock();
            Session(pool.config, pool.volume_locks.data(), worker.client.Get(),
                    worker.peer, worker.local, pool.log).Run();
            guard.lock();
            worker.client.Reset();
            worker.busy = false;
        }
    }

public:
    SessionPool(const Config &config, void (*log)(const char *))
        : config(config), log(log), volume_locks(config.volumes.size()) {
        condvarInit(&wake);
    }

    ~SessionPool() {
        {
            const auto guard = std::lock_guard(lock);
            stopped = true;
            for (auto &worker : workers) {
                if (worker.client) shutdown(worker.client.Get(), SHUT_RDWR);
            }
            condvarWakeAll(&wake);
        }
        for (size_t index = 0; index < started; ++index) {
            threadWaitForExit(&workers[index].thread);
            threadClose(&workers[index].thread);
        }
    }

    bool Start() {
        for (auto &worker : workers) {
            worker.pool = this;
            if (R_FAILED(threadCreate(&worker.thread, Run, &worker, nullptr, 0x20000, 48, -2))) return false;
            if (R_FAILED(threadStart(&worker.thread))) {
                threadClose(&worker.thread);
                return false;
            }
            ++started;
        }
        return true;
    }

    bool Submit(Fd &client, sockaddr_in peer, sockaddr_in local) {
        const auto guard = std::lock_guard(lock);
        for (auto &worker : workers) {
            if (worker.busy) continue;
            worker.client = std::move(client);
            worker.peer = peer;
            worker.local = local;
            worker.busy = true;
            condvarWakeAll(&wake);
            return true;
        }
        return false;
    }
};

} // namespace

int Serve(const Config &config, void (*log)(const char *)) {
    const Fd listener = Listen(Endpoint(in_addr{htonl(INADDR_ANY)}, config.port), 4);
    if (!listener) {
        log("[ftpd] could not open control listener\n");
        return 1;
    }
    SessionPool pool(config, log);
    if (!pool.Start()) {
        log("[ftpd] could not start session workers\n");
        return 1;
    }
    log("[ftpd] listening\n");

    for (;;) {
        sockaddr_in peer{}, local{};
        socklen_t size = sizeof(peer);
        Fd client(accept(listener.Get(), Address(peer), &size));
        if (!client) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            log("[ftpd] control accept failed\n");
            return 1;
        }
        size = sizeof(local);
        const timeval timeout{5, 0};
        const int oob_inline = 1;
        if (getsockname(client.Get(), Address(local), &size) < 0 ||
            setsockopt(client.Get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) continue;
        // Some BSD backends omit urgent data support. Plain ABOR still works.
        setsockopt(client.Get(), SOL_SOCKET, SO_OOBINLINE, &oob_inline, sizeof(oob_inline));
        if (!pool.Submit(client, peer, local)) {
            SendAll(client.Get(), "421 Too many FTP sessions\r\n");
        }
    }
}

} // namespace ftp
