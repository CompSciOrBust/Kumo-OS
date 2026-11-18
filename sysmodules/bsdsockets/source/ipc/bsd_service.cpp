#include <kumo/prelude.hpp>
#include "bsd_service.hpp"
#include "../bsd_log.hpp"

namespace kumo::horizon::hznet::ipc {

    namespace {

        using socket::CallResult;

        constexpr CallResult NotSupported = { -1, wire::Errno_EOPNOTSUPP };
        constexpr CallResult TooManyClients = { -1, wire::Errno_ENOMEM };

        /* fd_set as libnx (newlib) lays it out: a bit per descriptor, least significant bit first. */
        constexpr s32 SelectMaxDescriptors = 1024;

        bool TestBit(const u8 *set, size_t size, s32 fd) {
            return set != nullptr && static_cast<size_t>(fd / 8) < size && (set[fd / 8] & (1u << (fd % 8))) != 0;
        }

        void SetBit(u8 *set, size_t size, s32 fd) {
            if (set != nullptr && static_cast<size_t>(fd / 8) < size) {
                set[fd / 8] |= static_cast<u8>(1u << (fd % 8));
            }
        }

    }

    #define HZNET_RETURN(expr)                      \
        do {                                        \
            const CallResult _result = (expr);      \
            out_ret.SetValue(_result.value);        \
            out_errno.SetValue(_result.error);      \
            KUMO_SUCCEED();                            \
        } while (0)

    /* Every socket command starts by finding its client. */
    #define HZNET_GET_CLIENT(name)                  \
        Client * const name = this->GetClient();    \
        if (name == nullptr) {                      \
            HZNET_RETURN(TooManyClients);           \
        }

    BsdService::~BsdService() {
        /* If this was a monitoring session, the process it watched has exited or closed the service. */
        /* Either way its calls must not stay blocked and its sockets must not stay open. */
        if (m_monitored_client != nullptr) {
            AbandonClient(m_monitored_client);
            ReleaseClient(m_monitored_client);
        }
        if (m_client != nullptr) {
            ReleaseClient(m_client);
        }
    }

    Client *BsdService::GetClient() {
        std::scoped_lock lk(m_mutex);
        if (m_client == nullptr) {
            m_client = CreateClient();
        }
        return m_client;
    }

    ::Result BsdService::RegisterClient(kumo::ipc::Out<s32> out_errno, LibraryConfigData config, const kumo::ipc::ClientProcessId &client_pid, u64 transfer_memory_size, kumo::ipc::CopyHandle &&transfer_memory) {
        /* The official service takes its socket buffers from the client's transfer memory. This one */
        /* buffers in lwIP's own pools, so the memory, and the sizes asked for, are not used. */
        KUMO_UNUSED(config, transfer_memory_size);
        ::svcCloseHandle(transfer_memory.GetOsHandle());
        transfer_memory.Detach();

        Client *client = this->GetClient();
        if (client == nullptr) {
            out_errno.SetValue(wire::Errno_ENOMEM);
            KUMO_SUCCEED();
        }

        ipc::RegisterClient(client, client_pid.GetValue());
        out_errno.SetValue(0);
        KUMO_SUCCEED();
    }

    ::Result BsdService::RegisterClientShared(kumo::ipc::Out<s32> out_errno, LibraryConfigData config, const kumo::ipc::ClientProcessId &client_pid, u64 work_buffer_size) {
        KUMO_UNUSED(config, work_buffer_size);

        Client *client = this->GetClient();
        if (client == nullptr) {
            out_errno.SetValue(wire::Errno_ENOMEM);
            KUMO_SUCCEED();
        }

        ipc::RegisterClient(client, client_pid.GetValue());
        out_errno.SetValue(0);
        KUMO_SUCCEED();
    }

    ::Result BsdService::StartMonitoring(const kumo::ipc::ClientProcessId &client_pid) {
        std::scoped_lock lk(m_mutex);

        /* From here on, this session closing means the process is gone. */
        if (m_monitored_client == nullptr) {
            m_monitored_client = AcquireClientForMonitoring(client_pid.GetValue());
        }
        KUMO_SUCCEED();
    }

