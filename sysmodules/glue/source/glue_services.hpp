#pragma once
#include "glue_inner.hpp"
#include <kumo/ipc/dispatch.hpp>
#include <kumo/prelude.hpp>
namespace kumo::horizon::glue {
using namespace kumo::ipc;
/* Opaque, fixed-layout CMIF records. Glue forwards their bytes unchanged; PSC
 * owns their interpretation. Explicit alignment matches the original time ABI.
 */
template <size_t N, size_t A = 1> struct alignas(A) Wire {
  static constexpr size_t WireBytes = N;
  u8 bytes[N];
};
using Context = Wire<0x20, 8>;
using Point = Wire<0x18, 8>;
using Calendar = Wire<8, 2>;
using CalendarOutput = Wire<0x20, 4>;
using Location = Wire<0x24>;
using Version = Wire<0x10>;
using SnapshotInput = Wire<0x48, 8>;
class GlueObject : public ServiceObject {
protected:
  InnerStatic *inner_;
  InnerKind kind_;
  bool is_static_;
  Handle handles_[3]{INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE};
  GlueObject(InnerStatic *inner, InnerKind kind, bool stat = false)
      : inner_(inner), kind_(kind), is_static_(stat) {}
  ~GlueObject() override {
    for (auto h : handles_)
      if (h != INVALID_HANDLE)
        svcCloseHandle(h);
  }
  Result Get(::Service **s) {
    return is_static_ ? inner_->GetStatic(s) : inner_->GetObject(s, kind_);
  }

public:
  template <u32 Cmd, class I, class O, size_t NI, size_t NO>
  static Result Forward(GlueObject &self, const I &in, O &out,
                        Arguments<NI, NO> &args) {
    ::Service *s;
    KUMO_TRY(self.Get(&s));
    if constexpr (NI == 0 && NO == 0) {
      if constexpr (std::is_same_v<I, NoData> && std::is_same_v<O, NoData>)
        return serviceDispatch(s, Cmd);
      else if constexpr (std::is_same_v<I, NoData>)
        return serviceDispatchOut(s, Cmd, out);
      else if constexpr (std::is_same_v<O, NoData>)
        return serviceDispatchIn(s, Cmd, in);
      else
        return serviceDispatchInOut(s, Cmd, in, out);
    } else if constexpr (Cmd == 400 || Cmd == 401) {
      auto b = args.Out(0);
      return serviceDispatchIn(s, Cmd, in,
                               .buffer_attrs = {SfBufferAttr_HipcPointer |
                                                SfBufferAttr_Out |
                                                SfBufferAttr_FixedSize},
                               .buffers = {{b.GetPointer(), b.GetSize()}});
    } else if constexpr (Cmd == 500 || Cmd == 501) {
      auto a = args.In(0);
      auto b = args.In(1);
      return serviceDispatchOut(
          s, Cmd, out,
          .buffer_attrs = {SfBufferAttr_HipcPointer | SfBufferAttr_In |
                               SfBufferAttr_FixedSize,
                           SfBufferAttr_HipcPointer | SfBufferAttr_In |
                               SfBufferAttr_FixedSize},
          .buffers = {{a.GetPointer(), a.GetSize()},
                      {b.GetPointer(), b.GetSize()}});
    } else if constexpr (Cmd == 201 && NI == 1 && NO == 1) {
      auto a = args.In(0);
      auto b = args.Out(0);
      return serviceDispatchInOut(
          s, Cmd, in, out,
          .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                           SfBufferAttr_HipcPointer | SfBufferAttr_Out},
          .buffers = {{a.GetPointer(), a.GetSize()},
                      {b.GetPointer(), b.GetSize()}});
    } else if constexpr (Cmd == 202) {
      auto b = args.Out(0);
      return serviceDispatchInOut(
          s, Cmd, in, out,
          .buffer_attrs = {SfBufferAttr_HipcPointer | SfBufferAttr_Out},
          .buffers = {{b.GetPointer(), b.GetSize()}});
    } else if constexpr (NI == 1 && NO == 1) {
      auto a = args.In(0);
      auto b = args.Out(0);
      return serviceDispatch(
          s, Cmd,
          .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
                           SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
          .buffers = {{b.GetPointer(), b.GetSize()},
                      {a.GetPointer(), a.GetSize()}});
    } else if constexpr (NI == 1) {
      auto a = args.In(0);
      return serviceDispatchInOut(
          s, Cmd, in, out,
          .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
          .buffers = {{a.GetPointer(), a.GetSize()}});
    } else {
      auto b = args.Out(0);
      if constexpr (std::is_same_v<O, NoData>)
        return serviceDispatchIn(
            s, Cmd, in,
            .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
            .buffers = {{b.GetPointer(), b.GetSize()}});
      else
        return serviceDispatchInOut(
            s, Cmd, in, out,
            .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
            .buffers = {{b.GetPointer(), b.GetSize()}});
    }
  }
  /* Keep the copied handle alive until the service object is destroyed: the
   * server sends borrowed copy handles after Dispatch returns. */
  template <u32 Cmd, size_t Slot>
  static Result HandleCommand(GlueObject &self, const NoData &, NoData &,
                              Arguments<0, 0> &args) {
    ::Service *s;
    KUMO_TRY(self.Get(&s));
    if (self.handles_[Slot] == INVALID_HANDLE)
      KUMO_TRY(serviceDispatch(s, Cmd,
                               .out_handle_attrs = {SfOutHandleAttr_HipcCopy},
                               .out_handles = &self.handles_[Slot]));
    args.ReturnCopy({self.handles_[Slot]});
    return 0;
  }
};
class GlueSystemClock final : public GlueObject {
public:
  GlueSystemClock(InnerStatic *i, InnerKind k) : GlueObject(i, k) {}
  bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
};
class GlueSteadyClock final : public GlueObject {
public:
  explicit GlueSteadyClock(InnerStatic *i)
      : GlueObject(i, InnerKind_SteadyClock) {}
  bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
};
class GlueTimeZoneService final : public GlueObject {
public:
  explicit GlueTimeZoneService(InnerStatic *i)
      : GlueObject(i, InnerKind_TimeZone) {}
  bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
};
class GlueStaticService final : public GlueObject {
public:
  explicit GlueStaticService(InnerStatic *i)
      : GlueObject(i, InnerKind_UserClock, true) {}
  bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
  template <u32 Cmd>
  static Result Child(GlueStaticService &self, const NoData &, NoData &,
                      Arguments<0, 0> &args) {
    auto kind = static_cast<InnerKind>(Cmd);
    ::Service *s;
    KUMO_TRY(self.inner_->GetObject(&s, kind));
    ObjectRef object;
    if constexpr (Cmd == 2)
      object = MakeObject<GlueSteadyClock>(self.inner_);
    else if constexpr (Cmd == 3)
      object = MakeObject<GlueTimeZoneService>(self.inner_);
    else
      object = MakeObject<GlueSystemClock>(self.inner_, kind);
    if (!object)
      return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    args.ReturnObject(std::move(object));
    return 0;
  }
};
} // namespace kumo::horizon::glue
