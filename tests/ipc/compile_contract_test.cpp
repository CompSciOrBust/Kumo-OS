#include <kumo/ipc/dispatch.hpp>
using namespace kumo::ipc;
constexpr auto handler=[](int &,const NoData &,NoData &,Arguments<0,0>&)->Result{return 0;};
#if defined(TEST_DUPLICATE)
constexpr CommandTable table({MakeCommand<int,NoData,NoData,0,0,handler>(1),MakeCommand<int,NoData,NoData,0,0,handler>(1)});
#elif defined(TEST_HANDLES)
constexpr auto invalid=MakeCommand<int,NoData,NoData,0,0,handler>(1,{false,16});
#elif defined(TEST_CAPACITY)
constexpr auto invalid=MakeCommand<int,NoData,NoData,15,13,
    [](int &,const NoData &,NoData &,Arguments<15,13>&)->Result{return 0;}>(1);
#elif defined(TEST_WIRE)
struct Unsafe { void *address; };
constexpr auto invalid=MakeCommand<int,Unsafe,NoData,0,0,
    [](int &,const Unsafe &,NoData &,Arguments<0,0>&)->Result{return 0;}>(1);
#else
constexpr CommandTable table({MakeCommand<int,NoData,NoData,0,0,handler>(1)});
#endif
