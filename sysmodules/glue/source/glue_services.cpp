#include "glue_services.hpp"
namespace kumo::horizon::glue {
bool GlueSystemClock::Dispatch(ConcurrentRequest &request,
                               ConcurrentReply &reply) {
  static constexpr CommandTable table({
      MakeCommand<GlueObject, NoData, s64, 0, 0,
                  &GlueObject::Forward<0, NoData, s64, 0, 0>>(
          0, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, s64, NoData, 0, 0,
                  &GlueObject::Forward<1, s64, NoData, 0, 0>>(
          1, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, Context, 0, 0,
                  &GlueObject::Forward<2, NoData, Context, 0, 0>>(
          2, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, Context, NoData, 0, 0,
                  &GlueObject::Forward<3, Context, NoData, 0, 0>>(
          3, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, NoData, 0, 0,
                  &GlueObject::HandleCommand<4, 0>>(4),
  });
  return table.Dispatch(static_cast<GlueObject *>(this), request, reply);
}
bool GlueSteadyClock::Dispatch(ConcurrentRequest &request,
                               ConcurrentReply &reply) {
  static constexpr CommandTable table({
      MakeCommand<GlueObject, NoData, Point, 0, 0,
                  &GlueObject::Forward<0, NoData, Point, 0, 0>>(
          0, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, s64, 0, 0,
                  &GlueObject::Forward<2, NoData, s64, 0, 0>>(
          2, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, s64, NoData, 0, 0,
                  &GlueObject::Forward<3, s64, NoData, 0, 0>>(
          3, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, s64, 0, 0,
                  &GlueObject::Forward<100, NoData, s64, 0, 0>>(
          100, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, u8, 0, 0,
                  &GlueObject::Forward<101, NoData, u8, 0, 0>>(
          101, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, u32, 0, 0,
                  &GlueObject::Forward<102, NoData, u32, 0, 0>>(
          102, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, s64, 0, 0,
                  &GlueObject::Forward<200, NoData, s64, 0, 0>>(
          200, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, s64, NoData, 0, 0,
                  &GlueObject::Forward<201, s64, NoData, 0, 0>>(
          201, {.buffers = BufferMode::Mapped}),
  });
  return table.Dispatch(static_cast<GlueObject *>(this), request, reply);
}
bool GlueTimeZoneService::Dispatch(ConcurrentRequest &request,
                                   ConcurrentReply &reply) {
  static constexpr CommandTable table({
      MakeCommand<GlueObject, NoData, Location, 0, 0,
                  &GlueObject::Forward<0, NoData, Location, 0, 0>>(
          0, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, Location, NoData, 0, 0,
                  &GlueObject::Forward<1, Location, NoData, 0, 0>>(
          1, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, s32, 0, 0,
                  &GlueObject::Forward<2, NoData, s32, 0, 0>>(
          2, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, s32, s32, 0, 1,
                  &GlueObject::Forward<3, s32, s32, 0, 1>>(
          3, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, Location, NoData, 0, 1,
                  &GlueObject::Forward<4, Location, NoData, 0, 1>>(
          4, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, Version, 0, 0,
                  &GlueObject::Forward<5, NoData, Version, 0, 0>>(
          5, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, NoData, 0, 0,
                  &GlueObject::Forward<6, NoData, NoData, 0, 0>>(
          6, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, NoData, 0, 0,
                  &GlueObject::Forward<7, NoData, NoData, 0, 0>>(
          7, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, NoData, 1, 1,
                  &GlueObject::Forward<8, NoData, NoData, 1, 1>>(
          8, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, NoData, 0, 0,
                  &GlueObject::HandleCommand<20, 0>>(20),
      MakeCommand<GlueObject, s64, CalendarOutput, 1, 0,
                  &GlueObject::Forward<100, s64, CalendarOutput, 1, 0>>(
          100, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, s64, CalendarOutput, 0, 0,
                  &GlueObject::Forward<101, s64, CalendarOutput, 0, 0>>(
          101, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, Calendar, s32, 1, 1,
                  &GlueObject::Forward<201, Calendar, s32, 1, 1>>(
          201, {.buffers = BufferMode::PointerOutput}),
      MakeCommand<GlueObject, Calendar, s32, 0, 1,
                  &GlueObject::Forward<202, Calendar, s32, 0, 1>>(
          202, {.buffers = BufferMode::PointerOutput}),
  });
  return table.Dispatch(static_cast<GlueObject *>(this), request, reply);
}
bool GlueStaticService::Dispatch(ConcurrentRequest &request,
                                 ConcurrentReply &reply) {
  static constexpr CommandTable table({
      MakeCommand<GlueStaticService, NoData, NoData, 0, 0,
                  &GlueStaticService::Child<0>>(0),
      MakeCommand<GlueStaticService, NoData, NoData, 0, 0,
                  &GlueStaticService::Child<1>>(1),
      MakeCommand<GlueStaticService, NoData, NoData, 0, 0,
                  &GlueStaticService::Child<2>>(2),
      MakeCommand<GlueStaticService, NoData, NoData, 0, 0,
                  &GlueStaticService::Child<3>>(3),
      MakeCommand<GlueStaticService, NoData, NoData, 0, 0,
                  &GlueStaticService::Child<4>>(4),
      MakeCommand<GlueStaticService, NoData, NoData, 0, 0,
                  &GlueStaticService::Child<5>>(5),
      MakeCommand<GlueObject, NoData, NoData, 0, 0,
                  &GlueObject::HandleCommand<20, 0>>(20),
      MakeCommand<GlueObject, NoData, NoData, 0, 0,
                  &GlueObject::HandleCommand<30, 1>>(30),
      MakeCommand<GlueObject, NoData, NoData, 0, 0,
                  &GlueObject::HandleCommand<31, 2>>(31),
      MakeCommand<GlueObject, s64, NoData, 0, 0,
                  &GlueObject::Forward<50, s64, NoData, 0, 0>>(
          50, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, s64, 0, 0,
                  &GlueObject::Forward<51, NoData, s64, 0, 0>>(
          51, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, u8, 0, 0,
                  &GlueObject::Forward<100, NoData, u8, 0, 0>>(
          100, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, u8, NoData, 0, 0,
                  &GlueObject::Forward<101, u8, NoData, 0, 0>>(
          101, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, s32, 0, 0,
                  &GlueObject::Forward<102, NoData, s32, 0, 0>>(
          102, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, u8, 0, 0,
                  &GlueObject::Forward<200, NoData, u8, 0, 0>>(
          200, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, Point, 0, 0,
                  &GlueObject::Forward<201, NoData, Point, 0, 0>>(
          201, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, Context, s64, 0, 0,
                  &GlueObject::Forward<300, Context, s64, 0, 0>>(
          300, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, u8, NoData, 0, 1,
                  &GlueObject::Forward<400, u8, NoData, 0, 1>>(
          400, {.buffers = BufferMode::FixedPointer,
                .input_pointer_size = 0,
                .output_pointer_size = 0xD0}),
      MakeCommand<GlueObject, SnapshotInput, NoData, 0, 1,
                  &GlueObject::Forward<401, SnapshotInput, NoData, 0, 1>>(
          401, {.buffers = BufferMode::FixedPointer,
                .input_pointer_size = 0,
                .output_pointer_size = 0xD0}),
      MakeCommand<GlueObject, NoData, s64, 2, 0,
                  &GlueObject::Forward<500, NoData, s64, 2, 0>>(
          500, {.buffers = BufferMode::FixedPointer,
                .input_pointer_size = 0xD0,
                .output_pointer_size = 0}),
      MakeCommand<GlueObject, NoData, s64, 2, 0,
                  &GlueObject::Forward<501, NoData, s64, 2, 0>>(
          501, {.buffers = BufferMode::FixedPointer,
                .input_pointer_size = 0xD0,
                .output_pointer_size = 0}),
      MakeCommand<GlueObject, NoData, NoData, 0, 0,
                  &GlueObject::Forward<600, NoData, NoData, 0, 0>>(
          600, {.buffers = BufferMode::Mapped}),
      MakeCommand<GlueObject, NoData, NoData, 0, 0,
                  &GlueObject::Forward<601, NoData, NoData, 0, 0>>(
          601, {.buffers = BufferMode::Mapped}),
  });
  return table.Dispatch(static_cast<GlueObject *>(this), request, reply);
}
} // namespace kumo::horizon::glue