    ::Result BsdService::Socket(HZNET_BSD_OUT, s32 domain, s32 type, s32 protocol) {
        HZNET_GET_CLIENT(client);

        /* Raw sockets are for bsd:s only on the Switch; here nobody has them yet. */
        HZNET_RETURN(socket::Create(client->m_table, domain, type, protocol));
    }

    ::Result BsdService::SocketExempt(HZNET_BSD_OUT, s32 domain, s32 type, s32 protocol) {
        /* On the Switch such a socket survives ShutdownAllSockets when the console sleeps. There is no sleep here. */
        KUMO_RETURN(this->Socket(out_ret, out_errno, domain, type, protocol));
    }

    ::Result BsdService::Open(HZNET_BSD_OUT, HZNET_BSD_IN path, s32 flags) {
        /* Only /dev/bpf can be opened on the Switch; packet capture is not implemented. */
        KUMO_UNUSED(path, flags);
        HZNET_RETURN(NotSupported);
    }

    ::Result BsdService::Select(HZNET_BSD_OUT, SelectInput in, HZNET_BSD_IN read_in, HZNET_BSD_IN write_in, HZNET_BSD_IN except_in, HZNET_BSD_OUTBUF read_out, HZNET_BSD_OUTBUF write_out, HZNET_BSD_OUTBUF except_out) {
        HZNET_GET_CLIENT(client);

        if (in.nfds < 0 || in.nfds > SelectMaxDescriptors) {
            HZNET_RETURN((CallResult{ -1, wire::Errno_EINVAL }));
        }

        /* select is poll with a different way of naming descriptors. */
        static_assert(socket::MaxDescriptorsPerClient <= 128);
        wire::PollFd fds[socket::MaxDescriptorsPerClient];
        u32 count = 0;
        for (s32 fd = 0; fd < in.nfds; ++fd) {
            s16 events = 0;
            if (TestBit(read_in.GetPointer(), read_in.GetSize(), fd)) {
                events |= wire::POLLIN_;
            }
            if (TestBit(write_in.GetPointer(), write_in.GetSize(), fd)) {
                events |= wire::POLLOUT_;
            }
            const bool wants_except = TestBit(except_in.GetPointer(), except_in.GetSize(), fd);
            if (events == 0 && !wants_except) {
                continue;
            }
            if (fd >= socket::MaxDescriptorsPerClient) {
                HZNET_RETURN((CallResult{ -1, wire::Errno_EBADF }));
            }
            fds[count++] = { fd, events, 0 };
        }

        s32 timeout_ms = -1;
        if (!in.timeout_is_null) {
            if (in.timeout_sec < 0 || in.timeout_usec < 0 || in.timeout_sec > 0x7FFFFFFF / 1000 - 1) {
                HZNET_RETURN((CallResult{ -1, wire::Errno_EINVAL }));
            }
            timeout_ms = static_cast<s32>(in.timeout_sec * 1000 + (in.timeout_usec + 999) / 1000);
        }

        const CallResult result = socket::Poll(client->m_table, fds, count, timeout_ms);
        if (result.value < 0) {
            HZNET_RETURN(result);
        }

        std::memset(read_out.GetPointer(),   0, read_out.GetSize());
        std::memset(write_out.GetPointer(),  0, write_out.GetSize());
        std::memset(except_out.GetPointer(), 0, except_out.GetSize());

        s32 ready = 0;
        for (u32 i = 0; i < count; ++i) {
            if ((fds[i].revents & wire::POLLNVAL_) != 0) {
                HZNET_RETURN((CallResult{ -1, wire::Errno_EBADF }));
            }

            /* An error or hang-up makes a descriptor both readable and writable: the next call reports it. */
            constexpr s16 Broken = wire::POLLERR_ | wire::POLLHUP_;
            if ((fds[i].events & wire::POLLIN_) != 0 && (fds[i].revents & (wire::POLLIN_ | Broken)) != 0) {
                SetBit(read_out.GetPointer(), read_out.GetSize(), fds[i].fd);
                ++ready;
            }
            if ((fds[i].events & wire::POLLOUT_) != 0 && (fds[i].revents & (wire::POLLOUT_ | Broken)) != 0) {
                SetBit(write_out.GetPointer(), write_out.GetSize(), fds[i].fd);
                ++ready;
            }
            /* The exceptional condition select reports is out-of-band data, which is not supported. */
        }
        HZNET_RETURN((CallResult{ ready, 0 }));
    }

