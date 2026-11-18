/*
 * Test client for the psc system module. Runs as a KIP and reports each check
 * on the kernel debug log.
 *
 * It plays both sides. On psc:m it registers fake modules through
 * libnx's PscPmModule client, each
 * served by its own thread, with this dependency graph (an arrow is "depends
 * on"):
 *
 *     Nifm -> WlanSockets -> Fs        Erpt (no dependencies)
 *                   Lm   -> Fs
 *
 * On psc:c, which libnx does not wrap, it is a raw client. It dispatches
 * ShutdownReady and FullAwake and checks the order requests arrive in, that the
 * controller is told only after every acknowledgement, that sleep states are
 * refused, Cancel, modules that go away mid-request (an object closed, and a
 * whole session closed), a module that never acknowledges (the timeout), and
 * registration errors. Nothing outside the guest is needed.
 */
#include <switch.h>
#include <kumo/assert.hpp>
#include <kumo/result.hpp>
#include <kumo/mutex.hpp>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <memory>
#include <mutex>

/* libnx leaves the service headers out of switch.h, and they have no C linkage guard of their own. */
extern "C" {
    #include <switch/services/psc.h>
}

namespace psc {
    using PmModuleId = PscPmModuleId;
    using PmState = PscPmState;
    using PmFlagSet = u32;
    constexpr u32 MaximumDependencyLevels = 20;
    constexpr auto PmState_FullAwake = PscPmState_Awake;
    constexpr auto PmState_MinimumAwake = PscPmState_ReadyAwaken;
    constexpr auto PmState_EssentialServicesAwake = PscPmState_ReadyAwakenCritical;
    constexpr auto PmState_ShutdownReady = PscPmState_ReadyShutdown;
    constexpr auto PmState_Unknown = static_cast<PscPmState>(6);
    class PmModule {
        PscPmModule module_{};
        bool initialized_ = false;
    public:
        ~PmModule() { Close(); }
        Result Initialize(PmModuleId id, const PmModuleId *deps, u32 count) {
            static_assert(sizeof(PmModuleId) == sizeof(u32));
            const Result rc = pscmGetPmModule(&module_, id, reinterpret_cast<const u32 *>(deps), count, false);
            initialized_ = R_SUCCEEDED(rc);
            return rc;
        }
        Result Finalize() {
            const Result rc = pscPmModuleFinalize(&module_);
            if (R_SUCCEEDED(rc)) Close();
            return rc;
        }
        Result GetRequest(PmState *state, PmFlagSet *flags) { return pscPmModuleGetRequest(&module_, state, flags); }
        Result Acknowledge(PmState state) { return pscPmModuleAcknowledge(&module_, state); }
        Event *GetEventPointer() { return &module_.event; }
        void Close() { if (initialized_) { pscPmModuleClose(&module_); initialized_ = false; } }
    };
}

namespace {

    namespace {

        /* psc's results (module 138): the description is the value shifted down by 9 bits. */
        constexpr u32 ResultStateNotSupported = 0xC88A;   /* 2138-0100 */
        constexpr u32 ResultInvalidArgument   = 0xCA8A;   /* 2138-0101 */
        constexpr u32 ResultBusy              = 0xCC8A;   /* 2138-0102 */
        constexpr u32 ResultInProgress        = 0xCE8A;   /* 2138-0103 */
        constexpr u32 ResultCancelled         = 0xD08A;   /* 2138-0104 */
        constexpr u32 ResultTimedOut          = 0xD28A;   /* 2138-0105 */
        constexpr u32 ResultDependencyTooDeep = 0xD68A;   /* 2138-0107 */
        constexpr u32 ResultNotSupported      = 0xD88A;   /* 2138-0108 */

        /* psc's acknowledgement timeout (sysmodules/psc/source/psc_pm_manager.hpp). */
        constexpr s64 AcknowledgeTimeoutMs = 10000;

        constexpr u32 OrderToLower  = 0;
        constexpr u32 OrderToHigher = 1;

        constinit int g_failures = 0;
        constinit int g_checks   = 0;

        void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

        void Log(const char *fmt, ...) {
            char buf[0x100];
            std::va_list vl;
            va_start(vl, fmt);
            const int len = std::vsnprintf(buf, sizeof(buf), fmt, vl);
            va_end(vl);
            ::svcOutputDebugString(buf, std::min<size_t>(len, sizeof(buf) - 1));
        }

