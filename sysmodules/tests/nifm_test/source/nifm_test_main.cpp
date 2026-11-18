/*
 * Test client for the nifm system module. Runs as a KIP, uses libnx's nifm
 * API (nifmInitialize and the calls it provides) the way an application
 * would, and reports each check on the kernel debug log.
 *
 * It runs one of two groups, chosen by asking the sockets module whether it
 * has a network device:
 *   - with a device (QEMU's default virtio-net, on user-mode networking where
 *     the guest is 10.0.2.15, its gateway 10.0.2.2 and its DNS server
 *     10.0.2.3): the service, the address, the connection status and the
 *     profile, requests and their events, a TCP and a UDP socket agreeing
 *     with what nifm says, losing and regaining the network, and error cases;
 *   - without one (QEMU started with no NIC): that nothing claims a
 *     connection and requests fail as "network not available".
 * Nothing outside the guest is needed. The group with a device takes the
 * interface down and up again through bsdcfg, which this KIP calls directly
 * as a second client, to see requests go OnHold and be Accepted again.
 */
#include <switch.h>
#include <kumo/assert.hpp>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

/* libnx leaves the service headers out of switch.h, and they have no C linkage guard of their own. */
extern "C" {
    #include <switch/services/bsd.h>
    #include <switch/services/nifm.h>
}

namespace {

    namespace {

        /* QEMU's user-mode network. */
        constexpr u8 GuestAddress[4]  = { 10, 0, 2, 15 };
        constexpr u8 Netmask[4]       = { 255, 255, 255, 0 };
        constexpr u8 Gateway[4]       = { 10, 0, 2, 2 };
        constexpr u8 PrimaryDns[4]    = { 10, 0, 2, 3 };
        constexpr u8 NoAddress[4]     = { 0, 0, 0, 0 };

        /* A port nobody listens on, on the host: connecting to it is answered with a reset. */
        constexpr u16 HostPort_Closed = 47009;

        /* nifm's results (module 110): the description is the value shifted down by 9 bits. */
        constexpr u32 ResultNetworkNotAvailable = 0x8986E;   /* 2110-1100 */
        constexpr u32 ResultConnectionReset     = 0xFA66E;   /* 2110-2003 */
        constexpr u32 ResultWrongInput          = 0x1906E;   /* 2110-0200 */
        constexpr u32 ResultInvalidPermission   = 0x0F06E;   /* 2110-0120 */
        constexpr u32 ResultNotSubmitted        = 0x26E6E;   /* 2110-0311 */

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
            Log("[nifm_test] %s: %s (0x%lx)\n", ok ? "PASS" : "FAIL", what, value);
        }

        /* An address as libnx hands it over: the four bytes in the order they are written, read as a u32. */
        u32 Pack(const u8 (&bytes)[4]) {
            u32 value;
            std::memcpy(std::addressof(value), bytes, sizeof(value));
            return value;
        }

        s64 ElapsedMilliSeconds(u64 since) {
            return static_cast<s64>(armTicksToNs(svcGetSystemTick() - since) / 1000000);
        }

        /* Whether an event is signalled now. libnx's events here auto-clear, so this consumes the signal. */
        bool IsSignalled(::Event *event) {
            return R_SUCCEEDED(::eventWait(event, 0));
        }

        bool WaitSignalled(::Event *event, u64 milliseconds) {
            return R_SUCCEEDED(::eventWait(event, milliseconds * 1000000ull));
        }

        /* ===== Memory libnx asks for ===== */
        /* socketInitialize allocates its transfer memory, and newlib a small record per descriptor. */

        alignas(0x1000) constinit u8 g_malloc_memory[8 * 1024 * 1024];

        /* ===== bsdcfg, called directly: this KIP plays the second client that takes the interface down ===== */

        constexpr Result ResultNotSupported = MAKERESULT(Module_Kernel, 127);

        constexpr u32 BsdCfgSetIfUp    = 0;
        constexpr u32 BsdCfgSetIfDown  = 3;
        constexpr u32 BsdCfgGetIfState = 4;
        constexpr u32 BsdCfgCommand100 = 100;
        constexpr u32 IfUpFlagDhcp = 1;

        struct IfUpInput { u32 index; u32 flags; u32 reserved[8]; };
        struct IfStaticConfig { u32 address; u32 netmask; u32 gateway; u32 dns[2]; };
        struct IfState { u32 flags; u32 mtu; u8 mac[6]; u8 padding[2]; u32 address; u32 netmask; u32 gateway; u32 dns[2]; u32 reserved; };
        constexpr u32 IfStateHasAddress = 1u << 2;

        constinit ::Service g_bsdcfg = {};

