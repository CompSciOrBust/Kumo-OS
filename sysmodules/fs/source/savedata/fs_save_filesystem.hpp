#pragma once
#include "fs_filesystem.hpp"

namespace kumo::horizon::hzfs::savedata {
Result MountJournal(std::shared_ptr<fs::fsa::IFileSystem> *out, std::shared_ptr<fs::fsa::IFileSystem> save_root);
}
