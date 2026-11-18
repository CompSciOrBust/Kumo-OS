/*
 * nifm's result codes: module 110, which the Switch shows as "2110-<description>".
 * Only the ones this service returns are defined. The values are from
 * https://switchbrew.org/wiki/Error_codes.
 */
#pragma once
#include <switch.h>

namespace kumo::horizon::nifm {

    constexpr ::Result ResultInvalidPermission()    { return MAKERESULT(110, 120); }
    constexpr ::Result ResultWrongInput()           { return MAKERESULT(110, 200); }
    constexpr ::Result ResultNotSubmitted()         { return MAKERESULT(110, 311); }
    constexpr ::Result ResultNetworkNotAvailable()  { return MAKERESULT(110, 1100); }
    constexpr ::Result ResultConnectionReset()      { return MAKERESULT(110, 2003); }

    static_assert(ResultInvalidPermission() == 0xF06E && ResultWrongInput() == 0x1906E);
    static_assert(ResultNetworkNotAvailable() == 0x8986E && ResultConnectionReset() == 0xFA66E);

}