    ::Result BsdService::Poll(HZNET_BSD_OUT, u32 nfds, s32 timeout, HZNET_BSD_IN fds_in, HZNET_BSD_OUTBUF fds_out) {
        HZNET_GET_CLIENT(client);

        const size_t size = static_cast<size_t>(nfds) * sizeof(wire::PollFd);
        if (fds_in.GetSize() < size || fds_out.GetSize() < size) {
            HZNET_RETURN((CallResult{ -1, wire::Errno_EINVAL }));
        }

        /* The result is the input with revents filled in, so work in the output buffer. */
        if (size > 0) {
            std::memmove(fds_out.GetPointer(), fds_in.GetPointer(), size);
        }
        HZNET_RETURN(socket::Poll(client->m_table, fds_out.GetPointer(), nfds, timeout));
    }

    ::Result BsdService::Sysctl(HZNET_BSD_OUT, kumo::ipc::Out<u64> out_size, HZNET_BSD_IN name, HZNET_BSD_IN new_value, HZNET_BSD_OUTBUF old_value) {
        KUMO_UNUSED(name, new_value, old_value);
        out_size.SetValue(0);
        HZNET_RETURN(NotSupported);
    }

    ::Result BsdService::Recv(HZNET_BSD_OUT, s32 fd, s32 flags, HZNET_BSD_OUTBUF buffer) {
        HZNET_GET_CLIENT(client);
        HZNET_RETURN(socket::Receive(client->m_table, fd, buffer.GetPointer(), buffer.GetSize(), flags, nullptr, 0, nullptr));
    }

    ::Result BsdService::RecvFrom(HZNET_BSD_OUT, kumo::ipc::Out<u32> out_address_size, s32 fd, s32 flags, HZNET_BSD_OUTBUF buffer, HZNET_BSD_OUTBUF address) {
        out_address_size.SetValue(0);
        HZNET_GET_CLIENT(client);

        u32 address_size = 0;
        const CallResult result = socket::Receive(client->m_table, fd, buffer.GetPointer(), buffer.GetSize(), flags, address.GetPointer(), address.GetSize(), std::addressof(address_size));
        out_address_size.SetValue(address_size);
        HZNET_RETURN(result);
    }

    ::Result BsdService::Send(HZNET_BSD_OUT, s32 fd, s32 flags, HZNET_BSD_IN buffer) {
        HZNET_GET_CLIENT(client);
        HZNET_RETURN(socket::Send(client->m_table, fd, buffer.GetPointer(), buffer.GetSize(), flags, nullptr, 0));
    }

    ::Result BsdService::SendTo(HZNET_BSD_OUT, s32 fd, s32 flags, HZNET_BSD_IN buffer, HZNET_BSD_IN address) {
        HZNET_GET_CLIENT(client);
        HZNET_RETURN(socket::Send(client->m_table, fd, buffer.GetPointer(), buffer.GetSize(), flags, address.GetSize() > 0 ? address.GetPointer() : nullptr, address.GetSize()));
    }

    ::Result BsdService::Accept(HZNET_BSD_OUT, kumo::ipc::Out<u32> out_address_size, s32 fd, HZNET_BSD_OUTBUF address) {
        out_address_size.SetValue(0);
        HZNET_GET_CLIENT(client);

        u32 address_size = 0;
        const CallResult result = socket::Accept(client->m_table, fd, address.GetPointer(), address.GetSize(), std::addressof(address_size));
        out_address_size.SetValue(address_size);
        HZNET_RETURN(result);
    }

    ::Result BsdService::Bind(HZNET_BSD_OUT, s32 fd, HZNET_BSD_IN address) {
        HZNET_GET_CLIENT(client);
        HZNET_RETURN(socket::Bind(client->m_table, fd, address.GetPointer(), address.GetSize()));
    }

