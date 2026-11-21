/*
 * Network-device interface: the boundary between the protocol stack and the
 * NIC drivers. Nothing above this interface may know which controller
 * (virtio-net, gVNIC, ENA, ...) carries the frames.
 *
 * Devices are polled: the kernel has no MSI/MSI-X support yet, so the stack
 * calls Poll() from its network thread with the stack lock held.
 */
#pragma once
#include <kumo/prelude.hpp>

namespace kumo::horizon::hznet::netdev {

    constexpr inline size_t MacAddressSize = 6;

    /* Largest Ethernet frame a device carries: 1500 bytes of payload, the 14-byte header, and a VLAN tag. */
    constexpr inline size_t MaxFrameSize = 1518;

    /* Receives the frames a device has taken in. The frame is only valid during the call. */
    class IFrameReceiver {
        public:
            virtual void OnFrameReceived(const void *frame, size_t size) = 0;
    };

    /* One piece of a frame to transmit. A frame is the concatenation of its pieces. */
    struct FramePiece {
        const void *data;
        size_t size;
    };

    /* Total length of a frame given as pieces. */
    inline size_t GetFrameSize(const FramePiece *pieces, size_t piece_count) {
        size_t size = 0;
        for (size_t i = 0; i < piece_count; ++i) {
            size += pieces[i].size;
        }
        return size;
    }

    /* Copy a frame's pieces, in order, to `out`, which holds at least GetFrameSize() bytes. */
    inline void CopyFrame(void *out, const FramePiece *pieces, size_t piece_count) {
        u8 *dst = static_cast<u8 *>(out);
        for (size_t i = 0; i < piece_count; ++i) {
            std::memcpy(dst, pieces[i].data, pieces[i].size);
            dst += pieces[i].size;
        }
    }

    class INetDevice {
        public:
            virtual ~INetDevice() { /* ... */ }

            /* Short human-readable name, e.g. "virtio-net0". */
            virtual const char *GetName() const = 0;

            /* The device's Ethernet address. */
            virtual void GetMacAddress(u8 *out) const = 0;

            /* Largest IP packet the device carries (the Ethernet payload size). */
            virtual u32 GetMtu() const = 0;

            /* Whether the link is up. Devices that cannot tell report true. */
            virtual bool IsLinkUp() = 0;

            /* Transmit one frame of at most MaxFrameSize bytes, without the frame check sequence. */
            /* The pieces need no alignment and are not referenced after the call; a driver that */
            /* does DMA copies them. Fails with ResultOutOfResource when the device's transmit */
            /* queue is full; the stack treats that as a dropped packet. */
            virtual ::Result Send(const FramePiece *pieces, size_t piece_count) = 0;

            /* Hand every frame received since the last call to `receiver`, and reclaim finished */
            /* transmissions. Returns how many frames were delivered. */
            virtual int Poll(IFrameReceiver *receiver) = 0;

            /* Runs on the network thread outside the stack lock. Drivers must serialize any */
            /* maintenance that shares state with Send(). */
            virtual void Maintain() {}
    };

}