        Result ConnectToBsdConfig() {
            Handle handle;
            Result rc = smGetServiceOriginal(std::addressof(handle), smEncodeName("bsdcfg"));
            if (R_FAILED(rc)) return rc;
            ::serviceCreate(std::addressof(g_bsdcfg), handle);
            return 0;
        }

        Result BsdConfigGetState(IfState *out) {
            const u32 index = 0;
            return (serviceDispatchInOut(std::addressof(g_bsdcfg), BsdCfgGetIfState, index, *out));
        }

        Result BsdConfigSetDown() {
            const u32 index = 0;
            return (serviceDispatchIn(std::addressof(g_bsdcfg), BsdCfgSetIfDown, index));
        }

        Result BsdConfigSetUpDhcp() {
            const IfUpInput in = { .index = 0, .flags = IfUpFlagDhcp, .reserved = {} };
            const IfStaticConfig config = {};
            return (serviceDispatchIn(std::addressof(g_bsdcfg), BsdCfgSetIfUp, in,
                .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
                .buffers      = { { std::addressof(config), sizeof(config) } },
            ));
        }

        /* ===== Helpers over libnx's nifm ===== */

        /* The address nifm reports, or 0 if it reports none. */
        Result GetNifmAddress(u32 *out) {
            *out = 0;
            return (::nifmGetCurrentIpAddress(out));
        }

        /* Wait for nifm to report an address. */
        bool WaitForAddress(u32 *out, u64 timeout_ms) {
            const u64 start = svcGetSystemTick();
            while (R_FAILED(GetNifmAddress(out)) && ElapsedMilliSeconds(start) < static_cast<s64>(timeout_ms)) {
                svcSleepThread(INT64_C(1000000) * (100));
            }
            return *out != 0;
        }

        /*
         * The state and result as the service has them now. libnx's own nifmGetRequestState and nifmGetResult
         * return a copy and only ask the service when the request's state event is signalled, and waiting on
         * that event clears it, so a check that has waited on the event would read a stale copy.
         */
        ::NifmRequestState GetState(::NifmRequest *request) {
            u32 state = NifmRequestState_Invalid;
            serviceAssumeDomain(std::addressof(request->s));
            serviceDispatchOut(std::addressof(request->s), 0, state);
            return static_cast<::NifmRequestState>(state);
        }

        u32 GetResultValue(::NifmRequest *request) {
            serviceAssumeDomain(std::addressof(request->s));
            return serviceDispatch(std::addressof(request->s), 1);
        }

        constexpr ::NifmRequestState Free     = NifmRequestState_Unknown1;
        constexpr ::NifmRequestState OnHold   = NifmRequestState_OnHold;
        constexpr ::NifmRequestState Accepted = NifmRequestState_Available;

        /* ===== Service ===== */

        bool TestService() {
            Result rc = ::nifmInitialize(NifmServiceType_User);
            Check("nifmInitialize(nifm:u) connects, and IGeneralService is created over a domain", R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) {
                return false;
            }

            const NifmClientId id = ::nifmGetClientId();
            Check("GetClientId returns an ID (libnx reports 0 on failure)", id.id != 0, id.id);

            /* Each port is a separate service; libnx keeps one connection, so leave and come back. */
            ::nifmExit();
            rc = ::nifmInitialize(NifmServiceType_System);
            Check("nifm:s accepts a connection", R_SUCCEEDED(rc), rc);
            ::nifmExit();
            rc = ::nifmInitialize(NifmServiceType_Admin);
            Check("nifm:a accepts a connection", R_SUCCEEDED(rc), rc);
            ::nifmExit();

            rc = ::nifmInitialize(NifmServiceType_User);
            Check("nifm:u connects again after the others", R_SUCCEEDED(rc), rc);
            return R_SUCCEEDED(rc);
        }

