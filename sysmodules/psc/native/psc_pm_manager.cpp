#include <kumo/prelude.hpp>
#include <kumo/mutex.hpp>
#include "psc_pm_manager.hpp"
#include "../source/psc_dependency_graph.hpp"
#include "../source/psc_log.hpp"

namespace kumo::horizon::psc::srv {


    namespace {

        /* Where a module is in the transition in progress. */
        enum Stage {
            Stage_Done,         /* not part of it, or has acknowledged */
            Stage_Waiting,      /* part of it, not asked yet: waiting for the modules it is ordered after */
            Stage_Asked,        /* its event is signalled, and the transition waits for its acknowledgement */
        };

    }

    struct ModuleEntry {
        bool used;
        u32 serial;                          /* registration number, for the log */
        PmModuleId id;
        u32 dependencies[MaxDependencies];
        size_t dependency_count;
        Event event;
        PmState state;                       /* the last state it was asked for and acknowledged (or FullAwake) */
        PmState request;                     /* what GetRequest returns: the last state it was asked for */
        PmFlagSet flags;
        Stage stage;
        u64 asked_at = 0;
    };

    namespace {

        constinit kumo::Mutex g_mutex;
        constinit CondVar g_cv;

        constinit ModuleEntry g_modules[MaxModules] = {};
        constinit u32 g_next_serial = 1;

        /* The control side's event: signalled when a transition completes, cleared when one starts. */
        constinit Event g_control_event = {};
        /* GetStateLockUpdateEvent's: never signalled, since no state is ever locked. */
        constinit Event g_state_lock_event = {};

        /* The settled state: what the last completed transition reached. */
        constinit PmState g_state = PmState_FullAwake;

        /* The transition in progress, if any. */
        constinit bool g_active = false;
        constinit PmState g_target = PmState_FullAwake;
        constinit u32 g_order = PmTransitionOrder_ToHigherPowerState;
        constinit PmFlagSet g_flags = {};
        constinit bool g_cancelled = false;
        constinit bool g_timed_out = false;
        constinit u64 g_started_at = 0;

        /* What GetResult returns. Nothing has been asked yet: success. */
        constinit Result g_result = 0;

        constexpr size_t WatchdogStackSize = (16 * 1024);
        alignas(4096) constinit u8 g_watchdog_stack[WatchdogStackSize];
        constinit Thread g_watchdog_thread = {};

        const char *StateName(PmState state) {
            switch (state) {
                case PmState_FullAwake:                   return "FullAwake";
                case PmState_MinimumAwake:                return "MinimumAwake";
                case PmState_SleepReady:                  return "SleepReady";
                case PmState_EssentialServicesSleepReady: return "EssentialServicesSleepReady";
                case PmState_EssentialServicesAwake:      return "EssentialServicesAwake";
                case PmState_ShutdownReady:               return "ShutdownReady";
                default:                                  return "?";
            }
        }

        constexpr u64 AcknowledgeTimeoutNs = 10ULL * 1000000000ULL;
        u64 NanoSecondsSince(u64 tick) { return armTicksToNs(armGetSystemTick() - tick); }
        long MilliSecondsSince(u64 tick) { return static_cast<long>(NanoSecondsSince(tick) / 1000000); }

        /* Whether every module ordered before this one has acknowledged. */
        bool IsReady(const ModuleEntry &entry) {
            return CanRequest(entry, g_modules, MaxModules,
                g_order == PmTransitionOrder_ToLowerPowerState,
                [](const ModuleEntry &other) { return other.used && other.stage != Stage_Done; });
        }

        /* Ask every waiting module that may be asked, and complete the transition if nothing is left. Lock held. */
        void Advance() {
            if (!g_active) {
                return;
            }

            bool pending = false;
            for (auto &entry : g_modules) {
                if (!entry.used) {
                    continue;
                }
                /* Asking a module completes nothing, so one pass finds everything that can be asked now. */
                if (entry.stage == Stage_Waiting && IsReady(entry)) {
                    entry.stage    = Stage_Asked;
                    entry.request  = g_target;
                    entry.flags    = g_flags;
                    entry.asked_at = armGetSystemTick();
                    KUMO_ABORT_RESULT(eventFire(&entry.event));
                    Log("[psc] asking module %u (id %u) for %s\n", entry.serial, static_cast<u32>(entry.id), StateName(g_target));
                }
                if (entry.stage != Stage_Done) {
                    pending = true;
                }
            }

            if (pending) {
                condvarWakeAll(&g_cv);
                return;
            }

            /* Every module has acknowledged. */
            g_active = false;
            g_state  = g_target;
            if (g_cancelled) {
                g_result = ResultCancelled();
            } else if (g_timed_out) {
                g_result = ResultTimedOut();
            } else {
                g_result = 0;
            }
            Log("[psc] reached %s in %ld ms (result 0x%x)\n", StateName(g_state), MilliSecondsSince(g_started_at), g_result);
            KUMO_ABORT_RESULT(eventFire(&g_control_event));
            condvarWakeAll(&g_cv);
        }

