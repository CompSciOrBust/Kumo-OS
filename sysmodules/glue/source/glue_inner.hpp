/*
 * glue's connection to the time core. On retail firmware glue is a client of
 * psc's time:m, which hands it one static service object per name glue serves
 * (GetStaticServiceAsUser for time:u, AsAdmin for time:a, AsRepair for time:r),
 * and glue's own objects wrap those. Here the inner objects are libnx
 * Services; one inner object of each kind is created on first use and shared
 * by every client of that name.
 */
#pragma once
#include <kumo/prelude.hpp>
#include <kumo/mutex.hpp>

namespace kumo::horizon::glue {

    /* The object kinds an IStaticService hands out; the value is the command that creates it. */
    enum InnerKind : u32 {
        InnerKind_UserClock    = 0,
        InnerKind_NetworkClock = 1,
        InnerKind_SteadyClock  = 2,
        InnerKind_TimeZone     = 3,
        InnerKind_LocalClock   = 4,
        InnerKind_Ephemeral    = 5,
        InnerKind_Count        = 6,
    };

    /* time:m's commands that create a static service. */
    enum ManagerCommand : u32 {
        ManagerCommand_AsUser   = 0,
        ManagerCommand_AsAdmin  = 5,
        ManagerCommand_AsRepair = 6,
    };

    class InnerStatic {
        private:
            const u32 m_manager_command;
            kumo::Mutex m_mutex;
            bool m_has_static;
            bool m_has_object[InnerKind_Count];
            ::Service m_static;
            ::Service m_objects[InnerKind_Count];
        public:
            constexpr explicit InnerStatic(u32 manager_command) : m_manager_command(manager_command), m_mutex(), m_has_static(false), m_has_object(), m_static(), m_objects() { /* ... */ }

            /* The inner IStaticService, connecting to time:m if that has not happened yet. */
            Result GetStatic(::Service **out);

            /* An object of the inner IStaticService, created on first use. */
            Result GetObject(::Service **out, InnerKind kind);
    };

    InnerStatic &GetUserStatic();
    InnerStatic &GetAdminStatic();
    InnerStatic &GetRepairStatic();

    /* Connects to time:m (waiting for psc to register it) and hands the core the settings glue stands in for. Runs on a thread of its own. */
    void StartConnectThread();

}