        /* `value` is whatever explains a failure: usually a result code. */
        void Check(const char *what, bool ok, long value = 0) {
            ++g_checks;
            if (!ok) {
                ++g_failures;
            }
            Log("[psc_test] %s: %s (0x%lx)\n", ok ? "PASS" : "FAIL", what, value);
        }

        s64 ElapsedMilliSeconds(u64 since) {
            return static_cast<s64>(armTicksToNs(svcGetSystemTick() - since) / 1000000);
        }

        /* ===== Recording what modules see ===== */

        /* One counter for every arrival and acknowledgement, so that orders can be compared across threads. */
        constinit kumo::Mutex g_record_mutex;
        constinit int g_sequence = 0;

        int NextSequence() {
            std::scoped_lock lk(g_record_mutex);
            return ++g_sequence;
        }

        /* ===== Fake modules on psc::PmModule ===== */

        enum Mode {
            Mode_Normal,            /* acknowledge every request */
            Mode_Disconnect,        /* on ShutdownReady, destroy the module object without Finalize */
            Mode_IgnoreShutdown,    /* never acknowledge ShutdownReady */
        };

        struct FakeModule {
            const char *name;
            psc::PmModuleId id;
            psc::PmModuleId dependencies[2];
            u32 dependency_count;
            Mode mode;

            psc::PmModule module;
            bool registered;
            Thread thread;
            alignas(0x1000) u8 stack[16 * 1024];

            /* Behaviour, set by the main thread between transitions. */
            std::atomic<int> delay_ms;
            std::atomic<bool> hold;
            kumo::Mutex release_mutex;
            CondVar release_cv;
            bool released;

            /* What it saw since the last Reset, under g_record_mutex. */
            int arrivals;
            int first_arrival;      /* sequence number of the first request since Reset */
            int last_arrival;
            int acks;
            int last_ack;           /* sequence number recorded just before the last acknowledgement */
            psc::PmState first_state;
            psc::PmState last_state;
            bool closed;

            psc::PmModule &Module() { return module; }

            void Reset() {
                std::scoped_lock lk(g_record_mutex);
                arrivals = first_arrival = last_arrival = acks = last_ack = 0;
                first_state = last_state = psc::PmState_Unknown;
            }
        };

        FakeModule g_nifm    = { .name = "Nifm",        .id = PscPmModuleId_Nifm,        .dependencies = { PscPmModuleId_WlanSockets }, .dependency_count = 1, .mode = Mode_Normal };
        FakeModule g_sockets = { .name = "WlanSockets", .id = PscPmModuleId_WlanSockets, .dependencies = { PscPmModuleId_Fs },          .dependency_count = 1, .mode = Mode_Normal };
        FakeModule g_fs      = { .name = "Fs",          .id = PscPmModuleId_Fs,          .dependencies = {},                             .dependency_count = 0, .mode = Mode_Normal };
        FakeModule g_lm      = { .name = "Lm",          .id = PscPmModuleId_Lm,          .dependencies = { PscPmModuleId_Fs },          .dependency_count = 1, .mode = Mode_Normal };
        FakeModule g_erpt    = { .name = "Erpt",        .id = PscPmModuleId_Erpt,        .dependencies = {},                             .dependency_count = 0, .mode = Mode_Normal };
        /* Added later: one that goes away mid-request, and one that never acknowledges ShutdownReady. */
        FakeModule g_bcat    = { .name = "Bcat",        .id = PscPmModuleId_Bcat,        .dependencies = { PscPmModuleId_Fs },          .dependency_count = 1, .mode = Mode_Disconnect };
        FakeModule g_friends = { .name = "Friends",     .id = PscPmModuleId_Friends,     .dependencies = {},                             .dependency_count = 0, .mode = Mode_IgnoreShutdown };

        FakeModule *const g_graph[] = { std::addressof(g_nifm), std::addressof(g_sockets), std::addressof(g_fs), std::addressof(g_lm), std::addressof(g_erpt) };

