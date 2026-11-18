/*
 * Program registry: which program each running process was launched from.
 * pm fills it through fsp-pr; fsp-srv will consult it for access control.
 */
#pragma once
#include "fs_filesystem.hpp"

namespace kumo::horizon::hzfs::registry {

    struct ProgramInfo {
        u64 process_id;
        ncm::ProgramId program_id;
        ncm::StorageId storage_id;
    };

    /* TODO: Keep the FS access control data (ACI0 `data`, ACID `desc`) once permission checks exist. */
    Result RegisterProgram(u64 process_id, ncm::ProgramId program_id, ncm::StorageId storage_id);
    Result UnregisterProgram(u64 process_id);

    /* Returns false if the process was never registered. */
    bool GetProgramInfo(ProgramInfo *out, u64 process_id);

}
