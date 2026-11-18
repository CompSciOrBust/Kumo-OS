/*
 * NVMe driver (PCI class 01:08:02). Covers AWS Nitro (EBS and instance
 * store), GCP machine types that expose Persistent Disk as NVMe (T2A among
 * them), and QEMU's emulated controller.
 *
 * Deliberately minimal:
 *   - one controller, its first active namespace
 *   - one I/O queue pair, one command in flight
 *   - completions are polled; no interrupts
 *   - every transfer is copied through a page-aligned bounce buffer
 */
#include "fs_filesystem.hpp"
#include <kumo/pci.hpp>
#include <kumo/platform.hpp>
#include "../fs_drivers.hpp"
#include "../../fs_log.hpp"
#include "../../blockdev/fs_block_device_registry.hpp"

namespace kumo::horizon::hzfs::drivers::nvme {

    namespace {

        constexpr u8 PciClass_MassStorage = 0x01;
        constexpr u8 PciSubClass_Nvm      = 0x08;
        constexpr u8 PciProgIf_Nvme       = 0x02;

        constexpr size_t PageSize = 0x1000;

        /* Controller registers. */
        constexpr u32 Reg_Cap  = 0x00;
        constexpr u32 Reg_Vs   = 0x08;
        constexpr u32 Reg_Cc   = 0x14;
        constexpr u32 Reg_Csts = 0x1C;
        constexpr u32 Reg_Aqa  = 0x24;
        constexpr u32 Reg_Asq  = 0x28;
        constexpr u32 Reg_Acq  = 0x30;
        constexpr u32 Reg_Doorbells = 0x1000;

        constexpr u32 Cc_Enable = (1 << 0);
        constexpr u32 Cc_IoSqEntrySize = (6 << 16);  /* 2^6 = 64 bytes */
        constexpr u32 Cc_IoCqEntrySize = (4 << 20);  /* 2^4 = 16 bytes */

        constexpr u32 Csts_Ready = (1 << 0);
        constexpr u32 Csts_Fatal = (1 << 1);

        /* Opcodes. */
        constexpr u8 Admin_CreateIoSq  = 0x01;
        constexpr u8 Admin_CreateIoCq  = 0x05;
        constexpr u8 Admin_Identify    = 0x06;
        constexpr u8 Admin_SetFeatures = 0x09;

        constexpr u8 Io_Flush = 0x00;
        constexpr u8 Io_Write = 0x01;
        constexpr u8 Io_Read  = 0x02;

        constexpr u32 Identify_Namespace        = 0;
        constexpr u32 Identify_Controller       = 1;
        constexpr u32 Identify_ActiveNamespaces = 2;

        constexpr u32 Feature_NumberOfQueues = 7;

        struct SubmissionEntry {
            u8  opcode;
            u8  flags;
            u16 command_id;
            u32 nsid;
            u64 reserved;
            u64 metadata;
            u64 prp1;
            u64 prp2;
            u32 cdw10;
            u32 cdw11;
            u32 cdw12;
            u32 cdw13;
            u32 cdw14;
            u32 cdw15;
        };
        static_assert(sizeof(SubmissionEntry) == 64);

        struct CompletionEntry {
            u32 result;
            u32 reserved;
            u16 sq_head;
            u16 sq_id;
            u16 command_id;
            u16 status;     /* bit 0 is the phase tag */
        };
        static_assert(sizeof(CompletionEntry) == 16);

        constexpr u16 AdminQueueEntries = 32;
        constexpr u16 IoQueueEntries    = 64;
        static_assert(IoQueueEntries * sizeof(SubmissionEntry) <= PageSize);

        constexpr u16 IoQueueId = 1;

        /* Largest single transfer we issue. The controller's own limit (MDTS) may lower it. */
        constexpr size_t MaxTransferPages = 32;

        /* Memory the controller reads and writes directly. It lives in our image, so it is always */
        /* resident and never moves; its physical addresses are looked up once at start-up. */
        alignas(PageSize) constinit u8 g_admin_sq_memory[PageSize] = {};
        alignas(PageSize) constinit u8 g_admin_cq_memory[PageSize] = {};
        alignas(PageSize) constinit u8 g_io_sq_memory[PageSize]    = {};
        alignas(PageSize) constinit u8 g_io_cq_memory[PageSize]    = {};
        alignas(PageSize) constinit u8 g_prp_list_memory[PageSize] = {};
        alignas(PageSize) constinit u8 g_transfer_memory[MaxTransferPages * PageSize] = {};