        void FakeModuleThread(void *arg) {
            auto &f = *static_cast<FakeModule *>(arg);
            auto *event = f.Module().GetEventPointer();

            while (true) {
                eventWait(event, UINT64_MAX);
                eventClear(event);

                psc::PmState state;
                psc::PmFlagSet flags;
                if (R_FAILED(f.Module().GetRequest(std::addressof(state), std::addressof(flags)))) {
                    Log("[psc_test] %s: GetRequest failed\n", f.name);
                    continue;
                }

                {
                    std::scoped_lock lk(g_record_mutex);
                    const int seq = ++g_sequence;
                    if (f.arrivals++ == 0) {
                        f.first_arrival = seq;
                        f.first_state   = state;
                    }
                    f.last_arrival = seq;
                    f.last_state   = state;
                }

                if (state == psc::PmState_ShutdownReady && f.mode == Mode_Disconnect) {
                    /* Gone without a word: the object is closed, the session stays. */
                    f.Module().Close();
                    {
                        std::scoped_lock lk(g_record_mutex);
                        f.closed = true;
                    }
                    return;
                }
                if (state == psc::PmState_ShutdownReady && f.mode == Mode_IgnoreShutdown) {
                    continue;
                }

                if (f.hold) {
                    std::scoped_lock lk(f.release_mutex);
                    while (!f.released) {
                        condvarWait(&f.release_cv, f.release_mutex.native_handle());
                    }
                    f.released = false;
                }

                const int delay = f.delay_ms;
                if (delay > 0) {
                    svcSleepThread(INT64_C(1000000) * (delay));
                }

                {
                    std::scoped_lock lk(g_record_mutex);
                    ++f.acks;
                    f.last_ack = ++g_sequence;
                }
                if (R_FAILED(f.Module().Acknowledge(state))) {
                    Log("[psc_test] %s: Acknowledge failed\n", f.name);
                }
            }
        }

        void Release(FakeModule &f) {
            std::scoped_lock lk(f.release_mutex);
            f.released = true;
            condvarWakeOne(&f.release_cv);
        }

        Result Register(FakeModule &f) {
            Result rc = f.Module().Initialize(f.id, f.dependencies, f.dependency_count);
            if (R_FAILED(rc)) {
                f.Module().Close();
                return rc;
            }
            f.registered = true;
            f.Reset();
            KUMO_ABORT_RESULT(threadCreate(&f.thread, FakeModuleThread, &f, f.stack, sizeof(f.stack), 49, -2));
            KUMO_ABORT_RESULT(threadStart(&f.thread));
            return 0;
        }

        int Arrivals(FakeModule &f)     { std::scoped_lock lk(g_record_mutex); return f.arrivals; }
        int Acks(FakeModule &f)         { std::scoped_lock lk(g_record_mutex); return f.acks; }
        int FirstArrival(FakeModule &f) { std::scoped_lock lk(g_record_mutex); return f.first_arrival; }
        int LastAck(FakeModule &f)      { std::scoped_lock lk(g_record_mutex); return f.last_ack; }

        /* Polls until `f` has `n` arrivals (or acknowledgements), for up to a few seconds. */
        bool WaitFor(FakeModule &f, int n, bool acks) {
            const auto start = svcGetSystemTick();
            while (ElapsedMilliSeconds(start) < 3000) {
                if ((acks ? Acks(f) : Arrivals(f)) >= n) {
                    return true;
                }
                svcSleepThread(INT64_C(1000000) * (5));
            }
            return false;
        }

        /* ===== A module on its own psc:m session, as libnx's calls would be made without a domain ===== */

        struct RawModule {
            ::Service service;
            ::Service module;
            ::Event event;
        };

        Result OpenRawModule(RawModule *out, u32 id, const u32 *dependencies, size_t count) {
            Handle handle;
            KUMO_TRY(smGetServiceOriginal(std::addressof(handle), smEncodeName("psc:m")));
            ::serviceCreate(std::addressof(out->service), handle);

            KUMO_TRY(serviceDispatch(std::addressof(out->service), 0,
                .out_num_objects = 1,
                .out_objects     = std::addressof(out->module),
            ));

            ::Handle event_handle = INVALID_HANDLE;
            KUMO_TRY(serviceDispatchIn(std::addressof(out->module), 0, id,
                .buffer_attrs     = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
                .buffers          = { { dependencies, count * sizeof(u32) } },
                .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
                .out_handles      = std::addressof(event_handle),
            ));
            ::eventLoadRemote(std::addressof(out->event), event_handle, false);
            KUMO_SUCCEED();
        }

