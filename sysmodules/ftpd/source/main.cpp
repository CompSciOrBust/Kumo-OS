#include "ftp_server.hpp"

#include <switch.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <string_view>
#include <cstdio>
#include <cstring>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void Log(const char *message) {
    svcOutputDebugString(message, std::strlen(message));
}

bool ValidCredential(std::string_view value) {
    return value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32 && c <= 126; });
}

bool ParsePort(std::string_view value, std::uint16_t &port) {
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && port != 0;
}

bool ParseAddress(std::string_view value, std::uint32_t &address) {
    if (value.empty()) return true;
    const std::string literal(value);
    in_addr parsed{};
    if (inet_pton(AF_INET, literal.c_str(), &parsed) != 1 || parsed.s_addr == INADDR_ANY) return false;
    address = parsed.s_addr;
    return true;
}

std::string_view TakeLine(std::string_view &text) {
    const auto end = text.find('\n');
    auto line = text.substr(0, end);
    text.remove_prefix(end == text.npos ? text.size() : end + 1);
    if (line.ends_with('\r')) line.remove_suffix(1);
    return line;
}

bool ParseConfig(std::string_view text, ftp::Config &config) {
    // Older images shipped two credential lines and an optional address.
    auto legacy = text;
    const auto first = TakeLine(legacy);
    if (!first.empty() && !first.starts_with('#') && first.find('=') == first.npos) {
        const auto password = TakeLine(legacy);
        if (password.empty() || !ValidCredential(first) || !ValidCredential(password) ||
            !ParseAddress(TakeLine(legacy), config.passive_address) || !legacy.empty()) return false;
        config.user = first;
        config.password = password;
        return true;
    }

    enum Key { Username, Password, Port, PassiveMin, PassiveMax, PassiveAddress, KeyCount };
    constexpr std::array<std::string_view, KeyCount> keys{
        "username", "password", "port", "passive_min_port", "passive_max_port", "passive_address"
    };
    std::array<bool, KeyCount> seen{};
    while (!text.empty()) {
        const auto line = TakeLine(text);
        if (line.empty() || line.starts_with('#')) continue;
        const auto separator = line.find('=');
        if (separator == line.npos) return false;
        const auto key = std::find(keys.begin(), keys.end(), line.substr(0, separator)) - keys.begin();
        if (key == KeyCount || seen[key]) return false;
        seen[key] = true;
        const auto value = line.substr(separator + 1);
        switch (key) {
            case Username: config.user = value; break;
            case Password: config.password = value; break;
            case Port: if (!ParsePort(value, config.port)) return false; break;
            case PassiveMin: if (!ParsePort(value, config.passive_min_port)) return false; break;
            case PassiveMax: if (!ParsePort(value, config.passive_max_port)) return false; break;
            case PassiveAddress: if (!ParseAddress(value, config.passive_address)) return false; break;
        }
    }

    // Explicit credentials make an empty or partly written file fail closed.
    return seen[Username] && seen[Password] &&
        config.user.empty() == config.password.empty() &&
        ValidCredential(config.user) && ValidCredential(config.password) &&
        config.port > 1 && config.passive_min_port <= config.passive_max_port &&
        (config.passive_max_port < config.port - 1 || config.passive_min_port > config.port);
}

bool CreateDefaultConfig(const char *path, const ftp::Config &config) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return errno == EEXIST;

    FILE *file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        unlink(path);
        return false;
    }

    const bool written = std::fprintf(file,
        "# Empty username and password enable anonymous FTP.\n"
        "username=\npassword=\nport=%u\npassive_min_port=%u\npassive_max_port=%u\npassive_address=\n",
        unsigned(config.port), unsigned(config.passive_min_port), unsigned(config.passive_max_port)) > 0;
    const bool closed = std::fclose(file) == 0;
    if (written && closed) return true;
    unlink(path);
    return false;
}

bool LoadConfig(const char *path, ftp::Config &config) {
    FILE *file = std::fopen(path, "rb");
    if (!file && errno == ENOENT && CreateDefaultConfig(path, config)) file = std::fopen(path, "rb");
    if (!file) return false;

    char buffer[4097];
    const auto size = std::fread(buffer, 1, sizeof(buffer), file);
    const bool complete = std::feof(file) && !std::ferror(file);
    std::fclose(file);
    const std::string_view text(buffer, size);
    return complete && text.find('\0') == text.npos && ParseConfig(text, config);
}

bool MakeDirectory(const char *path) {
    struct stat info{};
    return mkdir(path, 0700) == 0 || (stat(path, &info) == 0 && S_ISDIR(info.st_mode));
}

void MountPartitions(ftp::Config &config) {
    constexpr struct {
        const char *name;
        FsBisPartitionId id;
        const char *private_entry = "";
    } partitions[] = {
        {"prodinfof", FsBisPartitionId_CalibrationFile},
        {"safe", FsBisPartitionId_SafeMode},
        {"system", FsBisPartitionId_System, ftp::SystemConfigEntry},
        {"user", FsBisPartitionId_User},
    };

    for (const auto &partition : partitions) {
        FsFileSystem fs;
        if (R_SUCCEEDED(fsOpenBisFileSystem(&fs, partition.id, "")) &&
            fsdevMountDevice(partition.name, fs) >= 0) {
            config.volumes.push_back({partition.name, std::string(partition.name) + ":/", partition.private_entry});
        }
    }
}

}

extern "C" {
u32 __nx_applet_type = AppletType_None;

void __libnx_initheap() {
    alignas(0x1000) static char heap[4 * 1024 * 1024];
    extern char *fake_heap_start, *fake_heap_end;
    fake_heap_start = heap;
    fake_heap_end = heap + sizeof(heap);
}

void __appInit() {
    SecmonArgs config{{0xC3000002, 65011}};
    svcCallSecureMonitor(&config);
    if (config.X[0] != 0 || config.X[1] == 0) svcExitProcess();
    hosversionSet(BIT(31) | static_cast<u32>(config.X[1] >> 8));
    if (R_FAILED(smInitialize())) svcExitProcess();
}

void __appExit() { smExit(); }
void __libnx_exit(int) { svcExitProcess(); }
}

int main() {
    if (R_FAILED(fsInitialize())) return 1;

    ftp::Config config{};

    if (R_FAILED(fsdevMountSdmc())) {
        Log("[ftpd] SD card mount failed\n");
    } else {
        config.volumes.push_back({"sdmc", "sdmc:/"});
    }

    MountPartitions(config);
    const auto config_path = std::string("system:/") + ftp::SystemConfigEntry;

    int status = 1;
    if (!LoadConfig(config_path.c_str(), config)) {
        Log(std::string("[ftpd] could not load " + config_path + "\n").c_str());
    } else if (!MakeDirectory(config.volumes.front().root.c_str())) {
        Log("[ftpd] could not prepare SD card directories\n");
    } else {
        SocketInitConfig socket_config = *socketGetDefaultInitConfig();
        // The listener can block in accept while all workers poll their clients.
        socket_config.num_bsd_sessions = ftp::SessionLimit + 1;
        socket_config.tcp_tx_buf_size = 0x8000;
        socket_config.tcp_rx_buf_size = 0x8000;
        while (R_FAILED(socketInitialize(&socket_config))) svcSleepThread(500000000);
        status = ftp::Serve(config, Log);
        socketExit();
    }

    fsdevUnmountAll();
    fsExit();
    return status;
}