        /* libnx normally converts nifm to a domain; ordinary sessions must
         * also return usable child sessions with independent lifetimes. */
        void TestOrdinarySessions() {
            Handle handle;
            Result rc = smGetServiceOriginal(std::addressof(handle), smEncodeName("nifm:u"));
            Check("ordinary nifm:u session opens", R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) return;

            ::Service root = {};
            ::Service general = {};
            ::serviceCreate(std::addressof(root), handle);
            rc = serviceDispatch(std::addressof(root), 4,
                .out_num_objects = 1, .out_objects = std::addressof(general));
            Check("ordinary static service returns a general session", R_SUCCEEDED(rc), rc);
            ::serviceClose(std::addressof(root));
            if (R_FAILED(rc)) return;

            const s32 preset = 1;
            ::Service first = {}, second = {};
            rc = serviceDispatchIn(std::addressof(general), 4, preset,
                .out_num_objects = 1, .out_objects = std::addressof(first));
            Check("general session survives root close and creates a request", R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) {
                ::serviceClose(std::addressof(general));
                return;
            }
            rc = serviceDispatchIn(std::addressof(general), 4, preset,
                .out_num_objects = 1, .out_objects = std::addressof(second));
            Check("ordinary general session creates a second request", R_SUCCEEDED(rc), rc);
            ::serviceClose(std::addressof(general));
            ::serviceClose(std::addressof(first));
            if (R_SUCCEEDED(rc)) {
                u32 state = NifmRequestState_Invalid;
                rc = serviceDispatchOut(std::addressof(second), 0, state);
                Check("request survives parent and sibling session close", R_SUCCEEDED(rc) && state == Free, rc);
                const Result unknown = serviceDispatch(std::addressof(second), 7);
                rc = serviceDispatchOut(std::addressof(second), 0, state);
                Check("unknown request command closes its session", R_FAILED(unknown) && R_FAILED(rc), unknown);
                ::serviceClose(std::addressof(second));
            }
        }

        /* ===== With a network device ===== */

        void TestAddress() {
            u32 address = 0;
            const u64 start = svcGetSystemTick();
            const bool got = WaitForAddress(std::addressof(address), 20000);
            Check("GetCurrentIpAddress succeeds once the lease is held", got, ElapsedMilliSeconds(start));
            Check("the current address is 10.0.2.15", address == Pack(GuestAddress), address);

            /* What the socket layer would send from. */
            const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in to = {};
            to.sin_len         = sizeof(to);
            to.sin_family      = AF_INET;
            to.sin_port        = htons(HostPort_Closed);
            to.sin_addr.s_addr = Pack(Gateway);
            struct sockaddr_in name = {};
            socklen_t len = sizeof(name);
            const int connected = ::connect(fd, reinterpret_cast<struct sockaddr *>(std::addressof(to)), sizeof(to));
            const int named = ::getsockname(fd, reinterpret_cast<struct sockaddr *>(std::addressof(name)), std::addressof(len));
            Check("a UDP socket's own address (getsockname) is the address nifm reports", fd >= 0 && connected == 0 && named == 0 && name.sin_addr.s_addr == address, name.sin_addr.s_addr);
            ::close(fd);

            u32 current, mask, gateway, dns1, dns2;
            const Result rc = ::nifmGetCurrentIpConfigInfo(std::addressof(current), std::addressof(mask), std::addressof(gateway), std::addressof(dns1), std::addressof(dns2));
            Check("GetCurrentIpConfigInfo succeeds", R_SUCCEEDED(rc), rc);
            Check("IP config: address 10.0.2.15", current == Pack(GuestAddress), current);
            Check("IP config: netmask 255.255.255.0", mask == Pack(Netmask), mask);
            Check("IP config: gateway 10.0.2.2", gateway == Pack(Gateway), gateway);
            Check("IP config: primary DNS 10.0.2.3", dns1 == Pack(PrimaryDns), dns1);
            Check("IP config: no secondary DNS (QEMU offers one server)", dns2 == Pack(NoAddress), dns2);
        }

        void TestStatus() {
            ::NifmInternetConnectionType type = static_cast<::NifmInternetConnectionType>(0);
            u32 strength = 99;
            ::NifmInternetConnectionStatus status = NifmInternetConnectionStatus_ConnectingUnknown1;
            const Result rc = ::nifmGetInternetConnectionStatus(std::addressof(type), std::addressof(strength), std::addressof(status));
            Check("GetInternetConnectionStatus succeeds", R_SUCCEEDED(rc), rc);
            Check("connection type is Ethernet", type == NifmInternetConnectionType_Ethernet, type);
            Check("connection status is Connected", status == NifmInternetConnectionStatus_Connected, status);
            Check("Wi-Fi strength is 0", strength == 0, strength);

            bool ethernet = false, wireless = true;
            Result rc1 = ::nifmIsEthernetCommunicationEnabled(std::addressof(ethernet));
            Result rc2 = ::nifmIsWirelessCommunicationEnabled(std::addressof(wireless));
            Check("Ethernet communication is enabled", R_SUCCEEDED(rc1) && ethernet, rc1);
            Check("wireless communication is not", R_SUCCEEDED(rc2) && !wireless, rc2);
        }

