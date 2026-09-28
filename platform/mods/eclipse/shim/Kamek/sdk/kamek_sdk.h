// Port shim for Kamek's hook macros (kmCall and friends): the same registry.
#pragma once

#include <Kuribo/sdk/kuribo_sdk.h>

#define kmCall(addr, ptr)                                                                        \
	static pp::togglable_ppc_bl MACRO_CONCAT(_kmhook, __COUNTER__)((u32)(addr), (ptr), true,     \
	                                                               __FILE__, __LINE__)
#define kmBranch(addr, ptr)                                                                      \
	static pp::togglable_ppc_b MACRO_CONCAT(_kmhook, __COUNTER__)((u32)(addr), (ptr), true,      \
	                                                              __FILE__, __LINE__)
#define kmWrite32(addr, value)                                                                   \
	static pp::word_patch MACRO_CONCAT(_kmhook, __COUNTER__)((u32)(addr), (u32)(value), __FILE__, \
	                                                         __LINE__)
