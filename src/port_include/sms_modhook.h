/* Hook points for code mods (Super Mario Eclipse, BetterSunshineEngine) in
 * the decomp source, used by the modhook-* port patches.
 *
 * A mod patches the retail game at a fixed address; each hook names that
 * address and asks the registry (platform/mods/modhooks.cpp) whether a mod
 * replaced what the game does there. With nothing registered (every build
 * without a code mod) the original runs. The answer is cached per site and
 * looked up again only when the registry changes.
 *
 *   SMS_MOD_CALL(0x80014F9C, void (*)(void*), JAIGlobalParameter::setParamInitDataPointer(p), p);
 *
 * calls the mod's function with the listed arguments (a member function's
 * object first, as the PowerPC passes it in r3) in place of the original
 * call, which runs otherwise. SMS_MOD_CALL_R is the same for a call whose
 * value is used. SMS_MOD_WORD(addr, original) gives the 32-bit word a mod
 * wrote at addr, or the original. */
#ifndef SMS_MODHOOK_H
#define SMS_MODHOOK_H

#ifdef TARGET_PC
extern "C" void* sms_mod_target(unsigned int addr);
extern "C" int sms_mod_word(unsigned int addr, unsigned int* value);
extern "C" unsigned int sms_mod_generation;
extern "C" uintptr_t sms_mod_gpr[32];

// What retail register r`reg` holds at a hooked call, for a mod function
// that reads it (BetterSunshineEngine's SMS_FROM_GPR).
#define SMS_MOD_GPR(reg, value) (sms_mod_gpr[reg] = (uintptr_t)(value))

static inline void* sms_mod_site_(unsigned int addr, void** cache, unsigned int* gen)
{
	if (*gen != sms_mod_generation) {
		*cache = sms_mod_target(addr);
		*gen   = sms_mod_generation;
	}
	return *cache;
}
#define SMS_MOD_SITE(addr)                                                                         \
	({                                                                                             \
		static void* sms_mod_cache_;                                                               \
		static unsigned int sms_mod_gen_ = ~0u;                                                    \
		sms_mod_site_((addr), &sms_mod_cache_, &sms_mod_gen_);                                     \
	})


// The free-function type a mod's replacement for a member function has (the
// object first). Used through __typeof__ only; never called.
template <class T> struct sms_mod_type_ { typedef T type; };
template <class R, class C> R (*sms_mod_as_free(R (C::*)()))(C*) { return 0; }
template <class R, class C> R (*sms_mod_as_free(R (C::*)() const))(C*) { return 0; }
template <class R, class C, class A0> R (*sms_mod_as_free(R (C::*)(A0)))(C*, A0) { return 0; }
template <class R, class C, class A0> R (*sms_mod_as_free(R (C::*)(A0) const))(C*, A0) { return 0; }
template <class R, class C, class A0, class A1> R (*sms_mod_as_free(R (C::*)(A0, A1)))(C*, A0, A1) { return 0; }
template <class R, class C, class A0, class A1> R (*sms_mod_as_free(R (C::*)(A0, A1) const))(C*, A0, A1) { return 0; }
template <class R, class C, class A0, class A1, class A2> R (*sms_mod_as_free(R (C::*)(A0, A1, A2)))(C*, A0, A1, A2) { return 0; }
template <class R, class C, class A0, class A1, class A2> R (*sms_mod_as_free(R (C::*)(A0, A1, A2) const))(C*, A0, A1, A2) { return 0; }
template <class R, class C, class A0, class A1, class A2, class A3> R (*sms_mod_as_free(R (C::*)(A0, A1, A2, A3)))(C*, A0, A1, A2, A3) { return 0; }
template <class R, class C, class A0, class A1, class A2, class A3> R (*sms_mod_as_free(R (C::*)(A0, A1, A2, A3) const))(C*, A0, A1, A2, A3) { return 0; }
template <class R, class C, class A0, class A1, class A2, class A3, class A4> R (*sms_mod_as_free(R (C::*)(A0, A1, A2, A3, A4)))(C*, A0, A1, A2, A3, A4) { return 0; }
template <class R, class C, class A0, class A1, class A2, class A3, class A4> R (*sms_mod_as_free(R (C::*)(A0, A1, A2, A3, A4) const))(C*, A0, A1, A2, A3, A4) { return 0; }
template <class R, class C, class A0, class A1, class A2, class A3, class A4, class A5> R (*sms_mod_as_free(R (C::*)(A0, A1, A2, A3, A4, A5)))(C*, A0, A1, A2, A3, A4, A5) { return 0; }
template <class R, class C, class A0, class A1, class A2, class A3, class A4, class A5> R (*sms_mod_as_free(R (C::*)(A0, A1, A2, A3, A4, A5) const))(C*, A0, A1, A2, A3, A4, A5) { return 0; }
template <class R, class C, class A0, class A1, class A2, class A3, class A4, class A5, class A6> R (*sms_mod_as_free(R (C::*)(A0, A1, A2, A3, A4, A5, A6)))(C*, A0, A1, A2, A3, A4, A5, A6) { return 0; }
template <class R, class C, class A0, class A1, class A2, class A3, class A4, class A5, class A6> R (*sms_mod_as_free(R (C::*)(A0, A1, A2, A3, A4, A5, A6) const))(C*, A0, A1, A2, A3, A4, A5, A6) { return 0; }
template <class R, class C, class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7> R (*sms_mod_as_free(R (C::*)(A0, A1, A2, A3, A4, A5, A6, A7)))(C*, A0, A1, A2, A3, A4, A5, A6, A7) { return 0; }
template <class R, class C, class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7> R (*sms_mod_as_free(R (C::*)(A0, A1, A2, A3, A4, A5, A6, A7) const))(C*, A0, A1, A2, A3, A4, A5, A6, A7) { return 0; }

