#pragma once
#include "fs_filesystem.hpp"
#include <kumo/fs/nca.hpp>

namespace kumo::horizon::hzfs::nca {
inline constexpr s32 ProgramSection_Code = 0, ProgramSection_Data = 1, ProgramSection_Logo = 2;

struct Reader {
    std::shared_ptr<fs::IStorage> storage;
    kumo::fs::PlaintextNca archive;
};

Result OpenReader(std::shared_ptr<Reader> *out, std::shared_ptr<fs::fsa::IFileSystem> base, const char *path);
Result OpenRomStorage(std::shared_ptr<fs::IStorage> *out, std::shared_ptr<Reader> reader, s32 index);
Result OpenSectionFileSystem(std::shared_ptr<fs::fsa::IFileSystem> *out, std::shared_ptr<Reader> reader, s32 section);
Result OpenPackageFileSystem(std::shared_ptr<fs::fsa::IFileSystem> *out, std::shared_ptr<fs::fsa::IFileSystem> base,
                             const char *path);
}
