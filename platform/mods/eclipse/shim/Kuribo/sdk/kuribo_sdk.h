// Port shim for the Kuribo SDK: modules are linked into the port, their
// entry points run at boot, and their patches go into the port's registry
// (sms_mod/modhooks.h), keyed by the retail address they would overwrite.
#pragma once

#include <Dolphin/types.h>
#include <stddef.h>
#include <stdint.h>

#include "sms_mod/modhooks.h"

#define CONCAT_IMPL(x, y)  x##y
#define MACRO_CONCAT(x, y) CONCAT_IMPL(x, y)

// Plain functions and member-function pointers alike become an address.
template <typename T> static inline uintptr_t __sms_mod_fnptr(T fn)
{
	union {
		T f;
		uintptr_t p;
	} u;
	u.p = 0;
	u.f = fn;
	return u.p;
}
static inline uintptr_t __sms_mod_fnptr(uintptr_t v) { return v; }
static inline uintptr_t __sms_mod_fnptr(void* v) { return (uintptr_t)v; }

namespace pp {

class auto_patch {
public:
	auto_patch(int kind, u32 addr, uintptr_t val, bool by_default, const char* file, int line)
	    : mVal((u32)val)
	{
		mId = sms_mod_register(kind, addr, val, by_default, file, line);
	}
	auto_patch(u32 addr, u32 val, bool by_default = true)
	    : auto_patch(SMS_MOD_WORD, addr, val, by_default, nullptr, 0)
	{
	}

	bool is_enabled() const { return sms_mod_is_enabled(mId) != 0; }
	void enable() { sms_mod_set_enabled(mId, 1); }
	void disable() { sms_mod_set_enabled(mId, 0); }
	void set_enabled(bool s) { sms_mod_set_enabled(mId, s); }

	// The original word is not known natively.
	u32 overwritten_value() const { return 0; }
	u32 new_value() const { return mVal; }

private:
	int mId;
	u32 mVal;
};

class togglable_ppc_b : public auto_patch {
public:
	template <typename T>
	togglable_ppc_b(u32 addr, T target, bool by_default = true, const char* file = nullptr,
	                int line = 0)
	    : auto_patch(SMS_MOD_BRANCH, addr, __sms_mod_fnptr(target), by_default, file, line)
	{
	}
};
class togglable_ppc_bl : public auto_patch {
public:
	template <typename T>
	togglable_ppc_bl(u32 addr, T target, bool by_default = true, const char* file = nullptr,
	                 int line = 0)
	    : auto_patch(SMS_MOD_CALL, addr, __sms_mod_fnptr(target), by_default, file, line)
	{
	}
};
class word_patch : public auto_patch {
public:
	word_patch(u32 addr, u32 value, const char* file, int line)
	    : auto_patch(SMS_MOD_WORD, addr, value, true, file, line)
	{
	}
};

template <typename T, bool enabled> struct scoped_guard {
	scoped_guard(T& toggle) : mToggle(toggle), mSave(toggle.is_enabled())
	{
		mToggle.set_enabled(enabled);
	}
	~scoped_guard() { mToggle.set_enabled(mSave); }

	T& mToggle;
	bool mSave;
};

#define PatchIdentifier MACRO_CONCAT(_patch, __COUNTER__)

#define PatchB(a, b)  togglable_ppc_b static PatchIdentifier((u32)(a), (b), true, __FILE__, __LINE__)
#define PatchBL(a, b) togglable_ppc_bl static PatchIdentifier((u32)(a), (b), true, __FILE__, __LINE__)
#define Patch32(a, b) word_patch static PatchIdentifier((u32)(a), (u32)(b), __FILE__, __LINE__)

inline void* Import(const char* name) { return sms_mod_import(name); }

} // namespace pp

// A module's body runs once, at boot, with __kuribo_attach set.
#define KURIBO_MODULE_BEGIN(name, author, version)                                              \
	static int __sms_mod_entry(int __kuribo_attach_arg);                                         \
	static struct __sms_mod_registrar {                                                          \
		__sms_mod_registrar() { sms_mod_add_module(name, &__sms_mod_entry); }                    \
	} __sms_mod_registrar_instance;                                                              \
	static int __sms_mod_entry(int __kuribo_attach_arg)                                          \
	{                                                                                            \
		const int __kuribo_attach = __kuribo_attach_arg;                                         \
		const int __kuribo_detach = !__kuribo_attach_arg;                                        \
		(void)__kuribo_detach;

#define KURIBO_MODULE_END()                                                                      \
	return 0;                                                                                    \
	}

#define KURIBO_EXECUTE_ON_LOAD   if (__kuribo_attach)
#define KURIBO_EXECUTE_ON_UNLOAD if (__kuribo_detach)
#define KURIBO_EXECUTE_ALWAYS

#define KURIBO_EXPORT_AS(function, name)                                                         \
	if (__kuribo_attach)                                                                         \
	sms_mod_export(name, (void*)__sms_mod_fnptr(&function))
#define KURIBO_EXPORT(function)          KURIBO_EXPORT_AS(function, #function)
#define KURIBO_GET_PROCEDURE(function)   sms_mod_import(function)

// Inside a module body: patches applied when the module attaches.
#define KURIBO_PATCH_B(addr, value)                                                              \
	sms_mod_register(SMS_MOD_BRANCH, (u32)(addr), __sms_mod_fnptr(value), 1, __FILE__, __LINE__)
#define KURIBO_PATCH_BL(addr, value)                                                             \
	sms_mod_register(SMS_MOD_CALL, (u32)(addr), __sms_mod_fnptr(value), 1, __FILE__, __LINE__)
#define KURIBO_PATCH_32(addr, value)                                                             \
	sms_mod_register(SMS_MOD_WORD, (u32)(addr), (uintptr_t)(value), 1, __FILE__, __LINE__)