// A replaced call to a free or static function `fn`; arguments as in the call.
#define SMS_MOD_CALLF(addr, fn, original, ...)                                                     \
	SMS_MOD_CALL(addr, __typeof__(&fn), original, __VA_ARGS__)
#define SMS_MOD_CALLF_R(addr, fn, original, ...)                                                   \
	SMS_MOD_CALL_R(addr, __typeof__(&fn), original, __VA_ARGS__)
// A replaced call to member function Class::method on `obj` (a pointer).
#define SMS_MOD_CALLM(addr, Class, method, original, obj, ...)                                     \
	SMS_MOD_CALL(addr, __typeof__(sms_mod_as_free(&Class::method)), original, obj, ##__VA_ARGS__)
#define SMS_MOD_CALLM_R(addr, Class, method, original, obj, ...)                                   \
	SMS_MOD_CALL_R(addr, __typeof__(sms_mod_as_free(&Class::method)), original, obj, ##__VA_ARGS__)

#define SMS_MOD_CALL(addr, fnType, original, ...)                                                  \
	do {                                                                                           \
		void* sms_mod_t_ = SMS_MOD_SITE(addr);                                                     \
		if (sms_mod_t_)                                                                            \
			((fnType)sms_mod_t_)(__VA_ARGS__);                                                     \
		else                                                                                       \
			original;                                                                              \
	} while (0)
#define SMS_MOD_CALL_R(addr, fnType, original, ...)                                                \
	({                                                                                             \
		void* sms_mod_t_ = SMS_MOD_SITE(addr);                                                     \
		sms_mod_t_ ? ((fnType)sms_mod_t_)(__VA_ARGS__) : (original);                               \
	})
#define SMS_MOD_WORD(addr, original)                                                               \
	({                                                                                             \
		unsigned int sms_mod_w_;                                                                   \
		sms_mod_word((addr), &sms_mod_w_) ? sms_mod_w_ : (unsigned int)(original);                 \
	})
// A table a mod moved by rewriting the lis/addi pair that loads its address:
// the halves it wrote at hiAddr and loAddr (the instructions' immediates),
// else the original table.
#define SMS_MOD_HILO(hiAddr, loAddr, original)                                                     \
	({                                                                                             \
		unsigned int sms_mod_hi_, sms_mod_lo_;                                                     \
		(sms_mod_word((hiAddr), &sms_mod_hi_) && sms_mod_word((loAddr), &sms_mod_lo_))             \
		    ? (__typeof__(&(original)[0]))(uintptr_t)((sms_mod_hi_ << 16) + (int)(short)sms_mod_lo_) \
		    : &(original)[0];                                                                      \
	})
#endif

#endif