        void CloseRawModule(RawModule *m) {
            ::eventClose(std::addressof(m->event));
            ::serviceClose(std::addressof(m->module));
            ::serviceClose(std::addressof(m->service));
        }

        /* ===== psc:c, raw ===== */

        constinit ::Service g_control = {};
        constinit ::Event g_control_event = {};

        Result ControlConnect() {
            Handle handle;
            KUMO_TRY(smGetServiceOriginal(std::addressof(handle), smEncodeName("psc:c")));
            ::serviceCreate(std::addressof(g_control), handle);
            KUMO_SUCCEED();
        }

        Result ControlInitialize() {
            ::Handle event_handle = INVALID_HANDLE;
            KUMO_TRY(serviceDispatch(std::addressof(g_control), 0,
                .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
                .out_handles      = std::addressof(event_handle),
            ));
            ::eventLoadRemote(std::addressof(g_control_event), event_handle, false);
            KUMO_SUCCEED();
        }

        Result DispatchRequest(u32 state, u32 order, u32 flags = 0) {
            const struct { u32 state; u32 order; u32 flags; } in = { state, order, flags };
            KUMO_RETURN(serviceDispatchIn(std::addressof(g_control), 1, in));
        }

        Result GetResult() {
            KUMO_RETURN(serviceDispatch(std::addressof(g_control), 2));
        }

        u32 GetState() {
            u32 state = 0xFFFFFFFF;
            if (R_FAILED(serviceDispatchOut(std::addressof(g_control), 3, state))) {
                return 0xFFFFFFFF;
            }
            return state;
        }

        Result Cancel() {
            KUMO_RETURN(serviceDispatch(std::addressof(g_control), 4));
        }

        Result PrintModuleInformation() {
            KUMO_RETURN(serviceDispatch(std::addressof(g_control), 5));
        }

        Result GetModuleInformation() {
            u8 before[0x100], after[0x100];
            struct { u8 info[0x20]; u32 before_count; u32 after_count; } out;
            KUMO_RETURN(serviceDispatchOut(std::addressof(g_control), 6, out,
                .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out, SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
                .buffers      = { { before, sizeof(before) }, { after, sizeof(after) } },
            ));
        }

        Result IsStateLocked(u32 state, bool *out) {
            u8 locked = 0xFF;
            KUMO_TRY(serviceDispatchInOut(std::addressof(g_control), 11, state, locked));
            *out = locked != 0;
            KUMO_SUCCEED();
        }

        bool ControlEventSignalled() {
            return R_SUCCEEDED(::eventWait(std::addressof(g_control_event), 0));
        }

        bool WaitControlEvent(u64 milliseconds) {
            return R_SUCCEEDED(::eventWait(std::addressof(g_control_event), milliseconds * 1000000ull));
        }

        /* ===== The checks ===== */

        void ResetAll() {
            for (auto *f : g_graph) {
                f->Reset();
                f->delay_ms = 20;
                f->hold = false;
            }
        }

        /* `after` was asked only after `before` acknowledged. */
        bool AskedAfterAck(FakeModule &after, FakeModule &before) {
            return FirstArrival(after) > LastAck(before) && LastAck(before) != 0;
        }

        bool EveryoneSawOnce(psc::PmState state) {
            std::scoped_lock lk(g_record_mutex);
            for (auto *f : g_graph) {
                if (f->arrivals != 1 || f->acks != 1 || f->first_state != state) {
                    return false;
                }
            }
            return true;
        }

        void TestControlBasics() {
            Result rc = ControlConnect();
            Check("psc:c connects", R_SUCCEEDED(rc), rc);
            rc = ControlInitialize();
            Check("psc:c Initialize returns an event", R_SUCCEEDED(rc), rc);
            Check("GetState is FullAwake at start", GetState() == psc::PmState_FullAwake, GetState());
            rc = GetResult();
            Check("GetResult succeeds before any request", R_SUCCEEDED(rc), rc);
            bool locked = true;
            rc = IsStateLocked(psc::PmState_ShutdownReady, std::addressof(locked));
            Check("IsStateLocked(ShutdownReady) is false", R_SUCCEEDED(rc) && !locked, rc);
            rc = Cancel();
            Check("Cancel with nothing to cancel succeeds", R_SUCCEEDED(rc), rc);
            Check("and signals nothing", !ControlEventSignalled(), 0);
        }