    ::Result BsdService::Connect(HZNET_BSD_OUT, s32 fd, HZNET_BSD_IN address) {
        HZNET_GET_CLIENT(client);
        HZNET_RETURN(socket::Connect(client->m_table, fd, address.GetPointer(), address.GetSize()));
    }

    ::Result BsdService::GetPeerName(HZNET_BSD_OUT, kumo::ipc::Out<u32> out_address_size, s32 fd, HZNET_BSD_OUTBUF address) {
        out_address_size.SetValue(0);
        HZNET_GET_CLIENT(client);

        u32 address_size = 0;
        const CallResult result = socket::GetPeerName(client->m_table, fd, address.GetPointer(), address.GetSize(), std::addressof(address_size));
        out_address_size.SetValue(address_size);
        HZNET_RETURN(result);
    }

    ::Result BsdService::GetSockName(HZNET_BSD_OUT, kumo::ipc::Out<u32> out_address_size, s32 fd, HZNET_BSD_OUTBUF address) {
        out_address_size.SetValue(0);
        HZNET_GET_CLIENT(client);

        u32 address_size = 0;
        const CallResult result = socket::GetSockName(client->m_table, fd, address.GetPointer(), address.GetSize(), std::addressof(address_size));
        out_address_size.SetValue(address_size);
        HZNET_RETURN(result);
    }

    ::Result BsdService::GetSockOpt(HZNET_BSD_OUT, kumo::ipc::Out<u32> out_value_size, s32 fd, s32 level, s32 name, HZNET_BSD_OUTBUF value) {
        out_value_size.SetValue(0);
        HZNET_GET_CLIENT(client);

        u32 value_size = 0;
        const CallResult result = socket::GetSockOpt(client->m_table, fd, level, name, value.GetPointer(), value.GetSize(), std::addressof(value_size));
        out_value_size.SetValue(value_size);
        HZNET_RETURN(result);
    }

    ::Result BsdService::Listen(HZNET_BSD_OUT, s32 fd, s32 backlog) {
        HZNET_GET_CLIENT(client);
        HZNET_RETURN(socket::Listen(client->m_table, fd, backlog));
    }

    ::Result BsdService::Ioctl(HZNET_BSD_OUT, s32 fd, u32 request, u32 buffer_count, HZNET_BSD_IN in1, HZNET_BSD_IN in2, HZNET_BSD_IN in3, HZNET_BSD_IN in4, HZNET_BSD_OUTBUF out1, HZNET_BSD_OUTBUF out2, HZNET_BSD_OUTBUF out3, HZNET_BSD_OUTBUF out4) {
        /* The requests implemented all carry a single int, which travels in the first buffer of its direction. */
        KUMO_UNUSED(buffer_count, in2, in3, in4, out2, out3, out4);
        HZNET_GET_CLIENT(client);
        HZNET_RETURN(socket::Ioctl(client->m_table, fd, request, in1.GetPointer(), in1.GetSize(), out1.GetPointer(), out1.GetSize()));
    }

    ::Result BsdService::Fcntl(HZNET_BSD_OUT, s32 fd, s32 command, s32 flags) {
        HZNET_GET_CLIENT(client);
        HZNET_RETURN(socket::Fcntl(client->m_table, fd, command, flags));
    }

    ::Result BsdService::SetSockOpt(HZNET_BSD_OUT, s32 fd, s32 level, s32 name, HZNET_BSD_IN value) {
        HZNET_GET_CLIENT(client);
        HZNET_RETURN(socket::SetSockOpt(client->m_table, fd, level, name, value.GetPointer(), value.GetSize()));
    }

    ::Result BsdService::Shutdown(HZNET_BSD_OUT, s32 fd, s32 how) {
        HZNET_GET_CLIENT(client);
        HZNET_RETURN(socket::Shutdown(client->m_table, fd, how));
    }

    ::Result BsdService::ShutdownAllSockets(HZNET_BSD_OUT, s32 how) {
        HZNET_GET_CLIENT(client);
        HZNET_RETURN(socket::ShutdownAll(client->m_table, how));
    }

