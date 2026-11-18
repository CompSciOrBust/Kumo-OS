#pragma once
#include <kumo/prelude.hpp>
#include <kumo/mutex.hpp>
#include <kumo/scope_exit.hpp>
#include <kumo/fs/storage.hpp>
#include <cinttypes>
#include <optional>
#include <strings.h>
#include <string_view>

namespace kumo::horizon {
namespace ncm {
struct ProgramId {
    u64 value;
};
inline constexpr ProgramId InvalidProgramId{};
enum class StorageId : u8 { None, Host, GameCard, BuiltInSystem, BuiltInUser, SdCard };
}
namespace fs {
constexpr Result ResultWriteModeFileNotClosed() { return kumo::fs::Error(6457); }
constexpr Result ResultUnsupportedOperation() { return kumo::fs::Error(6300); }
constexpr Result ResultUnexpected() { return kumo::fs::Error(5000); }
constexpr Result ResultPreconditionViolation() { return kumo::fs::Error(6000); }
constexpr Result ResultPermissionDenied() { return kumo::fs::Error(6400); }
constexpr Result ResultNotEnoughFreeSpace() { return kumo::fs::Error(30); }
constexpr Result ResultReadNotPermitted() { return kumo::fs::Error(6202); }
constexpr Result ResultWriteNotPermitted() { return kumo::fs::Error(6203); }
constexpr Result ResultInvalidArgument() { return kumo::fs::Error(6001); }
constexpr Result ResultFileExtensionWithoutOpenModeAllowAppend() { return kumo::fs::Error(6201); }
constexpr Result ResultFatFileSystemCorrupted() { return kumo::fs::Error(4681); }
constexpr Result ResultAllocationMemoryFailed() { return kumo::fs::Error(3200); }
constexpr Result ResultAllocationMemoryFailedInFileSystemProxyImplA() { return kumo::fs::Error(3421); }
constexpr Result ResultAllocationMemoryFailedInNcaFileSystemServiceImplB() { return kumo::fs::Error(3257); }
constexpr Result ResultDirectoryNotEmpty() { return kumo::fs::Error(8); }
constexpr Result ResultInvalidNcaFileSystemType() { return kumo::fs::Error(4512); }
constexpr Result ResultInvalidNcaId() { return kumo::fs::Error(4527); }
constexpr Result ResultInvalidOffset() { return kumo::fs::Error(6061); }
constexpr Result ResultInvalidOpenMode() { return kumo::fs::Error(6072); }
constexpr Result ResultInvalidPathFormat() { return kumo::fs::Error(6005); }
constexpr Result ResultInvalidSaveDataSpaceId() { return kumo::fs::Error(6082); }
constexpr Result ResultInvalidSize() { return kumo::fs::Error(6062); }
constexpr Result ResultNotImplemented() { return kumo::fs::Error(3001); }
constexpr Result ResultNullptrArgument() { return kumo::fs::Error(6063); }
constexpr Result ResultOpenCountLimit() { return kumo::fs::Error(6709); }
constexpr Result ResultOutOfRange() { return kumo::fs::Error(3005); }
constexpr Result ResultPartitionNotFound() { return kumo::fs::Error(1001); }
constexpr Result ResultPathAlreadyExists() { return kumo::fs::Error(2); }
constexpr Result ResultPathNotFound() { return kumo::fs::Error(1); }
constexpr Result ResultSdCardNotPresent() { return kumo::fs::Error(2001); }
constexpr Result ResultTargetLocked() { return kumo::fs::Error(7); }
constexpr Result ResultTargetNotFound() { return kumo::fs::Error(1002); }
constexpr Result ResultTooLongPath() { return kumo::fs::Error(6003); }
enum class BisPartitionId {
    /* Boot0 */
    BootPartition1Root = 0,

    /* Boot1 */
    BootPartition2Root = 10,

    /* Non-Boot */
    UserDataRoot = 20,
    BootConfigAndPackage2Part1 = 21,
    BootConfigAndPackage2Part2 = 22,
    BootConfigAndPackage2Part3 = 23,
    BootConfigAndPackage2Part4 = 24,
    BootConfigAndPackage2Part5 = 25,
    BootConfigAndPackage2Part6 = 26,
    CalibrationBinary = 27,
    CalibrationFile = 28,
    SafeMode = 29,
    User = 30,
    System = 31,
    SystemProperEncryption = 32,
    SystemProperPartition = 33,
    SignedSystemPartitionOnSafeMode = 34,
    DeviceTreeBlob = 35,
    System0 = 36,
};
enum class ContentStorageId : u32 {
    System = 0,
    User = 1,
    SdCard = 2,
    System0 = 3,
};
enum class OperationId : s64 {
    FillZero = 0,
    DestroySignature = 1,
    Invalidate = 2,
    QueryRange = 3,
    QueryUnpreparedRange = 4,
    QueryLazyLoadCompletionRate = 5,
    SetLazyLoadPriority = 6,