        void TestProfile() {
            NifmNetworkProfileData profile = {};
            Result rc = ::nifmGetCurrentNetworkProfile(std::addressof(profile));
            Check("GetCurrentNetworkProfile succeeds", R_SUCCEEDED(rc), rc);
            Check("profile: name is set", profile.network_name[0] != '\0', profile.network_name[0]);
            Check("profile: interface type is Ethernet", profile.unk_x54 == NifmInternetConnectionType_Ethernet, profile.unk_x54);
            Check("profile: IP setting carries the current address, netmask and gateway",
                  std::memcmp(profile.ip_setting_data.ip_address_setting.current_addr.addr, GuestAddress, 4) == 0 &&
                  std::memcmp(profile.ip_setting_data.ip_address_setting.subnet_mask.addr, Netmask, 4) == 0 &&
                  std::memcmp(profile.ip_setting_data.ip_address_setting.gateway.addr, Gateway, 4) == 0, 0);
            Check("profile: DNS is automatic and names 10.0.2.3, and the MTU is 1500",
                  profile.ip_setting_data.dns_setting.is_automatic == 1 &&
                  std::memcmp(profile.ip_setting_data.dns_setting.primary_dns_server.addr, PrimaryDns, 4) == 0 &&
                  profile.ip_setting_data.mtu == 1500, profile.ip_setting_data.mtu);

            NifmNetworkProfileBasicInfo infos[4] = {};
            s32 total = -1;
            rc = ::nifmEnumerateNetworkProfiles(NifmNetworkProfileType_User, infos, 4, std::addressof(total));
            Check("EnumerateNetworkProfiles lists the one user profile", R_SUCCEEDED(rc) && total == 1 &&
                  std::memcmp(infos[0].uuid.uuid, profile.uuid.uuid, sizeof(profile.uuid.uuid)) == 0 &&
                  infos[0].connection_type == NifmInternetConnectionType_Ethernet, total);

            NifmNetworkProfileData again = {};
            rc = ::nifmGetNetworkProfile(infos[0].uuid, std::addressof(again));
            Check("GetNetworkProfile by that ID returns the same profile", R_SUCCEEDED(rc) && std::strcmp(again.network_name, profile.network_name) == 0, rc);

            Uuid unknown = {};
            unknown.uuid[0] = 0x55;
            rc = ::nifmGetNetworkProfile(unknown, std::addressof(again));
            Check("GetNetworkProfile with an unknown ID fails (2110-0200)", rc == ResultWrongInput, rc);

            total = -1;
            rc = ::nifmEnumerateNetworkProfiles(NifmNetworkProfileType_Temporary, infos, 4, std::addressof(total));
            Check("EnumerateNetworkProfiles for another kind finds none", R_SUCCEEDED(rc) && total == 0, total);
        }

