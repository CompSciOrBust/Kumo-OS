#!/usr/bin/env python3
"""Host side of sysmodules/tests/net_test.

The guest reaches these servers through QEMU's user-mode network, where the
host's loopback is 10.0.2.2. run-net-test.sh starts this script and QEMU
together; it can also be run by hand:

    sysmodules/tests/net_test/host_server.py [--base-port 47000] [--guest-tcp-port 47010] [--guest-udp-port 47011]

Ports, relative to the base port (the guest has the same numbers compiled in):

    +1  UDP echo      every datagram is sent back to its sender
    +2  TCP echo      every byte is sent back; closes when the client does
    +3  TCP sink      reads to end of stream, then replies with the byte count
                      and Adler-32 of what it read (two big-endian u32) and closes
    +4  TCP source    reads a seed and a length (two big-endian u32), sends that
                      many bytes of the test pattern, and closes
    +5  UDP control   "TCPIN": connect to the guest's listener through QEMU's
                      forwarded port, exchange greetings, and answer "TCPIN-OK"
                      or "TCPIN-FAIL <why>". "UDPIN": send a datagram to the
                      guest's forwarded UDP port and answer "UDPIN-SENT".
    +9  nothing       must stay closed: the guest expects a refused connection

The forwarded ports are the host ends of QEMU's hostfwd rules.
"""
import argparse
import socket
import struct
import sys
import threading
import zlib

HOST = "127.0.0.1"
GREETING = b"hello from the host\n"
REPLY = b"hello from the guest\n"


def log(message):
    print("[host] " + message, flush=True)


def pattern(length, seed):
    """The same bytes sysmodules/tests/net_test generates with FillPattern."""
    return bytes((((i * 31 + seed) ^ (i >> 8)) & 0xFF) for i in range(length))


def read_exact(conn, length):
    data = b""
    while len(data) < length:
        chunk = conn.recv(length - len(data))
        if not chunk:
            break
        data += chunk
    return data


def serve_tcp(port, handler, name):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((HOST, port))
    listener.listen(16)

    def run(conn, peer):
        try:
            handler(conn)
        except OSError as e:
            log("%s: %s" % (name, e))
        finally:
            conn.close()

    def accept_loop():
        while True:
            conn, peer = listener.accept()
            threading.Thread(target=run, args=(conn, peer), daemon=True).start()

    threading.Thread(target=accept_loop, daemon=True).start()


def tcp_echo(conn):
    total = 0
    while True:
        data = conn.recv(65536)
        if not data:
            break
        conn.sendall(data)
        total += len(data)
    log("tcp echo: connection closed after %d bytes" % total)


def tcp_sink(conn):
    total = 0
    checksum = 1
    while True:
        data = conn.recv(65536)
        if not data:
            break
        total += len(data)
        checksum = zlib.adler32(data, checksum)
    conn.sendall(struct.pack(">II", total & 0xFFFFFFFF, checksum))
    log("tcp sink: %d bytes, adler32 %08x" % (total, checksum))


def tcp_source(conn):
    header = read_exact(conn, 8)
    if len(header) != 8:
        return
    seed, length = struct.unpack(">II", header)
    conn.sendall(pattern(length, seed))
    log("tcp source: sent %d bytes with seed %d" % (length, seed))


def bind_udp(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((HOST, port))
    return sock


def udp_echo(sock):
    while True:
        data, peer = sock.recvfrom(65536)
        sock.sendto(data, peer)


def connect_in(guest_tcp_port):
    """Connect to the guest's listener and exchange greetings."""
    try:
        with socket.create_connection((HOST, guest_tcp_port), timeout=10) as conn:
            conn.settimeout(10)
            conn.sendall(GREETING)
            reply = read_exact(conn, len(REPLY))
            if reply != REPLY:
                return "TCPIN-FAIL unexpected reply %r" % reply
            conn.shutdown(socket.SHUT_WR)
            # The guest closes after reading our end of stream.
            if conn.recv(1) != b"":
                return "TCPIN-FAIL data after the reply"
        return "TCPIN-OK"
    except OSError as e:
        return "TCPIN-FAIL %s" % e


def udp_control(sock, guest_tcp_port, guest_udp_port):
    while True:
        data, peer = sock.recvfrom(2048)
        if data == b"TCPIN":
            def run(peer=peer):
                result = connect_in(guest_tcp_port)
                log("inbound tcp: " + result)
                sock.sendto(result.encode(), peer)
            threading.Thread(target=run, daemon=True).start()
        elif data == b"UDPIN":
            out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            out.sendto(b"datagram from the host", (HOST, guest_udp_port))
            out.close()
            sock.sendto(b"UDPIN-SENT", peer)
            log("inbound udp: sent")
        else:
            sock.sendto(b"UNKNOWN", peer)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base-port", type=int, default=47000)
    parser.add_argument("--guest-tcp-port", type=int, default=47010, help="host port QEMU forwards to the guest's TCP listener")
    parser.add_argument("--guest-udp-port", type=int, default=47011, help="host port QEMU forwards to the guest's UDP socket")
    args = parser.parse_args()
    base = args.base_port

    try:
        threading.Thread(target=udp_echo, args=(bind_udp(base + 1),), daemon=True).start()
        serve_tcp(base + 2, tcp_echo, "tcp echo")
        serve_tcp(base + 3, tcp_sink, "tcp sink")
        serve_tcp(base + 4, tcp_source, "tcp source")
        threading.Thread(target=udp_control, args=(bind_udp(base + 5), args.guest_tcp_port, args.guest_udp_port), daemon=True).start()
    except OSError as e:
        log("could not start: %s" % e)
        return 1

    log("serving on %s ports %d-%d" % (HOST, base + 1, base + 5))
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