        void TestRegistrationErrors() {
            /* A module that depends on itself. */
            {
                const psc::PmModuleId self[] = { PscPmModuleId_Ns };
                psc::PmModule m;
                Result rc = m.Initialize(PscPmModuleId_Ns, self, 1);
                Check("a module that depends on itself is refused", rc == ResultInvalidArgument, rc);
            }

            /* A cycle: X depends on Y (not yet registered), then Y on X. */
            {
                const psc::PmModuleId x_deps[] = { PscPmModuleId_Ndd };
                const psc::PmModuleId y_deps[] = { PscPmModuleId_Olsc };
                psc::PmModule x, y;
                Result rc = x.Initialize(PscPmModuleId_Olsc, x_deps, 1);
                Check("a module depending on one not registered is accepted", R_SUCCEEDED(rc), rc);
                rc = y.Initialize(PscPmModuleId_Ndd, y_deps, 1);
                Check("a module closing a dependency cycle is refused", rc == ResultDependencyTooDeep, rc);
                rc = x.Finalize();
                Check("Finalize succeeds", R_SUCCEEDED(rc), rc);
            }

            /* A chain of MaximumDependencyLevels modules is accepted; one more is refused. */
            {
                constexpr u32 Base = 200;
                static psc::PmModule chain[psc::MaximumDependencyLevels + 1];
                u32 registered = 0;
                Result last = 0;
                for (u32 i = 0; i <= psc::MaximumDependencyLevels; ++i) {
                    const psc::PmModuleId dep = static_cast<psc::PmModuleId>(Base + i - 1);
                    last = chain[i].Initialize(static_cast<psc::PmModuleId>(Base + i), std::addressof(dep), i == 0 ? 0 : 1);
                    if (R_SUCCEEDED(last)) {
                        ++registered;
                    }
                }
                Check("a chain of 20 dependency levels is accepted", registered == psc::MaximumDependencyLevels, registered);
                Check("a 21st level is refused", last == ResultDependencyTooDeep, last);
                for (u32 i = 0; i <= psc::MaximumDependencyLevels; ++i) {
                    if (i < registered) {
                        KUMO_ABORT_RESULT(chain[i].Finalize());
                    }
                }
            }
        }

        void TestUnsupported() {
            ResetAll();
            bool all_refused = true;
            u32 last = 0;
            for (u32 state = psc::PmState_MinimumAwake; state <= psc::PmState_EssentialServicesAwake; ++state) {
                const Result rc = DispatchRequest(state, OrderToLower);
                last = rc;
                all_refused &= (last == ResultStateNotSupported);
            }
            Check("DispatchRequest refuses the sleep states 1 to 4 as not supported", all_refused, last);
            Result rc = DispatchRequest(6, OrderToLower);
            Check("DispatchRequest refuses state 6 as invalid", rc == ResultInvalidArgument, rc);
            rc = DispatchRequest(psc::PmState_ShutdownReady, 2);
            Check("DispatchRequest refuses transition order 2 as invalid", rc == ResultInvalidArgument, rc);
            svcSleepThread(INT64_C(1000000) * (100));
            int arrivals = 0;
            for (auto *f : g_graph) { arrivals += Arrivals(*f); }
            Check("no module was asked anything", arrivals == 0, arrivals);
            Check("the state is still FullAwake", GetState() == psc::PmState_FullAwake, GetState());
            Check("the control event was not signalled", !ControlEventSignalled(), 0);
        }