        void TestRequest() {
            ::NifmRequest request;
            Result rc = ::nifmCreateRequest(std::addressof(request), true);
            Check("CreateRequest and GetSystemEventReadableHandles succeed", R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) {
                return;
            }

            Check("a new request is Free", GetState(std::addressof(request)) == Free, GetState(std::addressof(request)));
            Check("its result is 'not submitted' (2110-0311)", GetResultValue(std::addressof(request)) == ResultNotSubmitted, GetResultValue(std::addressof(request)));
            bool accepted = true;
            ::nifmIsAnyForegroundRequestAccepted(std::addressof(accepted));
            Check("no request is accepted yet", !accepted && !::nifmIsAnyInternetRequestAccepted(::nifmGetClientId()), accepted);

            /* The setters are accepted. Their effect is none. */
            rc = ::nifmRequestSetKeptInSleep(std::addressof(request), false);
            Result rc2 = ::nifmRequestSetNetworkProfileId(std::addressof(request), Uuid{});
            Check("SetKeptInSleep and SetNetworkProfileId are accepted", R_SUCCEEDED(rc) && R_SUCCEEDED(rc2), rc);

            /* A socket cannot be registered before the request has the network. */
            const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            rc = ::nifmRequestRegisterSocketDescriptor(std::addressof(request), fd);
            Check("RegisterSocketDescriptor on a Free request fails", R_FAILED(rc), rc);

            rc = ::nifmRequestSubmit(std::addressof(request));
            Check("Submit succeeds", R_SUCCEEDED(rc), rc);
            Check("with the interface up the request is Accepted at once", GetState(std::addressof(request)) == Accepted, GetState(std::addressof(request)));
            Check("its result is success", GetResultValue(std::addressof(request)) == 0, 0);
            Check("the state event was signalled by Submit", WaitSignalled(std::addressof(request.event_request_state), 1000), 0);
            Check("and is clear after being waited on", !IsSignalled(std::addressof(request.event_request_state)), 0);
            ::NifmRequestState cached = NifmRequestState_Invalid;
            ::nifmGetRequestState(std::addressof(request), std::addressof(cached));
            Check("libnx's own GetRequestState agrees (from the copy it took at Submit)", cached == Accepted, cached);
            Check("the second event is not signalled while the network is stable", !IsSignalled(std::addressof(request.event1)), 0);
            Check("IsAnyInternetRequestAccepted is true", ::nifmIsAnyInternetRequestAccepted(::nifmGetClientId()), 0);
            ::nifmIsAnyForegroundRequestAccepted(std::addressof(accepted));
            Check("IsAnyForegroundRequestAccepted is true", accepted, accepted);

            rc = ::nifmRequestSubmit(std::addressof(request));
            Check("submitting an Accepted request again changes nothing", R_SUCCEEDED(rc) && GetState(std::addressof(request)) == Accepted, rc);

            rc = ::nifmRequestRegisterSocketDescriptor(std::addressof(request), fd);
            Check("RegisterSocketDescriptor on an Accepted request succeeds", R_SUCCEEDED(rc), rc);
            rc = ::nifmRequestRegisterSocketDescriptor(std::addressof(request), fd);
            Check("only one socket can be registered", R_FAILED(rc), rc);
            rc = ::nifmRequestUnregisterSocketDescriptor(std::addressof(request), fd + 1);
            Check("UnregisterSocketDescriptor with another descriptor fails", R_FAILED(rc), rc);
            rc = ::nifmRequestUnregisterSocketDescriptor(std::addressof(request), fd);
            Check("UnregisterSocketDescriptor with the registered one succeeds", R_SUCCEEDED(rc), rc);
            ::close(fd);

            rc = ::nifmRequestCancel(std::addressof(request));
            Check("Cancel succeeds", R_SUCCEEDED(rc), rc);
            Check("a cancelled request is Free again", GetState(std::addressof(request)) == Free, GetState(std::addressof(request)));
            Check("Cancel signalled the state event", WaitSignalled(std::addressof(request.event_request_state), 1000), 0);
            Check("with its result back to 'not submitted'", GetResultValue(std::addressof(request)) == ResultNotSubmitted, 0);
            Check("and no request is accepted any more", !::nifmIsAnyInternetRequestAccepted(::nifmGetClientId()), 0);

            rc = ::nifmRequestSubmitAndWait(std::addressof(request));
            Check("SubmitAndWait after a cancel succeeds", R_SUCCEEDED(rc) && GetState(std::addressof(request)) == Accepted, rc);

            /* A second request, from the same client, is independent. */
            ::NifmRequest second;
            rc = ::nifmCreateRequest(std::addressof(second), true);
            Check("a second request can be created while the first is Accepted", R_SUCCEEDED(rc) && GetState(std::addressof(second)) == Free, rc);
            if (R_SUCCEEDED(rc)) {
                ::nifmRequestClose(std::addressof(second));
            }
            Check("closing it leaves the first Accepted", GetState(std::addressof(request)) == Accepted, GetState(std::addressof(request)));

            ::nifmRequestClose(std::addressof(request));
            Check("closing the last accepted request clears IsAnyInternetRequestAccepted", !::nifmIsAnyInternetRequestAccepted(::nifmGetClientId()), 0);
        }

        /* A TCP connection through the gateway, with the address nifm gave. */
        void TestSockets(const char *when) {
            char name[0x80];

            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in to = {};
            to.sin_len         = sizeof(to);
            to.sin_family      = AF_INET;
            to.sin_port        = htons(HostPort_Closed);
            to.sin_addr.s_addr = Pack(Gateway);
            const int ret = ::connect(fd, reinterpret_cast<struct sockaddr *>(std::addressof(to)), sizeof(to));
            const int error = errno;
            ::close(fd);
            std::snprintf(name, sizeof(name), "%s: TCP to the host through the gateway is answered (connection refused by a closed port)", when);
            Check(name, ret == -1 && error == ECONNREFUSED, error);

            u32 address = 0;
            std::snprintf(name, sizeof(name), "%s: nifm still reports 10.0.2.15", when);
            Check(name, R_SUCCEEDED(GetNifmAddress(std::addressof(address))) && address == Pack(GuestAddress), address);
        }