    ReadLazyLoadFileForciblyForDebug = 10001,
};
using SystemSaveDataId = u64;
enum class SaveDataSpaceId : u8 {
    System = 0,
    User = 1,
    SdSystem = 2,
    Temporary = 3,
    SdUser = 4,

    ProperSystem = 100,
    SafeMode = 101,
};

enum class SaveDataType : u8 {
    System = 0,
    Account = 1,
    Bcat = 2,
    Device = 3,
    Temporary = 4,
    Cache = 5,
    SystemBcat = 6,
};

enum class SaveDataRank : u8 {
    Primary = 0,
    Secondary = 1,
};

struct UserId {
    u64 data[2];
    bool operator==(const UserId &) const = default;
};

constexpr inline SystemSaveDataId InvalidSystemSaveDataId = 0;
constexpr inline UserId InvalidUserId = {};

struct SaveDataCreationInfo {
    static constexpr size_t WireBytes = 0x40;
    s64 size;
    s64 journal_size;
    s64 block_size;
    u64 owner_id;
    u32 flags;
    SaveDataSpaceId space_id;
    u8 pseudo;
    u8 reserved[0x1A];
};
static_assert(sizeof(SaveDataCreationInfo) == 0x40);

struct SaveDataAttribute {
    static constexpr size_t WireBytes = 0x40;
    ncm::ProgramId program_id;
    UserId user_id;
    SystemSaveDataId system_save_data_id;
    SaveDataType type;
    SaveDataRank rank;
    u16 index;
    u8 reserved[0x1C];
};
static_assert(sizeof(SaveDataAttribute) == 0x40);
static_assert(std::is_trivially_destructible<SaveDataAttribute>::value);

struct SaveDataExtraData {
    SaveDataAttribute attr;
    u64 owner_id;
    s64 timestamp;
    u32 flags;
    u8 pad[4];
    s64 available_size;
    s64 journal_size;
    s64 commit_id;
    u8 unused[0x190];
};
constexpr inline size_t EntryNameLengthMax = 0x300;

struct DirectoryEntry {
    char name[EntryNameLengthMax + 1];
    char pad[3];
    s8 type;
    u8 pad2[3];
    s64 file_size;
};
struct FileTimeStampRaw {
    s64 create;
    s64 modify;
    s64 access;
    u8 is_local_time;
    static constexpr size_t WireBytes = 0x20;
    char pad[7];
};
static_assert(sizeof(SaveDataExtraData) == 0x200);
static_assert(sizeof(DirectoryEntry) == 0x310 && offsetof(DirectoryEntry, type) == 0x304 &&
              offsetof(DirectoryEntry, file_size) == 0x308);
static_assert(offsetof(SaveDataAttribute, system_save_data_id) == 0x18);
static_assert(offsetof(SaveDataCreationInfo, space_id) == 0x24);
static_assert(offsetof(SaveDataExtraData, flags) == 0x50);
enum DirectoryEntryType : s8 { DirectoryEntryType_Directory, DirectoryEntryType_File };
using OpenMode = u32;
inline constexpr OpenMode OpenMode_Read = 1, OpenMode_Write = 2, OpenMode_ReadWrite = 3, OpenMode_AllowAppend = 4;
using OpenDirectoryMode = u32;
inline constexpr OpenDirectoryMode OpenDirectoryMode_Directory = 1, OpenDirectoryMode_File = 2,
                                   OpenDirectoryMode_All = 3, OpenDirectoryMode_NotRequireFileSize = 0x80000000;
enum class ReadOption : u32 { None };
enum class WriteOption : u32 { None, Flush };
constexpr bool HasFlushFlag(WriteOption option) { return static_cast<u32>(option) & 1u; }

template <class T, class... Args> std::shared_ptr<T> AllocateShared(Args &&...args) {
    try {
        return std::make_shared<T>(std::forward<Args>(args)...);
    } catch (const std::bad_alloc &) {
        return {};
    }
}
using IStorage = kumo::fs::Storage;
using QueryRangeInfo = kumo::fs::RangeInfo;
namespace impl {
inline constexpr char ContentStorageSdCardMountName[] = "@SdCardContent";
inline constexpr char ContentStorageUserMountName[] = "@UserContent";
inline constexpr char ContentStorageSystemMountName[] = "@SystemContent";
inline constexpr char BisSystemPartitionMountName[] = "@System";
inline constexpr char BisUserPartitionMountName[] = "@User";
inline constexpr char SdCardFileSystemMountName[] = "@Sdcard";
enum FileSystemProxyType {
    FileSystemProxyType_Code = 0,
    FileSystemProxyType_Rom = 1,
    FileSystemProxyType_Logo = 2,
    FileSystemProxyType_Control = 3,
    FileSystemProxyType_Manual = 4,
    FileSystemProxyType_Meta = 5,
    FileSystemProxyType_Data = 6,
    FileSystemProxyType_Package = 7,
    FileSystemProxyType_UpdatePartition = 8,
};
}
}
}
