/* Allow the game's EOF enum to coexist with the host stdio EOF macro. */
#ifndef SMS_PORT_JSU_STREAM_ENUM_HPP
#define SMS_PORT_JSU_STREAM_ENUM_HPP
#pragma push_macro("EOF")
#undef EOF
#include_next <JSystem/JSupport/JSUStreamEnum.hpp>
#pragma pop_macro("EOF")
#endif
