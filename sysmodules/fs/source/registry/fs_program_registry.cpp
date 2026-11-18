#include "fs_filesystem.hpp"
#include "fs_program_registry.hpp"

namespace kumo::horizon::hzfs::registry {

    namespace {

        constexpr size_t MaxPrograms = 96;

        struct Entry {
            bool in_use;
            ProgramInfo info;
        };

        constinit Entry g_entries[MaxPrograms] = {};
        constinit kumo::Mutex g_mutex;

        Entry *Find(u64 process_id) {
            for (auto &entry : g_entries) {
                if (entry.in_use && entry.info.process_id == process_id) {
                    return std::addressof(entry);
                }
            }
            return nullptr;
        }

    }

    Result RegisterProgram(u64 process_id, ncm::ProgramId program_id, ncm::StorageId storage_id) {
        std::scoped_lock lk(g_mutex);

        KUMO_UNLESS(Find(process_id) == nullptr, fs::ResultInvalidArgument());

        for (auto &entry : g_entries) {
            if (!entry.in_use) {
                entry.in_use = true;
                entry.info   = { process_id, program_id, storage_id };
                KUMO_SUCCEED();
            }
        }

        KUMO_THROW(fs::ResultAllocationMemoryFailed());
    }

    Result UnregisterProgram(u64 process_id) {
        std::scoped_lock lk(g_mutex);

        Entry *entry = Find(process_id);
        KUMO_UNLESS(entry != nullptr, fs::ResultInvalidArgument());

        entry->in_use = false;
        KUMO_SUCCEED();
    }

    bool GetProgramInfo(ProgramInfo *out, u64 process_id) {
        std::scoped_lock lk(g_mutex);

        const Entry *entry = Find(process_id);
        if (entry == nullptr) {
            return false;
        }

        *out = entry->info;
        return true;
    }

}