        /* The network goes away and comes back; requests follow. */
        void TestNetworkLoss() {
            Result rc = ConnectToBsdConfig();
            Check("bsdcfg accepts a connection", R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) {
                return;
            }

            IfState state = {};
            rc = BsdConfigGetState(std::addressof(state));
            Check("bsdcfg GetIfState reports an address", R_SUCCEEDED(rc) && (state.flags & IfStateHasAddress) != 0 && state.address == Pack(GuestAddress) && state.mtu == 1500, rc);

            ::NifmRequest accepted;
            rc = ::nifmCreateRequest(std::addressof(accepted), true);
            ::nifmRequestSubmit(std::addressof(accepted));
            Check("a request is Accepted before the network is taken down", R_SUCCEEDED(rc) && GetState(std::addressof(accepted)) == Accepted, GetState(std::addressof(accepted)));
            WaitSignalled(std::addressof(accepted.event_request_state), 0);
            IsSignalled(std::addressof(accepted.event1));

            rc = BsdConfigSetDown();
            Check("bsdcfg SetIfDown succeeds", R_SUCCEEDED(rc), rc);

            /* The monitor sees the change through bsdcfg's event; give it a moment. */
            Check("the accepted request's state event is signalled", WaitSignalled(std::addressof(accepted.event_request_state), 3000), 0);
            Check("its network event is signalled too", WaitSignalled(std::addressof(accepted.event1), 1000), 0);
            Check("it is Free again", GetState(std::addressof(accepted)) == Free, GetState(std::addressof(accepted)));
            Check("with the result 'connection reset' (2110-2003)", GetResultValue(std::addressof(accepted)) == ResultConnectionReset, GetResultValue(std::addressof(accepted)));

            u32 address = 1;
            rc = GetNifmAddress(std::addressof(address));
            Check("GetCurrentIpAddress fails without an address (2110-1100)", rc == ResultNetworkNotAvailable, rc);

            ::NifmInternetConnectionType type;
            u32 strength;
            ::NifmInternetConnectionStatus status;
            rc = ::nifmGetInternetConnectionStatus(std::addressof(type), std::addressof(strength), std::addressof(status));
            Check("GetInternetConnectionStatus fails when the interface is down (2110-1100)", rc == ResultNetworkNotAvailable, rc);

            /* Submitted while the network is down but the device exists: it waits. */
            ::NifmRequest waiting, cancelled;
            ::nifmCreateRequest(std::addressof(waiting), true);
            ::nifmCreateRequest(std::addressof(cancelled), true);
            rc = ::nifmRequestSubmit(std::addressof(waiting));
            ::nifmRequestSubmit(std::addressof(cancelled));
            Check("a request submitted with no address is OnHold", R_SUCCEEDED(rc) && GetState(std::addressof(waiting)) == OnHold, GetState(std::addressof(waiting)));

            /* Submit signalled the state event; what is checked below is the signal of the lease arriving. */
            IsSignalled(std::addressof(waiting.event_request_state));
            Check("its result is success while it waits", GetResultValue(std::addressof(waiting)) == 0, 0);
            ::nifmRequestCancel(std::addressof(cancelled));
            Check("an OnHold request can be cancelled", GetState(std::addressof(cancelled)) == Free, GetState(std::addressof(cancelled)));

            rc = BsdConfigSetUpDhcp();
            Check("bsdcfg SetIfUp with DHCP succeeds", R_SUCCEEDED(rc), rc);

            const u64 start = svcGetSystemTick();
            Check("the waiting request's state event is signalled when the lease arrives", WaitSignalled(std::addressof(waiting.event_request_state), 15000), ElapsedMilliSeconds(start));
            Check("it is Accepted", GetState(std::addressof(waiting)) == Accepted, GetState(std::addressof(waiting)));
            Check("the cancelled one stayed Free", GetState(std::addressof(cancelled)) == Free, GetState(std::addressof(cancelled)));

            Check("the address is 10.0.2.15 again", WaitForAddress(std::addressof(address), 5000) && address == Pack(GuestAddress), address);
            ::NifmInternetConnectionType type2 = static_cast<::NifmInternetConnectionType>(0);
            ::NifmInternetConnectionStatus status2 = NifmInternetConnectionStatus_ConnectingUnknown1;
            rc = ::nifmGetInternetConnectionStatus(std::addressof(type2), std::addressof(strength), std::addressof(status2));
            Check("the connection status is Connected again", R_SUCCEEDED(rc) && status2 == NifmInternetConnectionStatus_Connected, rc);

            /* Renewing an interface that is already doing DHCP must not drop the lease. */
            ::nifmRequestClose(std::addressof(accepted));
            ::nifmRequestClose(std::addressof(waiting));
            ::nifmRequestClose(std::addressof(cancelled));
            rc = BsdConfigSetUpDhcp();
            svcSleepThread(INT64_C(1000000) * (300));
            Check("SetIfUp with DHCP on an interface already doing DHCP keeps the lease", R_SUCCEEDED(rc) && R_SUCCEEDED(GetNifmAddress(std::addressof(address))) && address == Pack(GuestAddress), rc);
        }