        inline void MemoryBarrier() {
            __asm__ __volatile__("dmb sy" ::: "memory");
        }

        struct Queue {
            SubmissionEntry *sq;
            const volatile CompletionEntry *cq;
            volatile u32 *sq_doorbell;
            volatile u32 *cq_doorbell;
            u16 entries;
            u16 sq_tail;
            u16 cq_head;
            u16 phase;
        };

        class NvmeBlockDevice final : public blockdev::IBlockDevice {
            private:
                uintptr_t m_registers;
                u32 m_doorbell_stride;
                u64 m_timeout_ticks;
                Queue m_admin;
                Queue m_io;
                u16 m_next_command_id;
                u32 m_nsid;
                u32 m_block_size;
                u64 m_block_count;
                size_t m_max_transfer_size;
                u64 m_prp_list_physical;
                u64 m_transfer_physical[MaxTransferPages];
                kumo::Mutex m_mutex;
            public:
                NvmeBlockDevice() : m_registers(0), m_doorbell_stride(0), m_timeout_ticks(), m_admin(), m_io(), m_next_command_id(0), m_nsid(0), m_block_size(0), m_block_count(0), m_max_transfer_size(0), m_prp_list_physical(0), m_transfer_physical(), m_mutex() { /* ... */ }

                Result Initialize(const kumo::pci::DeviceInfo &pci_device);
            public:
                virtual const char *GetName() const override { return "nvme0n1"; }
                virtual u32 GetBlockSize() const override { return m_block_size; }
                virtual u64 GetBlockCount() const override { return m_block_count; }

                virtual Result Read(u64 lba, void *buffer, u32 block_count) override;
                virtual Result Write(u64 lba, const void *buffer, u32 block_count) override;
                virtual Result Flush() override;
            private:
                template<typename T> T ReadRegister(u32 offset) const { return *reinterpret_cast<const volatile T *>(m_registers + offset); }
                template<typename T> void WriteRegister(u32 offset, T value) const { *reinterpret_cast<volatile T *>(m_registers + offset) = value; }

                Result WaitReady(bool ready);
                void SetupQueue(Queue *queue, u16 queue_id, void *sq_memory, void *cq_memory, u16 entries);
                Result Execute(Queue *queue, SubmissionEntry *command, u32 *out_result = nullptr);
                Result Identify(u32 cns, u32 nsid);
                Result Transfer(u8 opcode, u64 lba, u32 block_count);
                void SetTransferPointers(SubmissionEntry *command, size_t size);
        };

        Result NvmeBlockDevice::WaitReady(bool ready) {
            const u64 start = armGetSystemTick();
            while (true) {
                const u32 csts = this->ReadRegister<u32>(Reg_Csts);
                KUMO_UNLESS((csts & Csts_Fatal) == 0, fs::ResultUnexpected());
                KUMO_SUCCEED_IF(((csts & Csts_Ready) != 0) == ready);

                KUMO_UNLESS(armGetSystemTick() - start < m_timeout_ticks, kumo::KernelError(117));
                svcSleepThread(1000000);
            }
        }

        void NvmeBlockDevice::SetupQueue(Queue *queue, u16 queue_id, void *sq_memory, void *cq_memory, u16 entries) {
            std::memset(sq_memory, 0, PageSize);
            std::memset(cq_memory, 0, PageSize);

            queue->sq          = static_cast<SubmissionEntry *>(sq_memory);
            queue->cq          = static_cast<const volatile CompletionEntry *>(cq_memory);
            queue->sq_doorbell = reinterpret_cast<volatile u32 *>(m_registers + Reg_Doorbells + (2 * queue_id)     * m_doorbell_stride);
            queue->cq_doorbell = reinterpret_cast<volatile u32 *>(m_registers + Reg_Doorbells + (2 * queue_id + 1) * m_doorbell_stride);
            queue->entries     = entries;
            queue->sq_tail     = 0;
            queue->cq_head     = 0;
            queue->phase       = 1;
        }

