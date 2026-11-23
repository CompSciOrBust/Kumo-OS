#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace ftp {

inline constexpr char SystemConfigEntry[] = "ftpd.cfg";

struct Volume {
    std::string name;
    std::string root;
    std::string private_entry{};
};

struct Config {
    std::vector<Volume> volumes;
    std::string user;
    std::string password;
    std::uint16_t port = 21;
    std::uint16_t passive_min_port = 50000;
    std::uint16_t passive_max_port = 50002;
    std::uint32_t passive_address = 0; // Network byte order; zero uses the control socket address.
};

inline constexpr std::size_t SessionLimit = 3;

// Each volume's root and staging directory must be on the same FAT volume.
int Serve(const Config &config, void (*log)(const char *));

}