    ::Result BsdService::Write(HZNET_BSD_OUT, s32 fd, HZNET_BSD_IN buffer) {
        HZNET_GET_CLIENT(client);
        HZNET_RETURN(socket::Send(client->m_table, fd, buffer.GetPointer(), buffer.GetSize(), 0, nullptr, 0));
    }

    ::Result BsdService::Read(HZNET_BSD_OUT, s32 fd, HZNET_BSD_OUTBUF buffer) {
        HZNET_GET_CLIENT(client);
        HZNET_RETURN(socket::Receive(client->m_table, fd, buffer.GetPointer(), buffer.GetSize(), 0, nullptr, 0, nullptr));
    }

    ::Result BsdService::Close(HZNET_BSD_OUT, s32 fd) {
        HZNET_GET_CLIENT(client);
        HZNET_RETURN(socket::Close(client->m_table, fd));
    }

    ::Result BsdService::DuplicateSocket(HZNET_BSD_OUT, s32 fd, u64 reserved) {
        KUMO_UNUSED(reserved);

        /* Reserved to bsd:s, as on the Switch. */
        if (!m_is_system) {
            HZNET_RETURN((CallResult{ -1, wire::Errno_EPERM }));
        }

        HZNET_GET_CLIENT(client);
        HZNET_RETURN(socket::Duplicate(client->m_table, fd));
    }

    /* ---- Not supported ---- */

    ::Result BsdService::GetResourceStatistics(HZNET_BSD_OUT, const kumo::ipc::ClientProcessId &client_pid, u64 unknown, HZNET_BSD_OUTBUF statistics, HZNET_BSD_IN name) {
        KUMO_UNUSED(client_pid, unknown, statistics, name);
        HZNET_RETURN(NotSupported);
    }

    ::Result BsdService::RecvMMsg(HZNET_BSD_OUT, RecvMMsgInput in, const kumo::ipc::OutBuffer &buffer) {
        KUMO_UNUSED(in, buffer);
        HZNET_RETURN(NotSupported);
    }

    ::Result BsdService::SendMMsg(HZNET_BSD_OUT, s32 fd, s32 vlen, s32 flags, const kumo::ipc::OutBuffer &buffer) {
        KUMO_UNUSED(fd, vlen, flags, buffer);
        HZNET_RETURN(NotSupported);
    }

    ::Result BsdService::Cmd36(kumo::ipc::Out<s32> out_errno, u32 version, const kumo::ipc::ClientProcessId &client_pid) {
        /* A newer form of RegisterClient that is a stub on the Switch: it returns 0. */
        KUMO_UNUSED(version, client_pid);
        out_errno.SetValue(0);
        KUMO_SUCCEED();
    }

    ::Result BsdService::Cmd38(HZNET_BSD_OUT, u32 unknown, HZNET_BSD_IN buffer) {
        KUMO_UNUSED(unknown, buffer);
        HZNET_RETURN(NotSupported);
    }

    #define HZNET_DEFINE_NOT_SUPPORTED(NAME) \
        ::Result BsdService::NAME(HZNET_BSD_OUT) { HZNET_RETURN(NotSupported); }

    HZNET_DEFINE_NOT_SUPPORTED(EventFd)
    HZNET_DEFINE_NOT_SUPPORTED(RegisterResourceStatisticsName)
    HZNET_DEFINE_NOT_SUPPORTED(GetSocketStatistics)
    HZNET_DEFINE_NOT_SUPPORTED(NifIoctl)
    HZNET_DEFINE_NOT_SUPPORTED(SetSocketBufferAttribute)
    HZNET_DEFINE_NOT_SUPPORTED(NoSfEventFd)
    HZNET_DEFINE_NOT_SUPPORTED(ShutdownAllSockets2)
    HZNET_DEFINE_NOT_SUPPORTED(Cmd41)
    HZNET_DEFINE_NOT_SUPPORTED(SendMMsgNoMem)
    HZNET_DEFINE_NOT_SUPPORTED(RecvMMsgNoMem)
    HZNET_DEFINE_NOT_SUPPORTED(Cmd44)
    HZNET_DEFINE_NOT_SUPPORTED(SetThreadCoreMask)
    HZNET_DEFINE_NOT_SUPPORTED(GetThreadCoreMask)

}
