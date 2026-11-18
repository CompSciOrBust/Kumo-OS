/*
 * Google Virtual NIC (gVNIC): what the device expects, as far as this driver
 * uses it. Written from Google's gVNIC documentation and the layouts in the
 * Linux driver (drivers/net/ethernet/google/gve, dual MIT/GPL); no code was
 * copied from it.
 *
 * Every multi-byte field the device reads or writes, in registers and in
 * memory, is big-endian. The structures below hold raw device values; convert
 * with Be16/Be32/Be64.
 */
#pragma once
#include <kumo/prelude.hpp>

namespace kumo::horizon::hznet::drivers::gvnic::abi {

    constexpr inline u16 PciVendorId = 0x1AE0;
    constexpr inline u16 PciDeviceId = 0x0042;

    constexpr inline int RegisterBar = 0;
    constexpr inline int DoorbellBar = 2;

    /* Byte swaps between the CPU (little-endian) and the device. */
    constexpr inline u16 Be16(u16 v) { return __builtin_bswap16(v); }
    constexpr inline u32 Be32(u32 v) { return __builtin_bswap32(v); }
    constexpr inline u64 Be64(u64 v) { return __builtin_bswap64(v); }

    /* BAR 0 registers. */
    constexpr inline u32 Register_DeviceStatus       = 0x00;
    constexpr inline u32 Register_DriverStatus       = 0x04;
    constexpr inline u32 Register_MaxTxQueues        = 0x08;
    constexpr inline u32 Register_MaxRxQueues        = 0x0C;
    constexpr inline u32 Register_AdminQueuePfn      = 0x10;  /* revision 0: page number of the admin queue */
    constexpr inline u32 Register_AdminQueueDoorbell = 0x14;
    constexpr inline u32 Register_AdminQueueCounter  = 0x18;  /* commands the device has completed */
    constexpr inline u32 Register_DriverVersion      = 0x1F;  /* 8 bits; the version string is written a byte at a time */
    constexpr inline u32 Register_AdminQueueBaseHigh = 0x20;  /* revision 1 and later */
    constexpr inline u32 Register_AdminQueueBaseLow  = 0x24;
    constexpr inline u32 Register_AdminQueueLength   = 0x28;  /* 16 bits */

    constexpr inline u32 DeviceStatus_ResetRequested = (1u << 1);
    constexpr inline u32 DeviceStatus_LinkUp         = (1u << 2);
    constexpr inline u32 DeviceStatus_IsReset        = (1u << 4);

    constexpr inline u32 DriverStatus_Run   = (1u << 0);
    constexpr inline u32 DriverStatus_Reset = (1u << 1);

    /* Written to a notification block's doorbell: stop it raising interrupts. */
    constexpr inline u32 IrqDoorbell_Mask = (1u << 30);

    /* The admin queue: one page of 64-byte commands. */
    constexpr inline size_t AdminQueueSize    = 0x1000;
    constexpr inline size_t AdminCommandSize  = 64;
    constexpr inline u32    AdminCommandCount = AdminQueueSize / AdminCommandSize;

    enum AdminOpcode : u32 {
        AdminOpcode_DescribeDevice           = 0x1,
        AdminOpcode_ConfigureDeviceResources = 0x2,
        AdminOpcode_RegisterPageList         = 0x3,
        AdminOpcode_CreateTxQueue            = 0x5,
        AdminOpcode_CreateRxQueue            = 0x6,
    };

    constexpr inline u32 AdminStatus_Unset  = 0;
    constexpr inline u32 AdminStatus_Passed = 1;

    enum QueueFormat : u8 {
        QueueFormat_GqiRda = 0x1,
        QueueFormat_GqiQpl = 0x2,
        QueueFormat_DqoRda = 0x3,
        QueueFormat_DqoQpl = 0x4,
    };

    /* Device options that follow the device descriptor. */
    enum DeviceOption : u16 {
        DeviceOption_GqiRawAddressing = 0x1,
        DeviceOption_GqiRda           = 0x2,
        DeviceOption_GqiQpl           = 0x3,
        DeviceOption_DqoRda           = 0x4,
        DeviceOption_ModifyRing       = 0x6,
        DeviceOption_DqoQpl           = 0x7,
        DeviceOption_JumboFrames      = 0x8,
        DeviceOption_BufferSizes      = 0xA,
    };

    struct AdminCommandHeader {
        u32 opcode;
        u32 status;
    };
    static_assert(sizeof(AdminCommandHeader) == 8);

    struct DescribeDevice {
        u64 descriptor_address;
        u32 descriptor_version;     /* 1 */
        u32 available_length;
    };