        void TestShutdownReady() {
            ResetAll();
            /* Lm is slow, so Fs, which it depends on, must be seen to wait for it. */
            g_lm.delay_ms = 500;

            const auto start = svcGetSystemTick();
            Result rc = DispatchRequest(psc::PmState_ShutdownReady, OrderToLower, 0);
            Check("DispatchRequest(ShutdownReady) succeeds", R_SUCCEEDED(rc), rc);
            rc = DispatchRequest(psc::PmState_ShutdownReady, OrderToLower, 0);
            Check("a second DispatchRequest while one is in progress is refused as busy", rc == ResultBusy, rc);
            rc = GetResult();
            Check("GetResult reports in progress", rc == ResultInProgress, rc);
            Check("the control event is not signalled yet", !ControlEventSignalled(), 0);

            /* While Lm holds things up, Fs must not have been asked. */
            WaitFor(g_sockets, 1, true);
            svcSleepThread(INT64_C(1000000) * (100));
            Check("while Lm has not acknowledged, Fs has not been asked", Arrivals(g_fs) == 0 && Acks(g_lm) == 0, Arrivals(g_fs));
            Check("and the control event is still not signalled", !ControlEventSignalled(), 0);

            const bool signalled = WaitControlEvent(5000);
            const s64 elapsed = ElapsedMilliSeconds(start);
            int acks = 0;
            for (auto *f : g_graph) { acks += Acks(*f); }
            Check("the control event is signalled", signalled, elapsed);
            Check("by then every module has acknowledged", acks == 5, acks);
            Check("and it took at least as long as Lm's delay", elapsed >= 500, elapsed);
            rc = GetResult();
            Check("GetResult succeeds", R_SUCCEEDED(rc), rc);
            Check("GetState is ShutdownReady", GetState() == psc::PmState_ShutdownReady, GetState());
            Check("every module was asked for ShutdownReady exactly once", EveryoneSawOnce(psc::PmState_ShutdownReady), 0);

            /* Down: the modules nobody depends on first, together; a module only after everything depending on it. */
            const int first_ack = std::min({ LastAck(g_nifm), LastAck(g_lm), LastAck(g_erpt) });
            Check("Nifm, Lm and Erpt were asked at once, before any acknowledgement",
                  FirstArrival(g_nifm) < first_ack && FirstArrival(g_lm) < first_ack && FirstArrival(g_erpt) < first_ack, first_ack);
            Check("WlanSockets was asked after Nifm acknowledged", AskedAfterAck(g_sockets, g_nifm), FirstArrival(g_sockets));
            Check("Fs was asked after WlanSockets acknowledged", AskedAfterAck(g_fs, g_sockets), FirstArrival(g_fs));
            Check("Fs was asked after Lm acknowledged", AskedAfterAck(g_fs, g_lm), FirstArrival(g_fs));

            rc = PrintModuleInformation();
            Check("PrintModuleInformation succeeds", R_SUCCEEDED(rc), rc);
        }

        void TestFullAwake() {
            ResetAll();
            g_lm.delay_ms = 0;
            g_fs.delay_ms = 200;

            Result rc = DispatchRequest(psc::PmState_FullAwake, OrderToHigher, 0);
            Check("DispatchRequest(FullAwake) succeeds", R_SUCCEEDED(rc), rc);
            const bool signalled = WaitControlEvent(5000);
            Check("the control event is signalled", signalled, 0);
            rc = GetResult();
            Check("GetResult succeeds", R_SUCCEEDED(rc), rc);
            Check("GetState is FullAwake", GetState() == psc::PmState_FullAwake, GetState());
            Check("every module was asked for FullAwake exactly once", EveryoneSawOnce(psc::PmState_FullAwake), 0);

            /* Up: a module only after everything it depends on. */
            const int first_ack = std::min(LastAck(g_fs), LastAck(g_erpt));
            Check("Fs and Erpt were asked first, together", FirstArrival(g_fs) < first_ack && FirstArrival(g_erpt) < first_ack, first_ack);
            Check("WlanSockets was asked after Fs acknowledged", AskedAfterAck(g_sockets, g_fs), FirstArrival(g_sockets));
            Check("Lm was asked after Fs acknowledged", AskedAfterAck(g_lm, g_fs), FirstArrival(g_lm));
            Check("Nifm was asked after WlanSockets acknowledged", AskedAfterAck(g_nifm, g_sockets), FirstArrival(g_nifm));
        }

