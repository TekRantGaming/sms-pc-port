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
// through its own type for the call it replaces, which need not be the mod
// function's: BOOL (a whole word) or u32 where the mod's function returns
// bool, BOOL where it returns u8 (BetterSunshineEngine's
// patchYStorageWalkEnd in place of TMario::walkEnd). On the PowerPC the
// callee extends a bool, u8, s8, u16 or s16 result to the whole of r3 (0 or 1,
// clrlwi, extsb, extsh), so the caller reads the same value whatever type it
// reads it as; natively such a result is the low byte or half alone (setcc
// %al), the rest of the register holding whatever it held, and bool arguments
// are a byte the callee may take to be 0 or 1 and zero-extended. So a function
// with a bool or narrow integer result, or a bool argument, is registered
// through a thunk: its result as a whole pointer-sized word, 0 or 1 for a
// bool, zero- or sign-extended as the mod declares it for the others, whatever
// type the game reads it as; and each bool argument taken from the low byte
// the caller passed (read as a word and masked: clang would take an unsigned
// char argument to be zero-extended), whether the game passes a bool or a
// BOOL. Other functions are registered as they are.
// L is a lambda that returns the function (each patch has its own). get() is
// kept out of line so that each patch target's type is in the debug info,
// where tools/mods/abi_check.py compares it with the port's hook.
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
// An integer or enum narrower than a register (not bool), and the word the
// PowerPC callee extends it to.
template <class R, bool = __is_enum(R)> struct __sms_mod_int { typedef R type; };
template <class R> struct __sms_mod_int<R, true> { typedef __underlying_type(R) type; };
template <class I, bool = __is_integral(I) && !__is_same(I, bool)> struct __sms_mod_small {
	static const bool value = false;
};
template <class I> struct __sms_mod_small<I, true> { static const bool value = sizeof(I) < sizeof(int); };
template <class R> struct __sms_mod_narrow {
	static const bool value = __sms_mod_small<typename __sms_mod_int<R>::type>::value;
};
template <class R, bool = __sms_mod_narrow<R>::value> struct __sms_mod_ret { typedef R type; };
template <bool S> struct __sms_mod_ext { typedef uintptr_t type; };
template <> struct __sms_mod_ext<true> { typedef intptr_t type; };
template <class R> struct __sms_mod_ret<R, true> {
	typedef typename __sms_mod_ext<__is_signed(typename __sms_mod_int<R>::type)>::type type;
};
template <> struct __sms_mod_ret<bool, false> { typedef intptr_t type; };
template <class R> struct __sms_mod_wide {
	static const bool value = __sms_mod_has_bool<R>::value || __sms_mod_narrow<R>::value;
};
// The result as the PowerPC callee leaves it in r3.
template <class R, class V> static constexpr typename __sms_mod_ret<R>::type __sms_mod_give(V v)
{
	if constexpr (__sms_mod_has_bool<R>::value)
		return v ? 1 : 0;
	else if constexpr (__sms_mod_narrow<R>::value)
		return (typename __sms_mod_ret<R>::type)(typename __sms_mod_int<R>::type)v;
	else
		return v;
}
static_assert(__sms_mod_give<unsigned char>(0x1FF) == 0xFF && __sms_mod_give<signed char>(0x1FF) == -1 &&
              __sms_mod_give<short>(0x18000) == -0x8000 && __sms_mod_give<unsigned short>(-1) == 0xFFFF &&
              __sms_mod_give<bool>(2) == 1 && __is_same(__sms_mod_ret<int>::type, int),
              "narrow results are extended as the PowerPC callee extends them");

template <class L, class F> struct __sms_mod_word_abi {
	__attribute__((noinline, used)) static F get() { return L{}(); }
};
template <class L, class R, class... A> struct __sms_mod_word_abi<L, R (*)(A...)> {
	typedef typename __sms_mod_ret<R>::type W;
	static W thunk(typename __sms_mod_arg<A>::type... a)
	{
		if constexpr (__sms_mod_wide<R>::value)
			return __sms_mod_give<R>(L{}()(__sms_mod_take<A>(a)...));
		else
			return L{}()(__sms_mod_take<A>(a)...);
	}
	__attribute__((noinline, used)) static uintptr_t get()
	{
		if constexpr (__sms_mod_wide<R>::value || __sms_mod_any_bool<A...>::value)
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
		if constexpr (__sms_mod_wide<R>::value)
			return __sms_mod_give<R>((self->*L{}())(__sms_mod_take<A>(a)...));
		else
			return (self->*L{}())(__sms_mod_take<A>(a)...);
	}
	__attribute__((noinline, used)) static uintptr_t get()
	{
		if constexpr (__sms_mod_wide<R>::value || __sms_mod_any_bool<A...>::value)
			return __sms_mod_fnptr(&thunk);
		else
			return __sms_mod_fnptr(L{}());
	}
};
template <class L, class R, class C, class... A> struct __sms_mod_word_abi<L, R (C::*)(A...) const> {
	typedef typename __sms_mod_ret<R>::type W;
	static W thunk(const C* self, typename __sms_mod_arg<A>::type... a)
	{
		if constexpr (__sms_mod_wide<R>::value)
			return __sms_mod_give<R>((self->*L{}())(__sms_mod_take<A>(a)...));
		else
			return (self->*L{}())(__sms_mod_take<A>(a)...);
	}
	__attribute__((noinline, used)) static uintptr_t get()
	{
		if constexpr (__sms_mod_wide<R>::value || __sms_mod_any_bool<A...>::value)
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