        void TestErrors() {
            /* A command that is in the table but not implemented answers an error and leaves the session working. */
            ::Service *general = ::nifmGetServiceSession_GeneralService();
            serviceAssumeDomain(general);

            Result rc = serviceDispatch(general, 26);
            Check("an unimplemented IGeneralService command (26) fails with NotSupported", (ResultNotSupported == rc), rc);
            Check("the session still works afterwards", ::nifmGetClientId().id != 0, 0);

            /* nifm:u may not call the system commands. libnx would refuse before asking, so ask directly. */
            const u8 enable = 1;
            rc = serviceDispatchIn(general, 16, enable);
            Check("SetWirelessCommunicationEnabled on nifm:u is refused by the service (2110-0120)", rc == ResultInvalidPermission, rc);

            const s32 time = 0;
            rc = serviceDispatchIn(general, 43, time);
            Check("SetWowlDelayedWakeTime on nifm:u is refused by the service (2110-0120)", rc == ResultInvalidPermission, rc);

            ::NifmRequest request;
            rc = ::nifmCreateRequest(std::addressof(request), true);
            if (R_SUCCEEDED(rc)) {
                rc = serviceDispatch(std::addressof(request.s), 19);
                Check("an unimplemented IRequest command (19, GetRequirement) fails with NotSupported", (ResultNotSupported == rc), rc);
                Check("the request still works afterwards", R_SUCCEEDED(::nifmRequestSubmit(std::addressof(request))) && GetState(std::addressof(request)) == Accepted, 0);
                ::nifmRequestClose(std::addressof(request));
            } else {
                Check("a request for the IRequest stub check", false, rc);
            }

            /* The system and admin ports accept what nifm:u refuses. */
            ::nifmExit();
            rc = ::nifmInitialize(NifmServiceType_System);
            const Result rc_set = R_SUCCEEDED(rc) ? ::nifmSetWirelessCommunicationEnabled(true) : rc;
            bool wireless = true;
            const Result rc_get = R_SUCCEEDED(rc) ? ::nifmIsWirelessCommunicationEnabled(std::addressof(wireless)) : rc;
            Check("SetWirelessCommunicationEnabled on nifm:s is accepted, and there is still no wireless", R_SUCCEEDED(rc_set) && R_SUCCEEDED(rc_get) && !wireless, rc_set);

            ::nifmExit();
            rc = ::nifmInitialize(NifmServiceType_Admin);
            NifmNetworkProfileData profile = {};
            Uuid id = {};
            const Result rc_save = R_SUCCEEDED(rc) ? ::nifmSetNetworkProfile(std::addressof(profile), std::addressof(id)) : rc;
            Check("SetNetworkProfile on nifm:a is not supported", (ResultNotSupported == rc_save), rc_save);

            ::nifmExit();
            rc = ::nifmInitialize(NifmServiceType_User);
            Check("back on nifm:u", R_SUCCEEDED(rc), rc);
        }

        /* ===== Without a network device ===== */