        void TestCancel() {
            ResetAll();
            /* Nifm holds on to ShutdownReady until released, so the transition stops at the first level. */
            g_nifm.hold = true;

            Result rc = DispatchRequest(psc::PmState_ShutdownReady, OrderToLower, 0);
            Check("DispatchRequest(ShutdownReady) succeeds", R_SUCCEEDED(rc), rc);
            const bool first_level = WaitFor(g_nifm, 1, false) && WaitFor(g_lm, 1, true) && WaitFor(g_erpt, 1, true);
            Check("Nifm is asked; Lm and Erpt acknowledge", first_level, 0);
            svcSleepThread(INT64_C(1000000) * (50));

            rc = Cancel();
            Check("Cancel succeeds", R_SUCCEEDED(rc), rc);
            g_nifm.hold = false;
            Release(g_nifm);

            const bool signalled = WaitControlEvent(5000);
            Check("the control event is signalled once everyone is back", signalled, 0);
            rc = GetResult();
            Check("GetResult reports the request as cancelled", rc == ResultCancelled, rc);
            Check("GetState is FullAwake", GetState() == psc::PmState_FullAwake, GetState());

            std::scoped_lock lk(g_record_mutex);
            Check("WlanSockets and Fs, never asked, are not asked anything", g_sockets.arrivals == 0 && g_fs.arrivals == 0, g_sockets.arrivals + g_fs.arrivals);
            Check("Lm and Erpt, which reached ShutdownReady, are asked for FullAwake",
                  g_lm.arrivals == 2 && g_lm.last_state == psc::PmState_FullAwake && g_erpt.arrivals == 2 && g_erpt.last_state == psc::PmState_FullAwake, g_lm.arrivals);
            Check("Nifm, asked but not done, is asked for FullAwake", g_nifm.arrivals == 2 && g_nifm.last_state == psc::PmState_FullAwake, g_nifm.arrivals);
        }