        /* Submit one command and wait for its completion. The caller holds m_mutex (or is single-threaded start-up). */
        Result NvmeBlockDevice::Execute(Queue *queue, SubmissionEntry *command, u32 *out_result) {
            command->command_id = m_next_command_id++;

            /* Submit. */
            queue->sq[queue->sq_tail] = *command;
            queue->sq_tail = (queue->sq_tail + 1) % queue->entries;
            MemoryBarrier();
            *queue->sq_doorbell = queue->sq_tail;

            /* Poll for the completion: it is ours once its phase tag matches the phase we expect. */
            const u64 start = armGetSystemTick();
            const volatile CompletionEntry *entry = queue->cq + queue->cq_head;
            u32 spins = 0;
            while ((entry->status & 1) != queue->phase) {
                if (++spins >= 2000) {
                    KUMO_UNLESS(armGetSystemTick() - start < m_timeout_ticks, kumo::KernelError(117));
                    svcSleepThread(100000);
                }
            }
            MemoryBarrier();

            const u16 status     = entry->status >> 1;
            const u16 command_id = entry->command_id;
            const u32 result     = entry->result;

            /* Consume the entry. */
            if (++queue->cq_head == queue->entries) {
                queue->cq_head = 0;
                queue->phase  ^= 1;
            }
            *queue->cq_doorbell = queue->cq_head;

            KUMO_ASSERT(command_id == command->command_id);

            if (status != 0) {
                /* TODO: Map NVMe status codes onto distinct results. */
                Log("[fs] nvme: opcode 0x%02x failed, status 0x%04x\n", command->opcode, status);
                KUMO_THROW(fs::ResultUnexpected());
            }

            if (out_result != nullptr) {
                *out_result = result;
            }
            KUMO_SUCCEED();
        }

        /* Point a command at the first `size` bytes of the transfer buffer. */
        void NvmeBlockDevice::SetTransferPointers(SubmissionEntry *command, size_t size) {
            const size_t pages = (size + PageSize - 1) / PageSize;
            KUMO_ASSERT(0 < pages && pages <= MaxTransferPages);

            command->prp1 = m_transfer_physical[0];
            if (pages == 2) {
                command->prp2 = m_transfer_physical[1];
            } else if (pages > 2) {
                u64 *list = reinterpret_cast<u64 *>(g_prp_list_memory);
                for (size_t i = 1; i < pages; ++i) {
                    list[i - 1] = m_transfer_physical[i];
                }
                command->prp2 = m_prp_list_physical;
            }
        }

        /* Run an Identify command; the 4096-byte answer lands at the start of the transfer buffer. */
        Result NvmeBlockDevice::Identify(u32 cns, u32 nsid) {
            SubmissionEntry command = {};
            command.opcode = Admin_Identify;
            command.nsid   = nsid;
            command.cdw10  = cns;
            this->SetTransferPointers(std::addressof(command), PageSize);
            KUMO_RETURN(this->Execute(std::addressof(m_admin), std::addressof(command)));
        }

