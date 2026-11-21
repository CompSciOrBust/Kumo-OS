/*
 * Content storage and the filesystems inside installed content.
 *
 * A content storage is the directory in which ncm keeps installed NCAs:
 * "Contents" on SYSTEM and USER, "Nintendo/Contents" on the SD card. ncm
 * manages the files; FS only serves the directory, and opens the filesystems
 * inside individual NCAs when given their path.
 */
#pragma once
#include "fs_filesystem.hpp"

namespace kumo::horizon::hzfs::content {

    /* The content storage directory, created if it does not exist yet. */
    Result OpenContentStorage(std::shared_ptr<fs::fsa::IFileSystem> *out, fs::ContentStorageId id);

    /*
     * Open a filesystem of the content at path. The path starts with a mount
     * name FS knows, as ncm and lr produce them ("@SystemContent://registered/...").
     * program_id is checked against the NCA unless it is zero.
     */
    Result OpenFileSystemByPath(std::shared_ptr<fs::fsa::IFileSystem> *out, const char *path, fs::impl::FileSystemProxyType type, u64 program_id);

    /*
     * Open the code filesystem (main, main.npdm, ...) of a program, locating its
     * NCA through ncm's location resolver, or by searching the content storage
     * when ncm is not running.
     */
    Result OpenCodeFileSystem(std::shared_ptr<fs::fsa::IFileSystem> *out, ncm::ProgramId program_id, ncm::StorageId storage_id);

    Result OpenProgramRomStorage(std::shared_ptr<fs::IStorage> *out, ncm::ProgramId program_id, ncm::StorageId storage_id);

}