        /* Start a transition. Modules for which `include` is true take part; the others are left as they are. Lock held. */
        template<typename F>
        void Start(PmState target, u32 order, PmFlagSet flags, F include) {
            g_active     = true;
            g_target     = target;
            g_order      = order;
            g_flags      = flags;
            g_timed_out  = false;
            g_started_at = armGetSystemTick();
            g_result     = ResultInProgress();
            KUMO_ABORT_RESULT(eventClear(&g_control_event));

            for (auto &entry : g_modules) {
                if (entry.used) {
                    entry.stage = include(entry) ? Stage_Waiting : Stage_Done;
                }
            }
            Advance();
        }

        /* Whether adding a module with these dependencies keeps the graph free of cycles and within MaximumDependencyLevels. Lock held. */
        bool CheckDependencyDepth(const ModuleEntry &added) {
            const ModuleEntry *entries[MaxModules + 1];
            size_t count = 0;
            for (const auto &entry : g_modules) {
                if (entry.used) entries[count++] = std::addressof(entry);
            }
            entries[count++] = std::addressof(added);
            return IsDependencyGraphValid<MaxModules + 1>(entries, count, MaxDependencyLevels);
        }

        void WatchdogThreadFunction(void *) {
            std::scoped_lock lk(g_mutex);
            while (true) {
                /* The asked module whose time runs out first. */
                ModuleEntry *oldest = nullptr;
                for (auto &entry : g_modules) {
                    if (entry.used && entry.stage == Stage_Asked && (oldest == nullptr || entry.asked_at < oldest->asked_at)) {
                        oldest = std::addressof(entry);
                    }
                }

                if (oldest == nullptr) {
                    condvarWait(&g_cv, g_mutex.native_handle());
                    continue;
                }

                const u64 waited = NanoSecondsSince(oldest->asked_at);
                if (waited < AcknowledgeTimeoutNs) {
                    condvarWaitTimeout(&g_cv, g_mutex.native_handle(), AcknowledgeTimeoutNs - waited);
                    continue;
                }

                Log("[psc] module %u (id %u) did not acknowledge %s within %ld ms; going on without it\n",
                    oldest->serial, static_cast<u32>(oldest->id), StateName(oldest->request), static_cast<long>(AcknowledgeTimeoutNs / 1000000));
                oldest->stage = Stage_Done;
                g_timed_out = true;
                Advance();
            }
        }

    }