        Result NvmeBlockDevice::Initialize(const kumo::pci::DeviceInfo &pci_device) {
            /* Map the controller registers and let the device do DMA. */
            size_t bar_size;
            KUMO_TRY(kumo::pci::MapBar(std::addressof(m_registers), std::addressof(bar_size), pci_device.location, 0));
            kumo::pci::EnableMemoryAndBusMaster(pci_device.location);

            /* Look up the physical addresses of our DMA memory. */
            u64 admin_sq_physical, admin_cq_physical, io_sq_physical, io_cq_physical;
            KUMO_TRY(kumo::platform::GetPhysicalAddress(std::addressof(admin_sq_physical), g_admin_sq_memory));
            KUMO_TRY(kumo::platform::GetPhysicalAddress(std::addressof(admin_cq_physical), g_admin_cq_memory));
            KUMO_TRY(kumo::platform::GetPhysicalAddress(std::addressof(io_sq_physical),    g_io_sq_memory));
            KUMO_TRY(kumo::platform::GetPhysicalAddress(std::addressof(io_cq_physical),    g_io_cq_memory));
            KUMO_TRY(kumo::platform::GetPhysicalAddress(std::addressof(m_prp_list_physical), g_prp_list_memory));
            for (size_t i = 0; i < MaxTransferPages; ++i) {
                KUMO_TRY(kumo::platform::GetPhysicalAddress(std::addressof(m_transfer_physical[i]), g_transfer_memory + i * PageSize));
            }

            /* Read the controller's capabilities. */
            const u64 cap = this->ReadRegister<u64>(Reg_Cap);
            const u32 max_queue_entries = static_cast<u32>(cap & 0xFFFF) + 1;
            const u32 timeout_500ms     = static_cast<u32>((cap >> 24) & 0xFF);
            const u32 min_page_shift    = 12 + static_cast<u32>((cap >> 48) & 0xF);
            m_doorbell_stride = 4u << ((cap >> 32) & 0xF);
            m_timeout_ticks         = armGetSystemTickFreq() * (500 * std::max<u32>(timeout_500ms, 1) + 5000) / 1000;

            /* We use 4 KiB pages and fixed queue sizes. */
            KUMO_UNLESS(min_page_shift == 12,                kumo::KernelError(127));
            KUMO_UNLESS(max_queue_entries >= IoQueueEntries, kumo::KernelError(127));
            KUMO_UNLESS(bar_size >= Reg_Doorbells + 4 * m_doorbell_stride, kumo::KernelError(127));

            /* Reset the controller. */
            this->WriteRegister<u32>(Reg_Cc, this->ReadRegister<u32>(Reg_Cc) & ~Cc_Enable);
            KUMO_TRY(this->WaitReady(false));

            /* Give it the admin queues and enable it. */
            this->SetupQueue(std::addressof(m_admin), 0, g_admin_sq_memory, g_admin_cq_memory, AdminQueueEntries);
            this->WriteRegister<u32>(Reg_Aqa, (static_cast<u32>(AdminQueueEntries - 1) << 16) | (AdminQueueEntries - 1));
            this->WriteRegister<u64>(Reg_Asq, admin_sq_physical);
            this->WriteRegister<u64>(Reg_Acq, admin_cq_physical);
            this->WriteRegister<u32>(Reg_Cc, Cc_IoSqEntrySize | Cc_IoCqEntrySize | Cc_Enable);
            KUMO_TRY(this->WaitReady(true));

            /* Identify the controller: we need its transfer size limit. */
            KUMO_TRY(this->Identify(Identify_Controller, 0));
            const u8 mdts = g_transfer_memory[77];
            m_max_transfer_size = MaxTransferPages * PageSize;
            if (mdts != 0 && mdts < 16) {
                m_max_transfer_size = std::min<size_t>(m_max_transfer_size, PageSize << mdts);
            }

            /* Pick the first active namespace. */
            KUMO_TRY(this->Identify(Identify_ActiveNamespaces, 0));
            std::memcpy(std::addressof(m_nsid), g_transfer_memory, sizeof(m_nsid));
            KUMO_UNLESS(m_nsid != 0, kumo::KernelError(121));

            /* Read its geometry. */
            KUMO_TRY(this->Identify(Identify_Namespace, m_nsid));
            u64 namespace_size;
            std::memcpy(std::addressof(namespace_size), g_transfer_memory, sizeof(namespace_size));
            const u8 format_index = g_transfer_memory[26] & 0xF;
            const u8 lba_shift    = g_transfer_memory[128 + 4 * format_index + 2];
            KUMO_UNLESS(9 <= lba_shift && (1u << lba_shift) <= blockdev::MaxBlockSize, kumo::KernelError(127));

            m_block_size  = 1u << lba_shift;
            m_block_count = namespace_size;

            /* Ask for one I/O queue pair, then create it: completion queue first. */
            {
                SubmissionEntry command = {};
                command.opcode = Admin_SetFeatures;
                command.cdw10  = Feature_NumberOfQueues;
                command.cdw11  = 0;     /* zero-based: one submission queue, one completion queue */
                KUMO_TRY(this->Execute(std::addressof(m_admin), std::addressof(command)));
            }

            this->SetupQueue(std::addressof(m_io), IoQueueId, g_io_sq_memory, g_io_cq_memory, IoQueueEntries);
            {
                SubmissionEntry command = {};
                command.opcode = Admin_CreateIoCq;
                command.prp1   = io_cq_physical;
                command.cdw10  = (static_cast<u32>(IoQueueEntries - 1) << 16) | IoQueueId;
                command.cdw11  = 1;     /* physically contiguous, interrupts disabled */
                KUMO_TRY(this->Execute(std::addressof(m_admin), std::addressof(command)));
            }
            {
                SubmissionEntry command = {};
                command.opcode = Admin_CreateIoSq;
                command.prp1   = io_sq_physical;
                command.cdw10  = (static_cast<u32>(IoQueueEntries - 1) << 16) | IoQueueId;
                command.cdw11  = (static_cast<u32>(IoQueueId) << 16) | 1;   /* its completion queue; physically contiguous */
                KUMO_TRY(this->Execute(std::addressof(m_admin), std::addressof(command)));
            }

            const u32 version = this->ReadRegister<u32>(Reg_Vs);
            Log("[fs] nvme: controller %04x:%04x at %02x:%02x.%x, NVMe %u.%u, namespace %u\n", pci_device.vendor_id, pci_device.device_id, pci_device.location.bus, pci_device.location.device, pci_device.location.function, version >> 16, (version >> 8) & 0xFF, m_nsid);
            KUMO_SUCCEED();
        }

