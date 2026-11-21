/*
 * The session object of bsd:u and bsd:s. Each command unpacks its buffers,
 * calls the socket layer, and returns the (value, errno) pair every bsd
 * command carries.
 */
#pragma once
#include <kumo/prelude.hpp>
#include <kumo/mutex.hpp>
#include "bsd_interface.hpp"
#include "bsd_client.hpp"

namespace kumo::horizon::hznet::ipc {

    class BsdService {
        private:
            const bool m_is_system;         /* opened through bsd:s */
            kumo::Mutex m_mutex;           /* guards the two pointers: cloned sessions share this object */
            Client *m_client;               /* the client this session issues calls for */
            Client *m_monitored_client;     /* the client this session watches (StartMonitoring) */
        public:
            explicit BsdService(bool is_system) : m_is_system(is_system), m_mutex(), m_client(nullptr), m_monitored_client(nullptr) { /* ... */ }
            ~BsdService();
        private:
            /* The client of this session, created on first use. nullptr when there are too many clients. */
            Client *GetClient();
        public:
            ::Result RegisterClient(kumo::ipc::Out<s32> out_errno, LibraryConfigData config, const kumo::ipc::ClientProcessId &client_pid, u64 transfer_memory_size, kumo::ipc::CopyHandle &&transfer_memory);
            ::Result StartMonitoring(const kumo::ipc::ClientProcessId &client_pid);
            ::Result Socket(HZNET_BSD_OUT, s32 domain, s32 type, s32 protocol);
            ::Result SocketExempt(HZNET_BSD_OUT, s32 domain, s32 type, s32 protocol);
            ::Result Open(HZNET_BSD_OUT, HZNET_BSD_IN path, s32 flags);
            ::Result Select(HZNET_BSD_OUT, SelectInput in, HZNET_BSD_IN read_in, HZNET_BSD_IN write_in, HZNET_BSD_IN except_in, HZNET_BSD_OUTBUF read_out, HZNET_BSD_OUTBUF write_out, HZNET_BSD_OUTBUF except_out);
            ::Result Poll(HZNET_BSD_OUT, u32 nfds, s32 timeout, HZNET_BSD_IN fds_in, HZNET_BSD_OUTBUF fds_out);
            ::Result Sysctl(HZNET_BSD_OUT, kumo::ipc::Out<u64> out_size, HZNET_BSD_IN name, HZNET_BSD_IN new_value, HZNET_BSD_OUTBUF old_value);
            ::Result Recv(HZNET_BSD_OUT, s32 fd, s32 flags, HZNET_BSD_OUTBUF buffer);
            ::Result RecvFrom(HZNET_BSD_OUT, kumo::ipc::Out<u32> out_address_size, s32 fd, s32 flags, HZNET_BSD_OUTBUF buffer, HZNET_BSD_OUTBUF address);
            ::Result Send(HZNET_BSD_OUT, s32 fd, s32 flags, HZNET_BSD_IN buffer);
            ::Result SendTo(HZNET_BSD_OUT, s32 fd, s32 flags, HZNET_BSD_IN buffer, HZNET_BSD_IN address);
            ::Result Accept(HZNET_BSD_OUT, kumo::ipc::Out<u32> out_address_size, s32 fd, HZNET_BSD_OUTBUF address);
            ::Result Bind(HZNET_BSD_OUT, s32 fd, HZNET_BSD_IN address);
            ::Result Connect(HZNET_BSD_OUT, s32 fd, HZNET_BSD_IN address);
            ::Result GetPeerName(HZNET_BSD_OUT, kumo::ipc::Out<u32> out_address_size, s32 fd, HZNET_BSD_OUTBUF address);
            ::Result GetSockName(HZNET_BSD_OUT, kumo::ipc::Out<u32> out_address_size, s32 fd, HZNET_BSD_OUTBUF address);
            ::Result GetSockOpt(HZNET_BSD_OUT, kumo::ipc::Out<u32> out_value_size, s32 fd, s32 level, s32 name, HZNET_BSD_OUTBUF value);
            ::Result Listen(HZNET_BSD_OUT, s32 fd, s32 backlog);
            ::Result Ioctl(HZNET_BSD_OUT, s32 fd, u32 request, u32 buffer_count, HZNET_BSD_IN in1, HZNET_BSD_IN in2, HZNET_BSD_IN in3, HZNET_BSD_IN in4, HZNET_BSD_OUTBUF out1, HZNET_BSD_OUTBUF out2, HZNET_BSD_OUTBUF out3, HZNET_BSD_OUTBUF out4);
            ::Result Fcntl(HZNET_BSD_OUT, s32 fd, s32 command, s32 flags);
            ::Result SetSockOpt(HZNET_BSD_OUT, s32 fd, s32 level, s32 name, HZNET_BSD_IN value);
            ::Result Shutdown(HZNET_BSD_OUT, s32 fd, s32 how);
            ::Result ShutdownAllSockets(HZNET_BSD_OUT, s32 how);
            ::Result Write(HZNET_BSD_OUT, s32 fd, HZNET_BSD_IN buffer);
            ::Result Read(HZNET_BSD_OUT, s32 fd, HZNET_BSD_OUTBUF buffer);
            ::Result Close(HZNET_BSD_OUT, s32 fd);
            ::Result DuplicateSocket(HZNET_BSD_OUT, s32 fd, u64 source_pid);
            ::Result RegisterClientShared(kumo::ipc::Out<s32> out_errno, LibraryConfigData config, const kumo::ipc::ClientProcessId &client_pid, u64 work_buffer_size);

            /* Not supported. Each answers "success, -1, EOPNOTSUPP", so a client gets an error it can handle */
            /* and its session stays open. */
            ::Result GetResourceStatistics(HZNET_BSD_OUT, const kumo::ipc::ClientProcessId &client_pid, u64 unknown, HZNET_BSD_OUTBUF statistics, HZNET_BSD_IN name);
            ::Result RecvMMsg(HZNET_BSD_OUT, RecvMMsgInput in, const kumo::ipc::OutBuffer &buffer);
            ::Result SendMMsg(HZNET_BSD_OUT, s32 fd, s32 vlen, s32 flags, const kumo::ipc::OutBuffer &buffer);
            ::Result Cmd36(kumo::ipc::Out<s32> out_errno, u32 version, const kumo::ipc::ClientProcessId &client_pid);
            ::Result Cmd38(HZNET_BSD_OUT, u32 unknown, HZNET_BSD_IN buffer);
            ::Result EventFd(HZNET_BSD_OUT);
            ::Result RegisterResourceStatisticsName(HZNET_BSD_OUT);
            ::Result GetSocketStatistics(HZNET_BSD_OUT);
            ::Result NifIoctl(HZNET_BSD_OUT);
            ::Result SetSocketBufferAttribute(HZNET_BSD_OUT);
            ::Result NoSfEventFd(HZNET_BSD_OUT);
            ::Result ShutdownAllSockets2(HZNET_BSD_OUT);
            ::Result Cmd41(HZNET_BSD_OUT);
            ::Result SendMMsgNoMem(HZNET_BSD_OUT);
            ::Result RecvMMsgNoMem(HZNET_BSD_OUT);
            ::Result Cmd44(HZNET_BSD_OUT);
            ::Result SetThreadCoreMask(HZNET_BSD_OUT);
            ::Result GetThreadCoreMask(HZNET_BSD_OUT);
    };

}