    void InitializeManager() {
        KUMO_ABORT_RESULT(eventCreate(&g_control_event, false));
        KUMO_ABORT_RESULT(eventCreate(&g_state_lock_event, false));
        s32 priority = 0;
        KUMO_ABORT_RESULT(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE));
        KUMO_ABORT_RESULT(threadCreate(&g_watchdog_thread, WatchdogThreadFunction, nullptr, g_watchdog_stack, sizeof(g_watchdog_stack), priority, -2));
        KUMO_ABORT_RESULT(threadStart(&g_watchdog_thread));
    }

    Result RegisterModule(ModuleEntry **out, Handle *out_event, PmModuleId id, const u32 *dependencies, size_t count) {
        std::scoped_lock lk(g_mutex);

        KUMO_UNLESS(count <= MaxDependencies, ResultInvalidArgument());
        for (size_t i = 0; i < count; ++i) {
            /* A module that depends on its own ID could never be ordered. */
            KUMO_UNLESS(dependencies[i] != static_cast<u32>(id), ResultInvalidArgument());
        }

        ModuleEntry *entry = nullptr;
        for (auto &candidate : g_modules) {
            if (!candidate.used) {
                entry = std::addressof(candidate);
                break;
            }
        }
        KUMO_UNLESS(entry != nullptr, ResultTooManyModules());

        entry->id = id;
        std::memcpy(entry->dependencies, dependencies, count * sizeof(u32));
        entry->dependency_count = count;
        KUMO_UNLESS(CheckDependencyDepth(*entry), ResultDependencyTooDeep());

        KUMO_ABORT_RESULT(eventCreate(&entry->event, false));
        entry->used    = true;
        entry->serial  = g_next_serial++;
        entry->state   = g_state;
        entry->request = g_state;
        entry->flags   = {};
        /* A module that registers during a transition takes part in it. */
        entry->stage   = g_active ? Stage_Waiting : Stage_Done;

        Log("[psc] module %u registered: id %u, %zu dependencies\n", entry->serial, static_cast<u32>(id), count);

        *out = entry;
        *out_event = entry->event.revent;

        Advance();
        KUMO_SUCCEED();
    }

    void UnregisterModule(ModuleEntry *entry, bool closed) {
        std::scoped_lock lk(g_mutex);

        if (entry->stage == Stage_Asked) {
            Log("[psc] module %u (id %u) %s while asked for %s; treated as having acknowledged\n",
                entry->serial, static_cast<u32>(entry->id), closed ? "went away" : "finalized", StateName(entry->request));
        } else {
            Log("[psc] module %u (id %u) %s\n", entry->serial, static_cast<u32>(entry->id), closed ? "went away" : "finalized");
        }

        eventClose(&entry->event);
        entry->used  = false;
        entry->stage = Stage_Done;

        /* Modules ordered after this one may be asked now. */
        Advance();
    }

    void GetModuleRequest(ModuleEntry *entry, PmState *out_state, PmFlagSet *out_flags) {
        std::scoped_lock lk(g_mutex);
        *out_state = entry->request;
        *out_flags = entry->flags;
    }

    void AcknowledgeModule(ModuleEntry *entry, bool has_state, PmState state) {
        std::scoped_lock lk(g_mutex);

        /* An acknowledgement nobody waits for (none asked, or one for a state that has since been cancelled) changes nothing. */
        if (entry->stage != Stage_Asked) {
            return;
        }
        if (has_state && state != entry->request) {
            Log("[psc] module %u (id %u) acknowledged %s, but was asked for %s; ignored\n",
                entry->serial, static_cast<u32>(entry->id), StateName(state), StateName(entry->request));
            return;
        }

        Log("[psc] module %u (id %u) acknowledged %s after %ld ms\n",
            entry->serial, static_cast<u32>(entry->id), StateName(entry->request), MilliSecondsSince(entry->asked_at));
        entry->stage = Stage_Done;
        entry->state = entry->request;
        Advance();
    }

    Handle GetControlEventHandle() {
        return g_control_event.revent;
    }

    Handle GetStateLockEventHandle() {
        return g_state_lock_event.revent;
    }

    Result DispatchRequest(u32 state, u32 order, u32 flags) {
        std::scoped_lock lk(g_mutex);

        KUMO_UNLESS(state <= PmState_ShutdownReady, ResultInvalidArgument());
        KUMO_UNLESS(order <= PmTransitionOrder_ToHigherPowerState, ResultInvalidArgument());
        KUMO_UNLESS(state == PmState_FullAwake || state == PmState_ShutdownReady, ResultStateNotSupported());
        KUMO_UNLESS(!g_active, ResultBusy());

        Log("[psc] dispatching %s (%s, flags 0x%x)\n", StateName(static_cast<PmState>(state)),
            order == PmTransitionOrder_ToLowerPowerState ? "to lower power" : "to higher power", flags);

        g_cancelled = false;
        PmFlagSet flag_set = flags;
        Start(static_cast<PmState>(state), order, flag_set, [](const ModuleEntry &) { return true; });
        KUMO_SUCCEED();
    }

    Result GetResult() {
        std::scoped_lock lk(g_mutex);
        return g_result;
    }

    PmState GetState() {
        std::scoped_lock lk(g_mutex);
        return g_state;
    }

    Result Cancel() {
        std::scoped_lock lk(g_mutex);

        /* Already awake, or already on the way there: nothing to undo. */
        if (g_active ? g_target == PmState_FullAwake : g_state == PmState_FullAwake) {
            KUMO_SUCCEED();
        }

        Log("[psc] cancelling: back to FullAwake\n");

        /* Every module that was asked for the cancelled state, or has reached another, is asked for FullAwake. Those never asked stay as they are. */
        g_cancelled = true;
        Start(PmState_FullAwake, PmTransitionOrder_ToHigherPowerState, {}, [](const ModuleEntry &entry) {
            return entry.stage == Stage_Asked || entry.request != PmState_FullAwake || entry.state != PmState_FullAwake;
        });
        KUMO_SUCCEED();
    }

    void PrintModuleInformation() {
        std::scoped_lock lk(g_mutex);

        Log("[psc] state %s%s%s\n", StateName(g_state), g_active ? ", going to " : "", g_active ? StateName(g_target) : "");
        for (const auto &entry : g_modules) {
            if (!entry.used) {
                continue;
            }
            char deps[0x80] = {};
            size_t len = 0;
            for (size_t i = 0; i < entry.dependency_count && len < sizeof(deps) - 12; ++i) {
                len += std::snprintf(deps + len, sizeof(deps) - len, "%s%u", i == 0 ? "" : ",", entry.dependencies[i]);
            }
            Log("[psc]   module %u: id %u, state %s, %s, depends on [%s]\n", entry.serial, static_cast<u32>(entry.id), StateName(entry.state),
                entry.stage == Stage_Asked ? "asked" : entry.stage == Stage_Waiting ? "waiting" : "idle", deps);
        }
    }

}
