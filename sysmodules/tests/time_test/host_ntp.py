#!/usr/bin/env python3
"""A fake NTP server for scripts/run-time-test.sh NTP=fake.

Answers every client request with the host's own clock. QEMU's user-mode
network maps 10.0.2.2 to the host, so the guest reaches this as 10.0.2.2:47123;
psc must have been built to ask there:

    make -C sysmodules/psc clean
    make -C sysmodules/psc nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1 KUMO_NTP_SERVERS="10.0.2.2:47123"

Usage: host_ntp.py [port]    (default 47123, bound to 127.0.0.1)
"""
import socket
import struct
import sys
import time

NTP_UNIX_DELTA = 2208988800


def timestamp(t):
    seconds = int(t)
    fraction = int((t - seconds) * (1 << 32))
    return struct.pack("!II", seconds + NTP_UNIX_DELTA, fraction)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 47123
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", port))
    print(f"[host_ntp] listening on 127.0.0.1:{port}", flush=True)

    while True:
        request, peer = sock.recvfrom(512)
        received = time.time()
        if len(request) < 48:
            continue
        version = (request[0] >> 3) & 7
        # LI 0, the client's version, mode 4 (server); stratum 2; poll; precision 2^-20 s.
        header = bytes([(version << 3) | 4, 2, request[2], 0xEC])
        reply = (
            header
            + struct.pack("!II", 0, 0)                  # root delay, root dispersion
            + b"LOCL"                                   # reference ID
            + timestamp(received)                       # reference timestamp
            + request[40:48]                            # originate: the client's transmit timestamp
            + timestamp(received)                       # receive
            + timestamp(time.time())                    # transmit
        )
        sock.sendto(reply, peer)
        print(f"[host_ntp] answered {peer[0]}:{peer[1]} with {time.time():.3f}", flush=True)


if __name__ == "__main__":
    main()