        /* Read or write `block_count` blocks between the device and the transfer buffer. */
        Result NvmeBlockDevice::Transfer(u8 opcode, u64 lba, u32 block_count) {
            SubmissionEntry command = {};
            command.opcode = opcode;
            command.nsid   = m_nsid;
            command.cdw10  = static_cast<u32>(lba);
            command.cdw11  = static_cast<u32>(lba >> 32);
            command.cdw12  = block_count - 1;
            this->SetTransferPointers(std::addressof(command), static_cast<size_t>(block_count) * m_block_size);
            KUMO_RETURN(this->Execute(std::addressof(m_io), std::addressof(command)));
        }

        Result NvmeBlockDevice::Read(u64 lba, void *buffer, u32 block_count) {
            KUMO_UNLESS(lba <= m_block_count && block_count <= m_block_count - lba, fs::ResultOutOfRange());

            std::scoped_lock lk(m_mutex);

            const u32 max_blocks = static_cast<u32>(m_max_transfer_size / m_block_size);
            u8 *dst = static_cast<u8 *>(buffer);
            while (block_count > 0) {
                const u32 cur = std::min(block_count, max_blocks);
                KUMO_TRY(this->Transfer(Io_Read, lba, cur));

                const size_t size = static_cast<size_t>(cur) * m_block_size;
                std::memcpy(dst, g_transfer_memory, size);

                dst         += size;
                lba         += cur;
                block_count -= cur;
            }
            KUMO_SUCCEED();
        }

        Result NvmeBlockDevice::Write(u64 lba, const void *buffer, u32 block_count) {
            KUMO_UNLESS(lba <= m_block_count && block_count <= m_block_count - lba, fs::ResultOutOfRange());

            std::scoped_lock lk(m_mutex);

            const u32 max_blocks = static_cast<u32>(m_max_transfer_size / m_block_size);
            const u8 *src = static_cast<const u8 *>(buffer);
            while (block_count > 0) {
                const u32 cur     = std::min(block_count, max_blocks);
                const size_t size = static_cast<size_t>(cur) * m_block_size;
                std::memcpy(g_transfer_memory, src, size);
                KUMO_TRY(this->Transfer(Io_Write, lba, cur));

                src         += size;
                lba         += cur;
                block_count -= cur;
            }
            KUMO_SUCCEED();
        }

        Result NvmeBlockDevice::Flush() {
            std::scoped_lock lk(m_mutex);

            SubmissionEntry command = {};
            command.opcode = Io_Flush;
            command.nsid   = m_nsid;
            KUMO_RETURN(this->Execute(std::addressof(m_io), std::addressof(command)));
        }

        constinit std::optional<NvmeBlockDevice> g_device = {};

    }

    Result Probe() {
        /* No reachable PCI host means no NVMe; that is not an error. */
        KUMO_SUCCEED_IF(R_FAILED(kumo::pci::Initialize()));

        kumo::pci::DeviceInfo pci_device;
        KUMO_SUCCEED_IF(!kumo::pci::FindDeviceByClass(std::addressof(pci_device), PciClass_MassStorage, PciSubClass_Nvm, PciProgIf_Nvme, 0));

        /* TODO: Further controllers and namespaces. */
        auto *device = &g_device.emplace();
        KUMO_TRY(device->Initialize(pci_device));

        blockdev::RegisterBlockDevice(device);
        KUMO_SUCCEED();
    }

}
