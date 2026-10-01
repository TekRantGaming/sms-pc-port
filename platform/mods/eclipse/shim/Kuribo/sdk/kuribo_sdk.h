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

// A patch's function as the game calls it. The game calls a mod's function
// through its own type for the call it replaces, and declares many of those
// BOOL (a whole word) or u32 where the mod's function returns bool. On the
// PowerPC a bool fills r3 (0 or 1), so either way the caller reads 0 or 1;
// natively a bool is returned in the low byte alone (setcc %al), the rest of
// the register holding whatever it held, and bool arguments are a byte the
// callee may take to be 0 or 1 and zero-extended. So a function with a bool
// result or argument is registered through a thunk: its result as a whole
// pointer-sized word, 0 or 1, whatever type the game reads it as, and each
// bool argument taken from the low byte the caller passed (read as a word and
// masked: clang would take an unsigned char argument to be zero-extended),
// whether the game passes a bool or a BOOL. Other functions are registered as
// they are.
// L is a lambda that returns the function (each patch has its own).
template <class T> struct __sms_mod_arg { typedef T type; };
template <> struct __sms_mod_arg<bool> { typedef unsigned int type; };
template <class T> static inline T __sms_mod_take(typename __sms_mod_arg<T>::type v) { return v; }
template <> inline bool __sms_mod_take<bool>(unsigned int v) { return (v & 0xFF) != 0; }
template <class T> struct __sms_mod_has_bool { static const bool value = false; };
template <> struct __sms_mod_has_bool<bool> { static const bool value = true; };
template <class... A> struct __sms_mod_any_bool { static const bool value = false; };
template <class A0, class... A> struct __sms_mod_any_bool<A0, A...> {
	static const bool value = __sms_mod_has_bool<A0>::value || __sms_mod_any_bool<A...>::value;
};
template <class R> struct __sms_mod_ret { typedef R type; };
template <> struct __sms_mod_ret<bool> { typedef intptr_t type; };

template <class L, class F> struct __sms_mod_word_abi {
	static F get() { return L{}(); }
};
template <class L, class R, class... A> struct __sms_mod_word_abi<L, R (*)(A...)> {
	typedef typename __sms_mod_ret<R>::type W;
	static W thunk(typename __sms_mod_arg<A>::type... a)
	{
		if constexpr (__sms_mod_has_bool<R>::value)
			return L{}()(__sms_mod_take<A>(a)...) ? 1 : 0;
		else
			return L{}()(__sms_mod_take<A>(a)...);
	}
	static uintptr_t get()
	{
		if constexpr (__sms_mod_has_bool<R>::value || __sms_mod_any_bool<A...>::value)
			return __sms_mod_fnptr(&thunk);
		else
			return __sms_mod_fnptr(L{}());
	}
};
// A member function, which the game calls with its object first.
template <class L, class R, class C, class... A> struct __sms_mod_word_abi<L, R (C::*)(A...)> {
	typedef typename __sms_mod_ret<R>::type W;
	static W thunk(C* self, typename __sms_mod_arg<A>::type... a)
	{
		if constexpr (__sms_mod_has_bool<R>::value)
			return (self->*L{}())(__sms_mod_take<A>(a)...) ? 1 : 0;
		else
			return (self->*L{}())(__sms_mod_take<A>(a)...);
	}
	static uintptr_t get()
	{
		if constexpr (__sms_mod_has_bool<R>::value || __sms_mod_any_bool<A...>::value)
			return __sms_mod_fnptr(&thunk);
		else
			return __sms_mod_fnptr(L{}());
	}
};
template <class L, class R, class C, class... A> struct __sms_mod_word_abi<L, R (C::*)(A...) const> {
	typedef typename __sms_mod_ret<R>::type W;
	static W thunk(const C* self, typename __sms_mod_arg<A>::type... a)
	{
		if constexpr (__sms_mod_has_bool<R>::value)
			return (self->*L{}())(__sms_mod_take<A>(a)...) ? 1 : 0;
		else
			return (self->*L{}())(__sms_mod_take<A>(a)...);
	}
	static uintptr_t get()
	{
		if constexpr (__sms_mod_has_bool<R>::value || __sms_mod_any_bool<A...>::value)
			return __sms_mod_fnptr(&thunk);
		else
			return __sms_mod_fnptr(L{}());
	}
};
template <class L> static inline auto __sms_mod_word_target(L)
{
	return __sms_mod_word_abi<L, decltype(L{}())>::get();
}
#define __SMS_MOD_TARGET(fn) __sms_mod_word_target([] { return (fn); })

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

#define PatchB(a, b)                                                                             \
	togglable_ppc_b static PatchIdentifier((u32)(a), __SMS_MOD_TARGET(b), true, __FILE__, __LINE__)
#define PatchBL(a, b)                                                                            \
	togglable_ppc_bl static PatchIdentifier((u32)(a), __SMS_MOD_TARGET(b), true, __FILE__, __LINE__)
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
	sms_mod_register(SMS_MOD_BRANCH, (u32)(addr), __SMS_MOD_TARGET(value), 1, __FILE__, __LINE__)
#define KURIBO_PATCH_BL(addr, value)                                                             \
	sms_mod_register(SMS_MOD_CALL, (u32)(addr), __SMS_MOD_TARGET(value), 1, __FILE__, __LINE__)
#define KURIBO_PATCH_32(addr, value)                                                             \
	sms_mod_register(SMS_MOD_WORD, (u32)(addr), (uintptr_t)(value), 1, __FILE__, __LINE__)