        void TestNoDevice() {
            u32 address = 1;
            Result rc = GetNifmAddress(std::addressof(address));
            Check("GetCurrentIpAddress fails (2110-1100)", rc == ResultNetworkNotAvailable, rc);

            u32 a, b, c, d, e;
            rc = ::nifmGetCurrentIpConfigInfo(std::addressof(a), std::addressof(b), std::addressof(c), std::addressof(d), std::addressof(e));
            Check("GetCurrentIpConfigInfo fails (2110-1100)", rc == ResultNetworkNotAvailable, rc);

            ::NifmInternetConnectionType type;
            u32 strength;
            ::NifmInternetConnectionStatus status;
            rc = ::nifmGetInternetConnectionStatus(std::addressof(type), std::addressof(strength), std::addressof(status));
            Check("GetInternetConnectionStatus fails (2110-1100)", rc == ResultNetworkNotAvailable, rc);

            NifmNetworkProfileData profile = {};
            rc = ::nifmGetCurrentNetworkProfile(std::addressof(profile));
            Check("GetCurrentNetworkProfile fails (2110-1100)", rc == ResultNetworkNotAvailable, rc);

            ::NifmRequest request;
            rc = ::nifmCreateRequest(std::addressof(request), true);
            Check("a request can still be created", R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) {
                return;
            }
            rc = ::nifmRequestSubmit(std::addressof(request));
            Check("Submit succeeds", R_SUCCEEDED(rc), rc);
            Check("the request stays Free", GetState(std::addressof(request)) == Free, GetState(std::addressof(request)));
            Check("its result is 'network not available' (2110-1100)", GetResultValue(std::addressof(request)) == ResultNetworkNotAvailable, GetResultValue(std::addressof(request)));
            /* libnx's SubmitAndWait returns what Submit returned; the failure is read from the request. */
            rc = ::nifmRequestSubmitAndWait(std::addressof(request));
            Check("SubmitAndWait returns, and the request is still Free with the same result", R_SUCCEEDED(rc) && GetState(std::addressof(request)) == Free && GetResultValue(std::addressof(request)) == ResultNetworkNotAvailable, rc);
            Check("no request is accepted", !::nifmIsAnyInternetRequestAccepted(::nifmGetClientId()), 0);
            ::nifmRequestClose(std::addressof(request));
        }

    }

    void RunTest() {
        Log("[nifm_test] starting\n");

        /* The sockets module is needed for the address checks. */
        Result rc = ::socketInitialize(nullptr);
        Check("socketInitialize", R_SUCCEEDED(rc), rc);

        if (R_SUCCEEDED(rc) && TestService()) {
            TestOrdinarySessions();
            /* Does the sockets module have a network device? It answers through bsdcfg, which this module started. */
            IfState state = {};
            Result cfg = ConnectToBsdConfig();
            while (R_FAILED(cfg)) {
                svcSleepThread(INT64_C(1000000) * (100));
                cfg = ConnectToBsdConfig();
            }
            u16 pointer_buffer_size = 0xffff;
            Result query = ::cmifQueryPointerBufferSize(g_bsdcfg.session, &pointer_buffer_size);
            Check("bsdcfg reports no pointer buffer", R_SUCCEEDED(query) && pointer_buffer_size == 0, query);
            const u8 input = 0;
            Result stub = serviceDispatch(std::addressof(g_bsdcfg), 13,
                .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
                .buffers = { { std::addressof(input), sizeof(input) } },
            );
            Check("bsdcfg mapped-buffer stub succeeds", R_SUCCEEDED(stub), stub);
            Result unsupported = serviceDispatch(std::addressof(g_bsdcfg), BsdCfgCommand100);
            Check("bsdcfg unsupported command returns NotSupported", (ResultNotSupported == unsupported), unsupported);
            Result unknown = serviceDispatch(std::addressof(g_bsdcfg), 0xffff);
            Check("bsdcfg closes the session on an unknown command", R_FAILED(unknown), unknown);
            ::serviceClose(std::addressof(g_bsdcfg));
            // More iterations than the server's handle table can hold, so a
            // leaked copy of the incoming event eventually prevents progress.
            Handle writable_event = INVALID_HANDLE, readable_event = INVALID_HANDLE;
            Result create_event = ::svcCreateEvent(&writable_event, &readable_event);
            bool rejected_handles = R_SUCCEEDED(create_event);
            for (int i = 0; rejected_handles && i < 270; ++i) {
                Result open = ConnectToBsdConfig();
                if (R_FAILED(open)) { rejected_handles = false; break; }
                const u32 index = 0;
                Result bad = serviceDispatchIn(std::addressof(g_bsdcfg), BsdCfgGetIfState, index,
                    .in_num_handles = 1,
                    .in_handles = { readable_event },
                );
                rejected_handles = R_FAILED(bad);
                ::serviceClose(std::addressof(g_bsdcfg));
            }
            if (writable_event != INVALID_HANDLE) ::svcCloseHandle(writable_event);
            if (readable_event != INVALID_HANDLE) ::svcCloseHandle(readable_event);
            Check("bsdcfg rejects incoming handles without exhausting its handle table", rejected_handles, create_event);
            cfg = ConnectToBsdConfig();
            Check("bsdcfg accepts a new session after close", R_SUCCEEDED(cfg), cfg);
            const bool has_device = R_SUCCEEDED(BsdConfigGetState(std::addressof(state)));
            Log("[nifm_test] network device: %s\n", has_device ? "present" : "none");

            if (has_device) {
                TestAddress();
                TestStatus();
                TestProfile();
                TestRequest();
                TestSockets("before");
                TestNetworkLoss();
                TestSockets("after");
                TestErrors();
            } else {
                TestNoDevice();
            }
        }

        Log("[nifm_test] done: %d checks, %d failure(s)\n", g_checks, g_failures);

        /* Idle forever; this KIP is immortal. */
        while (true) {
            svcSleepThread(INT64_C(1000000000) * (60));
        }
    }

}

extern "C" {
    u32 __nx_applet_type = AppletType_None;
    void __libnx_initheap() {
        extern char *fake_heap_start, *fake_heap_end;
        fake_heap_start = reinterpret_cast<char *>(g_malloc_memory);
        fake_heap_end = fake_heap_start + sizeof(g_malloc_memory);
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
