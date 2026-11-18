// Recognized command IDs for operations the existing FS leaves unimplemented.
constexpr UnimplementedCommand UnimplementedProxyCommands[] = {
    {0, 0, MAKEHOSVERSION(1, 0, 0)}, // OpenFileSystem
    {2, 0, UINT32_MAX}, // OpenDataFileSystemByCurrentProcess
    {7, MAKEHOSVERSION(2, 0, 0), UINT32_MAX}, // OpenFileSystemWithPatch
    {9, MAKEHOSVERSION(3, 0, 0), UINT32_MAX}, // OpenDataFileSystemByProgramId
    {17, 0, UINT32_MAX}, // OpenHostFileSystem
    {22, 0, UINT32_MAX}, // CreateSaveDataFileSystem
    {24, 0, UINT32_MAX}, // RegisterSaveDataFileSystemAtomicDeletion
    {30, 0, UINT32_MAX}, // OpenGameCardStorage
    {31, 0, UINT32_MAX}, // OpenGameCardFileSystem
    {32, MAKEHOSVERSION(3, 0, 0), UINT32_MAX}, // ExtendSaveDataFileSystem
    {33, MAKEHOSVERSION(5, 0, 0), UINT32_MAX}, // DeleteCacheStorage
    {34, MAKEHOSVERSION(5, 0, 0), UINT32_MAX}, // GetCacheStorageSize
    {35, MAKEHOSVERSION(6, 0, 0), UINT32_MAX}, // CreateSaveDataFileSystemWithHashSalt
    {36, MAKEHOSVERSION(9, 0, 0), UINT32_MAX}, // OpenHostFileSystemWithOption
    {51, 0, UINT32_MAX}, // OpenSaveDataFileSystem
    {53, MAKEHOSVERSION(2, 0, 0), UINT32_MAX}, // OpenReadOnlySaveDataFileSystem
    {60, 0, UINT32_MAX}, // OpenSaveDataInfoReader
    {61, 0, UINT32_MAX}, // OpenSaveDataInfoReaderBySaveDataSpaceId
    {62, MAKEHOSVERSION(5, 0, 0), UINT32_MAX}, // OpenSaveDataInfoReaderOnlyCacheStorage
    {64, MAKEHOSVERSION(5, 0, 0), UINT32_MAX}, // OpenSaveDataInternalStorageFileSystem
    {65, MAKEHOSVERSION(5, 0, 0), UINT32_MAX}, // UpdateSaveDataMacForDebug
    {66, MAKEHOSVERSION(5, 0, 0), UINT32_MAX}, // WriteSaveDataFileSystemExtraDataWithMask
    {67, MAKEHOSVERSION(6, 0, 0), UINT32_MAX}, // FindSaveDataWithFilter
    {68, MAKEHOSVERSION(6, 0, 0), UINT32_MAX}, // OpenSaveDataInfoReaderWithFilter
    {69, MAKEHOSVERSION(8, 0, 0), UINT32_MAX}, // ReadSaveDataFileSystemExtraDataBySaveDataAttribute
    {70, MAKEHOSVERSION(8, 0, 0), UINT32_MAX}, // WriteSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute
    {71, MAKEHOSVERSION(10, 0, 0), UINT32_MAX}, // ReadSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute
    {80, 0, UINT32_MAX}, // OpenSaveDataMetaFile
    {81, MAKEHOSVERSION(4, 0, 0), UINT32_MAX}, // OpenSaveDataTransferManager
    {82, MAKEHOSVERSION(5, 0, 0), UINT32_MAX}, // OpenSaveDataTransferManagerVersion2
    {83, MAKEHOSVERSION(6, 0, 0), UINT32_MAX}, // OpenSaveDataTransferProhibiter
    {84, MAKEHOSVERSION(6, 0, 0), UINT32_MAX}, // ListAccessibleSaveDataOwnerId
    {85, MAKEHOSVERSION(9, 0, 0), UINT32_MAX}, // OpenSaveDataTransferManagerForSaveDataRepair
    {86, MAKEHOSVERSION(10, 0, 0), UINT32_MAX}, // OpenSaveDataMover
    {87, MAKEHOSVERSION(11, 0, 0), UINT32_MAX}, // OpenSaveDataTransferManagerForRepair
    {100, 0, UINT32_MAX}, // OpenImageDirectoryFileSystem
    {101, MAKEHOSVERSION(11, 0, 0), UINT32_MAX}, // OpenBaseFileSystem
    {102, MAKEHOSVERSION(12, 0, 0), UINT32_MAX}, // FormatBaseFileSystem
    {120, MAKEHOSVERSION(6, 0, 0), MAKEHOSVERSION(9, 2, 0)}, // OpenCloudBackupWorkStorageFileSystem
    {130, MAKEHOSVERSION(7, 0, 0), UINT32_MAX}, // OpenCustomStorageFileSystem
    {200, 0, UINT32_MAX}, // OpenDataStorageByCurrentProcess
    {201, MAKEHOSVERSION(3, 0, 0), UINT32_MAX}, // OpenDataStorageByProgramId
    {202, 0, UINT32_MAX}, // OpenDataStorageByDataId
    {203, 0, UINT32_MAX}, // OpenPatchDataStorageByCurrentProcess
    {204, MAKEHOSVERSION(7, 0, 0), UINT32_MAX}, // OpenDataFileSystemWithProgramIndex
    {205, MAKEHOSVERSION(7, 0, 0), UINT32_MAX}, // OpenDataStorageWithProgramIndex
    {206, MAKEHOSVERSION(13, 0, 0), MAKEHOSVERSION(15, 0, 1)}, // OpenDataStorageByPathObsolete
    {206, MAKEHOSVERSION(16, 0, 0), UINT32_MAX}, // OpenDataStorageByPath
    {500, 0, UINT32_MAX}, // OpenSdCardDetectionEventNotifier
    {501, 0, UINT32_MAX}, // OpenGameCardDetectionEventNotifier
    {510, MAKEHOSVERSION(5, 0, 0), UINT32_MAX}, // OpenSystemDataUpdateEventNotifier
    {511, MAKEHOSVERSION(5, 0, 0), UINT32_MAX}, // NotifySystemDataUpdateEvent
    {520, MAKEHOSVERSION(6, 0, 0), UINT32_MAX}, // SimulateDeviceDetectionEvent
    {600, 0, MAKEHOSVERSION(3, 0, 2)}, // SetCurrentPosixTime
    {601, 0, UINT32_MAX}, // QuerySaveDataTotalSize
    {602, 0, UINT32_MAX}, // VerifySaveDataFileSystem
    {603, 0, UINT32_MAX}, // CorruptSaveDataFileSystem
    {604, 0, UINT32_MAX}, // CreatePaddingFile
    {605, 0, UINT32_MAX}, // DeleteAllPaddingFiles
    {606, MAKEHOSVERSION(2, 0, 0), UINT32_MAX}, // GetRightsId
    {607, MAKEHOSVERSION(2, 0, 0), UINT32_MAX}, // RegisterExternalKey
    {608, MAKEHOSVERSION(2, 0, 0), UINT32_MAX}, // UnregisterAllExternalKey
    {609, MAKEHOSVERSION(2, 0, 0), MAKEHOSVERSION(15, 0, 1)}, // GetRightsIdByPath
    {610, MAKEHOSVERSION(3, 0, 0), UINT32_MAX}, // GetRightsIdAndKeyGenerationByPathObsolete
    {610, MAKEHOSVERSION(16, 0, 0), UINT32_MAX}, // GetRightsIdAndKeyGenerationByPath
    {611, MAKEHOSVERSION(4, 0, 0), UINT32_MAX}, // SetCurrentPosixTimeWithTimeDifference
    {612, MAKEHOSVERSION(4, 0, 0), UINT32_MAX}, // GetFreeSpaceSizeForSaveData
    {613, MAKEHOSVERSION(4, 0, 0), UINT32_MAX}, // VerifySaveDataFileSystemBySaveDataSpaceId
    {614, MAKEHOSVERSION(4, 0, 0), UINT32_MAX}, // CorruptSaveDataFileSystemBySaveDataSpaceId
    {615, MAKEHOSVERSION(5, 0, 0), UINT32_MAX}, // QuerySaveDataInternalStorageTotalSize
    {616, MAKEHOSVERSION(6, 0, 0), UINT32_MAX}, // GetSaveDataCommitId
    {617, MAKEHOSVERSION(7, 0, 0), UINT32_MAX}, // UnregisterExternalKey
    {618, MAKEHOSVERSION(17, 0, 0), UINT32_MAX}, // GetProgramId
    {620, MAKEHOSVERSION(2, 0, 0), UINT32_MAX}, // SetSdCardEncryptionSeed
    {630, MAKEHOSVERSION(4, 0, 0), UINT32_MAX}, // SetSdCardAccessibility
    {631, MAKEHOSVERSION(4, 0, 0), UINT32_MAX}, // IsSdCardAccessible
    {640, MAKEHOSVERSION(4, 0, 0), MAKEHOSVERSION(7, 0, 1)}, // IsSignedSystemPartitionOnSdCardValid
    {700, MAKEHOSVERSION(5, 0, 0), UINT32_MAX}, // OpenAccessFailureDetectionEventNotifier
    {701, MAKEHOSVERSION(5, 0, 0), UINT32_MAX}, // GetAccessFailureDetectionEvent
    {702, MAKEHOSVERSION(5, 0, 0), UINT32_MAX}, // IsAccessFailureDetected
    {710, MAKEHOSVERSION(5, 0, 0), UINT32_MAX}, // ResolveAccessFailure
    {720, MAKEHOSVERSION(5, 0, 0), UINT32_MAX}, // AbandonAccessFailure
    {800, MAKEHOSVERSION(2, 0, 0), UINT32_MAX}, // GetAndClearErrorInfo
    {810, MAKEHOSVERSION(7, 0, 0), UINT32_MAX}, // RegisterProgramIndexMapInfo
    {1000, 0, MAKEHOSVERSION(9, 2, 0)}, // SetBisRootForHost
    {1001, 0, UINT32_MAX}, // SetSaveDataSize
    {1002, 0, UINT32_MAX}, // SetSaveDataRootPath
    {1004, 0, UINT32_MAX}, // SetGlobalAccessLogMode
    {1005, 0, UINT32_MAX}, // GetGlobalAccessLogMode
    {1006, 0, UINT32_MAX}, // OutputAccessLogToSdCard
    {1007, MAKEHOSVERSION(4, 0, 0), UINT32_MAX}, // RegisterUpdatePartition
    {1008, MAKEHOSVERSION(4, 0, 0), UINT32_MAX}, // OpenRegisteredUpdatePartition
    {1009, MAKEHOSVERSION(4, 0, 0), UINT32_MAX}, // GetAndClearMemoryReportInfo
    {1010, MAKEHOSVERSION(5, 1, 0), MAKEHOSVERSION(6, 2, 0)}, // SetDataStorageRedirectTarget
    {1011, MAKEHOSVERSION(7, 0, 0), UINT32_MAX}, // GetProgramIndexForAccessLog
    {1012, MAKEHOSVERSION(9, 0, 0), UINT32_MAX}, // GetFsStackUsage
    {1013, MAKEHOSVERSION(9, 0, 0), UINT32_MAX}, // UnsetSaveDataRootPath
    {1014, MAKEHOSVERSION(10, 0, 0), MAKEHOSVERSION(10, 2, 0)}, // OutputMultiProgramTagAccessLog
    {1016, MAKEHOSVERSION(11, 0, 0), UINT32_MAX}, // FlushAccessLogOnSdCard
    {1017, MAKEHOSVERSION(11, 0, 0), UINT32_MAX}, // OutputApplicationInfoAccessLog
    {1018, MAKEHOSVERSION(13, 0, 0), UINT32_MAX}, // RegisterDebugConfiguration
    {1019, MAKEHOSVERSION(13, 0, 0), UINT32_MAX}, // UnregisterDebugConfiguration
    {1100, MAKEHOSVERSION(4, 0, 0), UINT32_MAX}, // OverrideSaveDataTransferTokenSignVerificationKey
    {1110, MAKEHOSVERSION(6, 0, 0), UINT32_MAX}, // CorruptSaveDataFileSystemByOffset
    {1200, MAKEHOSVERSION(6, 0, 0), UINT32_MAX}, // OpenMultiCommitManager
    {1300, MAKEHOSVERSION(10, 0, 0), UINT32_MAX}, // OpenBisWiper
};
constexpr UnimplementedCommand UnimplementedDeviceCommands[] = {
    {1, 0, UINT32_MAX}, // GetSdCardSpeedMode
    {2, 0, UINT32_MAX}, // GetSdCardCid
    {3, 0, UINT32_MAX}, // GetSdCardUserAreaSize
    {4, 0, UINT32_MAX}, // GetSdCardProtectedAreaSize
    {5, 0, UINT32_MAX}, // GetAndClearSdCardErrorInfo
    {100, 0, UINT32_MAX}, // GetMmcCid
    {101, 0, UINT32_MAX}, // GetMmcSpeedMode
    {112, 0, UINT32_MAX}, // GetMmcPatrolCount
    {113, 0, UINT32_MAX}, // GetAndClearMmcErrorInfo
    {114, 0, UINT32_MAX}, // GetMmcExtendedCsd
    {202, 0, UINT32_MAX}, // GetGameCardHandle
    {208, 0, UINT32_MAX}, // GetGameCardIdSet
    {217, 0, UINT32_MAX}, // GetGameCardErrorReportInfo
    {218, 0, UINT32_MAX}, // GetGameCardDeviceId
};
