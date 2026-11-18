#include <switch.h>
#include <kumo/prelude.hpp>
#include "glue_inner.hpp"
#include "glue_log.hpp"


namespace kumo::horizon::glue {

    namespace {

        /* The settings the Switch reads from its settings service; there is none here, so glue gives the core these. */
        constexpr bool DefaultAutomaticCorrection        = true;
        constexpr u64  DefaultSufficientAccuracyMinutes  = 43200;

        constinit kumo::Mutex g_manager_mutex;
        constinit bool g_has_manager = false;
        constinit ::Service g_manager = {};

        constinit InnerStatic g_user_static(ManagerCommand_AsUser);
        constinit InnerStatic g_admin_static(ManagerCommand_AsAdmin);
        constinit InnerStatic g_repair_static(ManagerCommand_AsRepair);

        /* time:m 12 and 13 as libnx must send them: the framework lays raw data out sorted by alignment. */
        struct SetupNetworkInput {
            u8 context[0x20];
            u64 sufficient_accuracy_minutes;
        };

        struct SetupUserInput {
            u8 automatic_correction;
            u8 pad[7];
            u8 updated_time[0x18];
        };

        Result ConnectManager() {
            std::scoped_lock lk(g_manager_mutex);
            if (g_has_manager) {
                KUMO_SUCCEED();
            }

            KUMO_TRY(smGetService(&g_manager, "time:m"));
            g_has_manager = true;

            /* What psc cannot read for itself. A context of an invalid steady clock, which the core ignores, leaves the network clock unset. */
            const SetupNetworkInput network = { {}, DefaultSufficientAccuracyMinutes };
            Result rc = serviceDispatchIn(std::addressof(g_manager), 12, network);
            if (kumo::IsFailure(rc)) {
                Log("[glue] time:m SetupStandardNetworkSystemClockCore failed: 0x%x\n", rc);
            }

            SetupUserInput user = {};
            user.automatic_correction = DefaultAutomaticCorrection ? 1 : 0;
            rc = serviceDispatchIn(std::addressof(g_manager), 13, user);
            if (kumo::IsFailure(rc)) {
                Log("[glue] time:m SetupStandardUserSystemClockCore failed: 0x%x\n", rc);
            }

            KUMO_SUCCEED();
        }

        constexpr size_t ThreadStackSize = 16 * 1024;
        alignas(0x1000) constinit u8 g_thread_stack[ThreadStackSize];
        constinit Thread g_thread;

        void ConnectThreadFunction(void *) {
            /* psc may not have registered time:m yet. */
            while (kumo::IsFailure(ConnectManager())) {
                svcSleepThread(200000000ULL);
            }
            Log("[glue] connected to time:m\n");
        }

    }

    Result InnerStatic::GetStatic(::Service **out) {
        std::scoped_lock lk(m_mutex);
        if (!m_has_static) {
            KUMO_TRY(ConnectManager());

            ::Service created = {};
            KUMO_TRY(serviceDispatch(std::addressof(g_manager), m_manager_command,
                .out_num_objects = 1,
                .out_objects     = std::addressof(created),
            ));
            m_static     = created;
            m_has_static = true;
        }
        *out = std::addressof(m_static);
        KUMO_SUCCEED();
    }

    Result InnerStatic::GetObject(::Service **out, InnerKind kind) {
        ::Service *static_service;
        KUMO_TRY(this->GetStatic(std::addressof(static_service)));

        std::scoped_lock lk(m_mutex);
        if (!m_has_object[kind]) {
            ::Service created = {};
            KUMO_TRY(serviceDispatch(static_service, static_cast<u32>(kind),
                .out_num_objects = 1,
                .out_objects     = std::addressof(created),
            ));
            m_objects[kind]    = created;
            m_has_object[kind] = true;
        }
        *out = std::addressof(m_objects[kind]);
        KUMO_SUCCEED();
    }

    InnerStatic &GetUserStatic()   { return g_user_static; }
    InnerStatic &GetAdminStatic()  { return g_admin_static; }
    InnerStatic &GetRepairStatic() { return g_repair_static; }

    void StartConnectThread() {
        s32 priority = 0;
        KUMO_ABORT_RESULT(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE));
        KUMO_ABORT_RESULT(threadCreate(&g_thread, ConnectThreadFunction, nullptr, g_thread_stack, sizeof(g_thread_stack), priority, -2));
        KUMO_ABORT_RESULT(threadStart(&g_thread));
    }

}
