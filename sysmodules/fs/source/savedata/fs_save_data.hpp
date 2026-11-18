/*
 * System save data.
 *
 * Each save data is a directory, "save/<id>", on the FAT volume of its
 * space (SYSTEM, USER, SAFE or the SD card), holding:
 *
 *   0/       the committed contents
 *   1/       the working copy that clients read and write
 *   .lock    held open while the save data is mounted
 *   extra    its fs::SaveDataExtraData (owner, flags, sizes)
 *
 * Commit copies the working directory via the staging directory (_/) and
 * publishes it by renaming the staging directory to 0/.
 *
 * The size limits a save data is created with are recorded but not enforced.
 */
#pragma once
#include "fs_filesystem.hpp"

namespace kumo::horizon::hzfs::savedata {

    /* Create an empty system save data. Fails with fs::ResultPathAlreadyExists if it exists. */
    Result CreateSystemSaveData(const fs::SaveDataAttribute &attribute, const fs::SaveDataCreationInfo &creation_info);

    /* Mount a system save data. Fails with fs::ResultTargetNotFound if it does not exist, fs::ResultTargetLocked if it is mounted. */
    Result OpenSystemSaveData(std::shared_ptr<fs::fsa::IFileSystem> *out, fs::SaveDataSpaceId space_id, const fs::SaveDataAttribute &attribute);

    /* Delete a system save data and everything in it. */
    Result DeleteSystemSaveData(fs::SaveDataSpaceId space_id, const fs::SaveDataAttribute &attribute);

    Result ReadExtraData(fs::SaveDataExtraData *out, fs::SaveDataSpaceId space_id, const fs::SaveDataAttribute &attribute);

    /* Only the flags can be changed after creation. */
    Result WriteExtraDataFlags(fs::SaveDataSpaceId space_id, const fs::SaveDataAttribute &attribute, u32 flags);

    /* The attribute of the system save data with this ID that belongs to no user. */
    constexpr fs::SaveDataAttribute MakeSystemSaveDataAttribute(u64 save_data_id) {
        return {.system_save_data_id = save_data_id, .type = fs::SaveDataType::System};
    }

}
