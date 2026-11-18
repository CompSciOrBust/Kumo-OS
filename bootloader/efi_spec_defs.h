#pragma once
// Based on UEFI Specification, version 2.9
// https://uefi.org/sites/default/files/resources/UEFI_Spec_2_9_2021_03_18.pdf


typedef unsigned long long  ULL;
typedef unsigned long long  EFI_STATUS;
typedef ULL                 EFI_PHYSICAL_ADDRESS;
typedef void*               EFI_HANDLE;

#define EFI_ERROR_BIT   0x8000000000000000ULL
#define EFI_SUCCESS     0
#define EFI_NOT_FOUND   (EFI_ERROR_BIT | 14)

#define AllocateAnyPages  0
#define AllocateAddress   2

typedef struct {
    unsigned int Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char  Data4[8];
} EFI_GUID;

static const EFI_GUID FDT_GUID = {
    0xb1b621d5, 0xf19c, 0x41a5,
    {0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0}
};

static const EFI_GUID ACPI_20_GUID = {
    0x8868e871, 0xe4f1, 0x11d3,
    {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}
};

static const EFI_GUID LOADED_IMAGE_GUID = {
    0x5b1b31a1, 0x9562, 0x11d2,
    {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

static const EFI_GUID SIMPLE_FS_GUID = {
    0x964e5b22, 0x6459, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

static const EFI_GUID EFI_DEVICE_PATH_PROTOCOL_GUID = {
    0x09576e91, 0x6d3f, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

static const EFI_GUID FILE_INFO_GUID = {
    0x09576e92, 0x6d3f, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};


// Based on UEFI Specification, version 2.9
// https://uefi.org/sites/default/files/resources/UEFI_Spec_2_9_2021_03_18.pdf
// Chaper 4.2 EFI Table Header


typedef struct {
    ULL          Signature;
    unsigned int Revision;
    unsigned int HeaderSize;
    unsigned int CRC32;
    unsigned int Reserved;
}   EFI_TABLE_HEADER;

// Based on UEFI Specification, version 2.9
// https://uefi.org/sites/default/files/resources/UEFI_Spec_2_9_2021_03_18.pdf
// Chaper 4.4 EFI Boot Services Table


typedef struct efi_boot_services_table
{
    EFI_TABLE_HEADER Hdr;

    // Task priority services
    void* RaiseTPL;
    void* RestoreTPL;

    // Memory services
    EFI_STATUS (*AllocatePages)(ULL Type, ULL MemType, ULL Pages, EFI_PHYSICAL_ADDRESS* Mem);
    void* FreePages;
    EFI_STATUS (*GetMemoryMap)(ULL* MapSize, void* Map, ULL* MapKey, ULL *DescSize, unsigned int* DescVer);
    EFI_STATUS (*AllocatePool)(ULL PoolType, ULL Size, void** Buf);
    void* FreePool;

    // Event and timer services
    void* CreateEvent;
    void* SetTimer;
    void* WaitForEvent;
    void* SignalEvent;
    void* CloseEvent;
    void* CheckEvent;

    // Protocol handler services
    void* InstallProtocolInterface;
    void* ReinstallProtocolInterface;
    void* UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
    void* Reserved;
    void* RegisterProtocolNotify;
    void* LocateHandle;
    void* LocateDevicePath;
    void* InstallConfigurationTable;

    // Image services
    void* LoadImage;
    void* StartImage;
    void* Exit;
    void* UnloadImage;
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE ImageHandle, ULL MapKey);

    // Miscellaneous services
    void* GetNextMonotonicCount;
    void* Stall;
    void* SetWatchdogTimer;

    // Driver support services
    void* ConnectController;
    void* DisconnectController;

    // Open and close protocol services
    void* OpenProtocol;
    void* CloseProtocol;
    void* OpenProtocolInformation;

    // Library services
    void* ProtocolsPerHandle;
    EFI_STATUS (*LocateHandleBuffer)(unsigned int SearchType, EFI_GUID *Protocol, void *SearchKey, ULL *NoHandles, EFI_HANDLE **Buffer);
    void* LocateProtocol;
    void* InstallMultipleProtocolInterfaces;
    void* UninstallMultipleProtocolInterfaces;

    // CRC32 services
    void* CalculateCrc32;

    // Misc services
    void* CopyMem;
    void* SetMem;
    void* CreateEventEx;
} EFI_BOOT_SERVICES;

#define EFI_LOCATE_BY_PROTOCOL  2

static const EFI_GUID PCI_ROOT_BRIDGE_IO_GUID = {
    0x2f707ebb, 0x4a1a, 0x11d4,
    {0x9a, 0x38, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}
};

// EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL. Only Configuration() is used; the members
// before it are kept as placeholders so it sits at the right offset.
typedef struct EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL {
    EFI_HANDLE ParentHandle;
    void* PollMem;
    void* PollIo;
    void* MemRead;
    void* MemWrite;
    void* IoRead;
    void* IoWrite;
    void* PciRead;
    void* PciWrite;
    void* CopyMem;
    void* Map;
    void* Unmap;
    void* AllocateBuffer;
    void* FreeBuffer;
    void* Flush;
    void* GetAttributes;
    void* SetAttributes;
    EFI_STATUS (*Configuration)(struct EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This, void **Resources);
    unsigned int SegmentNumber;
} EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL;


// Based on UEFI Specification, version 2.9
// https://uefi.org/sites/default/files/resources/UEFI_Spec_2_9_2021_03_18.pdf
// Chaper 4.3 EFI System Table

typedef struct {
    EFI_GUID  VendorGuid;
    void     *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    void       *Reset;
    EFI_STATUS (*OutputString)(void *This, unsigned int short *String);
} EFI_TEXT_OUT;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    unsigned short* FirmwareVendor;
    unsigned int FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void* ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_TEXT_OUT* ConOut;
    EFI_HANDLE StdErrHandle;
    void* StdErr;
    void* RuntimeServices;
    EFI_BOOT_SERVICES* BootServices;
    ULL NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE* ConfigurationTable;

} EFI_SYSTEM_TABLE;

// Based on UEFI Specification, version 2.9
// https://uefi.org/sites/default/files/resources/UEFI_Spec_2_9_2021_03_18.pdf
// Chaper 10.2 EFI Device Path Protocol


typedef struct {
    unsigned char Type;
    unsigned char SubType;
    unsigned char Length[2];
} EFI_DEVICE_PATH_PROTOCOL;


// Based on UEFI Specification, version 2.9
// https://uefi.org/sites/default/files/resources/UEFI_Spec_2_9_2021_03_18.pdf
// Chapter 7.2 Memory Types

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiUnacceptedMemoryType,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

// Chapter 7.2 EFI_BOOT_SERVICES.GetMemoryMap()
// Entries in the map are DescSize bytes apart, which may be larger than this struct.
typedef struct {
    unsigned int         Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    ULL                  VirtualStart;
    ULL                  NumberOfPages;
    ULL                  Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef EFI_STATUS (*EFI_IMAGE_UNLOAD)(EFI_HANDLE ImageHandle);

// Based on UEFI Specification, version 2.9
// https://uefi.org/sites/default/files/resources/UEFI_Spec_2_9_2021_03_18.pdf
// Chaper 9.1 EFI Loaded Image Protocol


typedef struct {
    unsigned int Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;

    EFI_HANDLE DeviceHandle;
    EFI_DEVICE_PATH_PROTOCOL* FilePath;
    void* Reserved;

    unsigned int LoadOptionsSize;
    void* LoadOptions;

    void* ImageBase;
    ULL            ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    EFI_IMAGE_UNLOAD Unload;
} EFI_LOADED_IMAGE_PROTOCOL;


// Based on UEFI Specification, version 2.9
// https://uefi.org/sites/default/files/resources/UEFI_Spec_2_9_2021_03_18.pdf
// Chapter 13.4 Simple File System Protocol


// Based on UEFI Specification, version 2.9
// https://uefi.org/sites/default/files/resources/UEFI_Spec_2_9_2021_03_18.pdf
// Chapter 13.5 EFI File Protocol

typedef struct efi_file_protocol EFI_FILE_PROTOCOL;

#define EFI_FILE_MODE_READ    0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE   0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE  0x8000000000000000ULL

typedef EFI_STATUS (*EFI_FILE_OPEN)        (EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle, unsigned short *FileName, ULL OpenMode, ULL Attributes);
typedef EFI_STATUS (*EFI_FILE_CLOSE)       (EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS (*EFI_FILE_DELETE)      (EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS (*EFI_FILE_READ)        (EFI_FILE_PROTOCOL *This, ULL *BufferSize, void *Buffer);
typedef EFI_STATUS (*EFI_FILE_WRITE)       (EFI_FILE_PROTOCOL *This, ULL *BufferSize, void *Buffer);
typedef EFI_STATUS (*EFI_FILE_GET_POSITION)(EFI_FILE_PROTOCOL *This, ULL *Position);
typedef EFI_STATUS (*EFI_FILE_SET_POSITION)(EFI_FILE_PROTOCOL *This, ULL Position);
typedef EFI_STATUS (*EFI_FILE_GET_INFO)    (EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, ULL *BufferSize, void *Buffer);
typedef EFI_STATUS (*EFI_FILE_SET_INFO)    (EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, ULL BufferSize, void *Buffer);
typedef EFI_STATUS (*EFI_FILE_FLUSH)       (EFI_FILE_PROTOCOL *This);

struct efi_file_protocol {
    ULL                   Revision;
    EFI_FILE_OPEN         Open;
    EFI_FILE_CLOSE        Close;
    EFI_FILE_DELETE       Delete;
    EFI_FILE_READ         Read;
    EFI_FILE_WRITE        Write;
    EFI_FILE_GET_POSITION GetPosition;
    EFI_FILE_SET_POSITION SetPosition;
    EFI_FILE_GET_INFO     GetInfo;
    EFI_FILE_SET_INFO     SetInfo;
    EFI_FILE_FLUSH        Flush;
    void* OpenEx;
    void* ReadEx;
    void* WriteEx;
    void* FlushEx;
};

// Based on UEFI Specification, version 2.9
// https://uefi.org/sites/default/files/resources/UEFI_Spec_2_9_2021_03_18.pdf
// Appendix B EFI_TIME / Chapter 13.5 EFI_FILE_INFO

typedef struct {
    unsigned short Year;
    unsigned char  Month;
    unsigned char  Day;
    unsigned char  Hour;
    unsigned char  Minute;
    unsigned char  Second;
    unsigned char  Pad1;
    unsigned int   Nanosecond;
    short          TimeZone;
    unsigned char  Daylight;
    unsigned char  Pad2;
} EFI_TIME;

#define EFI_FILE_READ_ONLY  0x0000000000000001ULL
#define EFI_FILE_HIDDEN     0x0000000000000002ULL
#define EFI_FILE_SYSTEM     0x0000000000000004ULL
#define EFI_FILE_RESERVED   0x0000000000000008ULL
#define EFI_FILE_DIRECTORY  0x0000000000000010ULL
#define EFI_FILE_ARCHIVE    0x0000000000000020ULL

typedef struct {
    ULL            Size;
    ULL            FileSize;
    ULL            PhysicalSize;
    EFI_TIME       CreateTime;
    EFI_TIME       LastAccessTime;
    EFI_TIME       ModificationTime;
    ULL            Attribute;
    unsigned short FileName[];
} EFI_FILE_INFO;

// Chapter 13.4 Simple File System Protocol

typedef EFI_STATUS (*EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME)(void *This, EFI_FILE_PROTOCOL **Root);

typedef struct {
    ULL Revision;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME OpenVolume;
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;