#pragma once
#include <kumo/ipc/message.hpp>
#include <kumo/ipc/arguments.hpp>
#include <array>
#include <kumo/ipc/wire.hpp>

namespace kumo::ipc {
// Empty wire input/output occupies no bytes (unlike sizeof an empty C++ struct).
struct NoData {};
template<class T> inline constexpr size_t WireSize = std::is_same_v<T, NoData> ? 0 : sizeof(T);
// PointerOutput takes mapped inputs and one variable-size pointer output.
// FixedPointerInput takes fixed-size static inputs and mapped outputs.
// FixedPointerAndNonSecureMapped takes one fixed-size static input followed by
// non-secure mapped inputs. Its outputs are non-secure mapped.
enum class BufferMode { Auto, Mapped, FixedPointer, PointerOutput, NonSecureMapped, FixedPointerInput, FixedPointerAndNonSecureMapped };
struct CommandOptions {
    bool pid = false;
    size_t copies = 0, moves = 0;
    BufferMode buffers = BufferMode::Auto;
    size_t input_pointer_size = 0, output_pointer_size = 0;
    bool normal_map_alias = false;
};
template<size_t Inputs, size_t Outputs> struct Arguments {
    std::array<MappedBuffer, Inputs> inputs{};
    std::array<MappedBuffer, Outputs> outputs{};
    ConcurrentRequest &request;
    ConcurrentReply &reply;
    InBuffer In(size_t index) const { return {inputs.at(index).address, inputs.at(index).size}; }
    OutBuffer Out(size_t index) const { return {outputs.at(index).address, outputs.at(index).size}; }
    ClientProcessId Pid() const { return {request.input.hipc.pid}; }
    CopyHandle TakeCopy(size_t index) { return CopyHandle(index < request.copy_count ? request.TakeHandle(index) : INVALID_HANDLE); }
    MoveHandle TakeMove(size_t index) { return MoveHandle(index < request.move_count ? request.TakeHandle(request.copy_count + index) : INVALID_HANDLE); }
    void ReturnCopy(BorrowedHandle handle, size_t index = 0) {
        if (index == 0) reply.copy_handle = handle.value;
        else if (index == 1) reply.second_copy_handle = handle.value;
        else __builtin_trap();
    }
    void ReturnObject(ObjectRef object) { reply.object = std::move(object); }
    void ReturnMove(MoveHandle handle) {
        if (reply.move_handle != INVALID_HANDLE) svcCloseHandle(reply.move_handle);
        reply.move_handle = handle.Release();
    }
};
class TypedCommand;
template<class Service, class Input, class Output, size_t Inputs, size_t Outputs, auto Handler>
consteval TypedCommand MakeCommand(u32 id, CommandOptions options = {});
class TypedCommand {
    template<size_t> friend class CommandTable;
    template<class S, class I, class O, size_t NI, size_t NO, auto H>
    friend consteval TypedCommand MakeCommand(u32, CommandOptions);
    constexpr TypedCommand(u32 command_id, CommandOptions command_options,
        bool (*handler)(void *, ConcurrentRequest &, ConcurrentReply &, CommandOptions))
        : id(command_id), options(command_options), invoke(handler) {}
    u32 id;
    CommandOptions options;
    bool (*invoke)(void *, ConcurrentRequest &, ConcurrentReply &, CommandOptions);
};
// Handler receives validated descriptors and aligned, copied input. Serialization
// and pointer-size-table offsets are derived from the same wire types.
template<class Service, class Input, class Output, size_t Inputs, size_t Outputs, auto Handler>
consteval TypedCommand MakeCommand(u32 id, CommandOptions options) {
    static_assert(std::is_trivially_copyable_v<Input> && std::is_trivially_copyable_v<Output>);
    static_assert(WireSize<Output> <= 128);
    static_assert(Inputs <= 15 && Outputs <= 13);
    static_assert(std::is_same_v<Input, NoData> || IsWireType<Input>, "Input must declare an audited wire layout");
    static_assert(std::is_same_v<Output, NoData> || IsWireType<Output>, "Output must declare an audited wire layout");
    if (options.copies > 15 || options.moves > 15) __builtin_abort();
    size_t descriptor_bytes = 0, pointer_sizes = 0;
    switch (options.buffers) {
    case BufferMode::Auto:
        descriptor_bytes = Inputs * 20 + Outputs * 20;
        pointer_sizes = Outputs;
        break;
    case BufferMode::Mapped:
    case BufferMode::NonSecureMapped:
        descriptor_bytes = (Inputs + Outputs) * sizeof(HipcBufferDescriptor);
        break;
    case BufferMode::FixedPointer:
        if (Outputs > 1 || bool(Inputs) != bool(options.input_pointer_size) ||
            bool(Outputs) != bool(options.output_pointer_size) ||
            options.input_pointer_size > 0x800 || options.output_pointer_size > 0x800) __builtin_abort();
        descriptor_bytes = Inputs * sizeof(HipcStaticDescriptor) + Outputs * sizeof(HipcRecvListEntry);
        break;
    case BufferMode::FixedPointerInput:
        if (!Inputs || !Outputs || !options.input_pointer_size ||
            options.input_pointer_size > 0x800 || options.output_pointer_size) __builtin_abort();
        descriptor_bytes = Inputs * sizeof(HipcStaticDescriptor) + Outputs * sizeof(HipcBufferDescriptor);
        break;
    case BufferMode::FixedPointerAndNonSecureMapped:
        if (!Inputs || (Inputs == 1 && !Outputs) || !options.input_pointer_size ||
            options.input_pointer_size > 0x800 || options.output_pointer_size) __builtin_abort();
        descriptor_bytes = sizeof(HipcStaticDescriptor) + (Inputs - 1 + Outputs) * sizeof(HipcBufferDescriptor);
        break;
    case BufferMode::PointerOutput:
        if (Outputs != 1 || options.input_pointer_size || options.output_pointer_size) __builtin_abort();
        descriptor_bytes = Inputs * sizeof(HipcBufferDescriptor) + sizeof(HipcRecvListEntry);
        pointer_sizes = 1;
        break;
    default:
        __builtin_abort();
    }
    const size_t special_bytes = (options.pid || options.copies || options.moves) ? 4 + (options.pid ? 8 : 0) + 4 * (options.copies + options.moves) : 0;
    const size_t raw_bytes = OutPointerSizesOffset(WireSize<Input>, false) + pointer_sizes * sizeof(u16);
    if (sizeof(HipcHeader) + special_bytes + descriptor_bytes + ((raw_bytes + 3) & ~size_t(3)) > 256) __builtin_abort();
    return {id, options, [](void *object, ConcurrentRequest &r, ConcurrentReply &reply, CommandOptions o) {
        const auto &m = r.input.hipc.meta;
        if (bool(m.send_pid) != o.pid || r.copy_count != o.copies || r.move_count != o.moves || r.input.data_size < WireSize<Input>) return false;
        Arguments<Inputs, Outputs> args{{}, {}, r, reply};
        const auto &descriptors = r.input.hipc.data;
        const bool mixed = o.buffers == BufferMode::FixedPointerAndNonSecureMapped;
        const bool non_secure = mixed || o.buffers == BufferMode::NonSecureMapped;
        const bool fixed = o.buffers == BufferMode::FixedPointer;
        const size_t pointer_inputs = fixed || o.buffers == BufferMode::FixedPointerInput ? Inputs : size_t(mixed);
        const bool variable = o.buffers == BufferMode::PointerOutput;
        uintptr_t input_end = reinterpret_cast<uintptr_t>(r.pointer_buffer);
        size_t output_pointer_size = o.output_pointer_size;
        if (o.buffers == BufferMode::Auto) {
            if (!DecodeMappedAutoBuffers(r.input.hipc, args.inputs.data(), Inputs, args.outputs.data(), Outputs) ||
                !HasOnlyMappedAutoOutputs(r.input, WireSize<Input>, Outputs)) return false;
        } else {
            if (m.num_exch_buffers || m.num_send_statics != pointer_inputs ||
                m.num_send_buffers != Inputs - pointer_inputs ||
                m.num_recv_buffers != (fixed || variable ? 0 : Outputs) ||
                m.num_recv_statics != (fixed || variable ? Outputs : 0)) return false;

            // HIPC rewrites input statics into consecutive, aligned regions
            // of the receive pool, preserving their descriptor indices.
            if (pointer_inputs) {
                const uintptr_t end = input_end + r.pointer_buffer_size;
                if (!r.pointer_buffer || end < input_end) return false;
                for (size_t i = 0; i < pointer_inputs; ++i) {
                    const auto &d = descriptors.send_statics[i];
                    const uintptr_t address = reinterpret_cast<uintptr_t>(hipcGetStaticAddress(&d));
                    const size_t size = hipcGetStaticSize(&d);
                    if (d.index != i || size != o.input_pointer_size ||
                        address < input_end || address > end || size > end - address) return false;
                    args.inputs[i] = {reinterpret_cast<void *>(address), size};
                    input_end = address + size;
                }
            }
            for (size_t i = pointer_inputs; i < Inputs; ++i) {
                const auto &d = descriptors.send_buffers[i - pointer_inputs];
                if ((non_secure && d.mode != HipcBufferMode_NonSecure) ||
                    (o.normal_map_alias && d.mode != HipcBufferMode_Normal)) return false;
                args.inputs[i] = {hipcGetBufferAddress(&d), hipcGetBufferSize(&d)};
            }

            if (variable) {
                u16 size;
                if ((r.input.domain && r.input.data_size != WireSize<Input>) ||
                    !ReadOutPointerSize(r.input, WireSize<Input>, 0, size)) return false;
                output_pointer_size = size;
            }
            if constexpr (Outputs) {
                if (fixed || variable) {
                    const uintptr_t begin = reinterpret_cast<uintptr_t>(r.pointer_buffer);
                    const uintptr_t end = begin + r.pointer_buffer_size;
                    if (!r.pointer_buffer || end < begin || output_pointer_size > r.pointer_buffer_size) return false;
                    const uintptr_t address = (end - output_pointer_size) & ~uintptr_t(15);
                    if (address < input_end) return false;
                    args.outputs[0] = {reinterpret_cast<void *>(address), output_pointer_size};
                } else {
                    for (size_t i = 0; i < Outputs; ++i) {
                        const auto &d = descriptors.recv_buffers[i];
                        if ((non_secure && d.mode != HipcBufferMode_NonSecure) ||
                            (o.normal_map_alias && d.mode != HipcBufferMode_Normal)) return false;
                        args.outputs[i] = {hipcGetBufferAddress(&d), hipcGetBufferSize(&d)};
                    }
                }
            }
        }
        Input input{};
        if constexpr (WireSize<Input> != 0) std::memcpy(&input, r.data, sizeof(input));
        if constexpr (requires { input.IsValidWire(); }) if (!input.IsValidWire()) return false;
        if constexpr (Outputs) {
            if ((fixed || variable) && output_pointer_size)
                std::memset(args.outputs[0].address, 0, output_pointer_size);
        }
        Output output{};
        reply.result = Handler(*static_cast<Service *>(object), input, output, args);
        if (R_SUCCEEDED(reply.result)) {
            if constexpr (Outputs) {
                if (fixed || variable) reply.out_pointer = args.outputs[0];
            }
            if constexpr (WireSize<Output> != 0) reply.Write(output);
        }
        return true;
    }};
}
template<size_t N> class CommandTable {
    std::array<TypedCommand, N> commands_;
public:
    consteval CommandTable(const TypedCommand (&commands)[N]) : commands_(std::to_array(commands)) {
        for (size_t i = 0; i < N; ++i) {
            if (!commands[i].invoke) __builtin_abort();
            for (size_t j = 0; j < i; ++j) if (commands[i].id == commands[j].id) __builtin_abort();
        }
    }
    bool Dispatch(void *object, ConcurrentRequest &request, ConcurrentReply &reply) const {
        for (const auto &command : commands_) if (command.id == request.input.header.command_id)
            return command.invoke(object, request, reply, command.options);
        reply.result = MAKERESULT(10, 221);
        return true;
    }
};
template<size_t N> CommandTable(const TypedCommand (&)[N]) -> CommandTable<N>;
template<size_t N> bool Dispatch(void *object, const CommandTable<N> &commands, ConcurrentRequest &request, ConcurrentReply &reply) {
    return commands.Dispatch(object, request, reply);
}
}