    struct DeviceDescriptor {
        u64 max_registered_pages;
        u16 reserved1;
        u16 tx_queue_entries;
        u16 rx_queue_entries;
        u16 default_num_queues;
        u16 mtu;
        u16 counters;
        u16 tx_pages_per_qpl;
        u16 rx_pages_per_qpl;
        u8  mac[6];
        u16 num_device_options;
        u16 total_length;
        u8  reserved2[6];
    };
    static_assert(sizeof(DeviceDescriptor) == 40);

    struct DeviceOptionHeader {
        u16 option_id;
        u16 option_length;
        u32 required_features_mask;
    };
    static_assert(sizeof(DeviceOptionHeader) == 8);

    struct ConfigureDeviceResources {
        u64 counter_array;
        u64 irq_doorbell_array;
        u32 num_counters;
        u32 num_irq_doorbells;
        u32 irq_doorbell_stride;
        u32 notify_block_msix_base;
        u8  queue_format;
        u8  padding[7];
    };

    struct RegisterPageList {
        u32 page_list_id;
        u32 num_pages;
        u64 page_address_list;
        u64 page_size;
    };

    struct CreateTxQueue {
        u32 queue_id;
        u32 reserved;
        u64 queue_resources;
        u64 ring;
        u32 page_list_id;
        u32 notify_id;
        u64 completion_ring;        /* DQO only */
        u16 ring_size;
        u16 completion_ring_size;   /* DQO only */
        u8  padding[4];
    };

    struct CreateRxQueue {
        u32 queue_id;
        u32 index;
        u32 reserved;
        u32 notify_id;
        u64 queue_resources;
        u64 descriptor_ring;
        u64 data_ring;
        u32 page_list_id;
        u16 ring_size;
        u16 packet_buffer_size;
        u16 buffer_ring_size;       /* DQO only */
        u8  enable_rsc;
        u8  padding1;
        u16 header_buffer_size;
        u8  padding2[2];
    };

    static_assert(sizeof(AdminCommandHeader) + sizeof(DescribeDevice)           <= AdminCommandSize);
    static_assert(sizeof(AdminCommandHeader) + sizeof(ConfigureDeviceResources) <= AdminCommandSize);
    static_assert(sizeof(AdminCommandHeader) + sizeof(RegisterPageList)         <= AdminCommandSize);
    static_assert(sizeof(AdminCommandHeader) + sizeof(CreateTxQueue)            <= AdminCommandSize);
    static_assert(sizeof(AdminCommandHeader) + sizeof(CreateRxQueue)            <= AdminCommandSize);

    /* Where the device tells the driver which doorbell and event counter a queue was given. */
    struct QueueResources {
        u32 doorbell_index;
        u32 counter_index;
        u8  reserved[56];
    };
    static_assert(sizeof(QueueResources) == 64);

    /* One notification block's interrupt doorbell, filled in by the device. Each takes a cache line. */
    struct IrqDoorbell {
        u32 index;
        u8  reserved[60];
    };
    static_assert(sizeof(IrqDoorbell) == 64);

    /* GQI transmit descriptor for a packet's first (here, only) segment. With a queue page */
    /* list, `segment_address` is a byte offset into the list rather than a memory address. */
    struct TxPacketDescriptor {
        u8  type_flags;             /* type in the high four bits: 0 is a standard packet */
        u8  l4_checksum_offset;
        u8  l4_header_offset;
        u8  descriptor_count;
        u16 length;                 /* whole packet */
        u16 segment_length;         /* this descriptor's piece */
        u64 segment_address;
    };
    static_assert(sizeof(TxPacketDescriptor) == 16);

    /* GQI receive descriptor, written by the device. */
    struct RxDescriptor {
        u8  padding[48];
        u32 rss_hash;
        u16 mss;
        u16 reserved;
        u8  header_length;
        u8  header_offset;
        u16 checksum;
        u16 length;                 /* includes the RxPad bytes in front of the frame */
        u16 flags_sequence;         /* flags in bits 3-15, sequence number (1-7) in bits 0-2 */
    };
    static_assert(sizeof(RxDescriptor) == 64);

    constexpr inline u16 RxFlag_Error            = (1u << 11);
    constexpr inline u16 RxFlag_PacketContinues  = (1u << 13);
    constexpr inline u16 RxSequenceMask          = 0x7;

    /* The device puts this many bytes in front of every received frame. */
    constexpr inline size_t RxPad = 2;

    constexpr inline u8 NextRxSequence(u8 sequence) {
        return (sequence == 7) ? 1 : sequence + 1;
    }

}