        void TestDisconnect() {
            ResetAll();
            Result rc = Register(g_bcat);
            Check("a module that will close its object registers (Bcat, depends on Fs)", R_SUCCEEDED(rc), rc);
            RawModule raw = {};
            const u32 raw_deps[] = { PscPmModuleId_Fs };
            rc = OpenRawModule(std::addressof(raw), PscPmModuleId_Npns, raw_deps, std::size(raw_deps));
            Check("a module on its own non-domain session registers (Npns, depends on Fs)", R_SUCCEEDED(rc), rc);

            const auto start = svcGetSystemTick();
            rc = DispatchRequest(psc::PmState_ShutdownReady, OrderToLower, 0);
            Check("DispatchRequest(ShutdownReady) succeeds", R_SUCCEEDED(rc), rc);

            /* The raw module is asked, and closes its sessions instead of acknowledging. */
            const bool raw_asked = R_SUCCEEDED(::eventWait(std::addressof(raw.event), 3'000'000'000ull));
            const int raw_seq = NextSequence();
            Check("the raw module is asked", raw_asked, 0);
            CloseRawModule(std::addressof(raw));

            const bool signalled = WaitControlEvent(AcknowledgeTimeoutMs / 2);
            const s64 elapsed = ElapsedMilliSeconds(start);
            Check("the transition completes, well before the acknowledgement timeout", signalled, elapsed);
            rc = GetResult();
            Check("GetResult succeeds (not timed out)", R_SUCCEEDED(rc), rc);
            {
                std::scoped_lock lk(g_record_mutex);
                Check("Bcat was asked and closed its object", g_bcat.arrivals == 1 && g_bcat.closed, g_bcat.arrivals);
                Check("Fs was asked after Bcat and the raw module went away", g_fs.first_arrival > g_bcat.first_arrival && g_fs.first_arrival > raw_seq, g_fs.first_arrival);
            }

            rc = DispatchRequest(psc::PmState_FullAwake, OrderToHigher, 0);
            Check("and back to FullAwake", R_SUCCEEDED(rc) && WaitControlEvent(5000) && R_SUCCEEDED(GetResult()), rc);
        }

        void TestTimeout() {
            ResetAll();
            Result rc = Register(g_friends);
            Check("a module that ignores ShutdownReady registers (Friends)", R_SUCCEEDED(rc), rc);

            const auto start = svcGetSystemTick();
            rc = DispatchRequest(psc::PmState_ShutdownReady, OrderToLower, 0);
            Check("DispatchRequest(ShutdownReady) succeeds", R_SUCCEEDED(rc), rc);
            svcSleepThread(INT64_C(1000000) * (1000));
            {
                int acks = 0;
                for (auto *f : g_graph) { acks += Acks(*f); }
                Check("the others acknowledge, but the transition waits for Friends", acks == 5 && !ControlEventSignalled() && GetResult() == ResultInProgress, acks);
            }

            const bool signalled = WaitControlEvent(AcknowledgeTimeoutMs + 5000);
            const s64 elapsed = ElapsedMilliSeconds(start);
            Check("the transition completes after the acknowledgement timeout", signalled && elapsed >= AcknowledgeTimeoutMs - 100 && elapsed < AcknowledgeTimeoutMs + 3000, elapsed);
            rc = GetResult();
            Check("GetResult reports the timeout", rc == ResultTimedOut, rc);
            Check("GetState is ShutdownReady", GetState() == psc::PmState_ShutdownReady, GetState());

            rc = DispatchRequest(psc::PmState_FullAwake, OrderToHigher, 0);
            Check("back to FullAwake, with Friends acknowledging that", R_SUCCEEDED(rc) && WaitControlEvent(5000) && R_SUCCEEDED(GetResult()) && Acks(g_friends) == 1, Acks(g_friends));
        }

        void TestUndocumented() {
            Result rc = GetModuleInformation();
            Check("GetModuleInformation answers not supported", rc == ResultNotSupported, rc);
            rc = serviceDispatch(std::addressof(g_control), 7);
            Check("SetRandomDelay answers not supported", rc == ResultNotSupported, rc);
            Check("and the session still works", GetState() == psc::PmState_FullAwake, GetState());
        }

        void TestFinalize() {
            /* Finalize every module; a transition with none must complete at once. */
            bool ok = true;
            for (auto *f : g_graph) {
                ok &= R_SUCCEEDED(f->Module().Finalize());
            }
            ok &= R_SUCCEEDED(g_friends.Module().Finalize());
            Check("every module finalizes", ok, 0);
            const Result rc = DispatchRequest(psc::PmState_ShutdownReady, OrderToLower, 0);
            Check("with no modules, ShutdownReady completes at once", R_SUCCEEDED(rc) && WaitControlEvent(1000) && R_SUCCEEDED(GetResult()) && GetState() == psc::PmState_ShutdownReady, rc);
            Check("and FullAwake", R_SUCCEEDED(DispatchRequest(psc::PmState_FullAwake, OrderToHigher, 0)) && WaitControlEvent(1000) && GetState() == psc::PmState_FullAwake, 0);
        }

    }

    void RunTest() {
        Log("[psc_test] starting\n");

        /* Waits in sm until psc has registered psc:m. */
        Result rc = ::pscmInitialize();
        Check("pscmInitialize", R_SUCCEEDED(rc), rc);

        if (R_SUCCEEDED(rc)) {
            TestControlBasics();
            TestRegistrationErrors();

            bool registered = true;
            for (auto *f : g_graph) {
                rc = Register(*f);
                registered &= R_SUCCEEDED(rc);
            }
            Check("five modules register through libnx PscPmModule", registered, rc);

            if (registered) {
                TestUnsupported();
                TestShutdownReady();
                TestFullAwake();
                TestCancel();
                TestDisconnect();
                TestTimeout();
                TestUndocumented();
                TestFinalize();
            }
        }

        Log("[psc_test] done: %d checks, %d failure(s)\n", g_checks, g_failures);

        /* Idle forever; this KIP is immortal. */
        while (true) {
            svcSleepThread(INT64_C(60000000000));
        }
    }

}

extern "C" {
    u32 __nx_applet_type = AppletType_None;
    void __libnx_initheap() {
        alignas(16) static char heap[1024 * 1024];
        extern char *fake_heap_start, *fake_heap_end;
        fake_heap_start = heap; fake_heap_end = heap + sizeof(heap);
    }
    void __appInit() {
        SecmonArgs config{{0xC3000002, 65011}};
        svcCallSecureMonitor(&config);
        KUMO_ASSERT(config.X[0] == 0 && config.X[1] != 0);
        hosversionSet(BIT(31) | static_cast<u32>(config.X[1] >> 8));
        KUMO_ABORT_RESULT(smInitialize());
    }
    void __appExit() { smExit(); }
    void __libnx_exit(int) { for (;;) svcSleepThread(1000000000); }
}
int main(int, char **) { RunTest(); return 0; }
