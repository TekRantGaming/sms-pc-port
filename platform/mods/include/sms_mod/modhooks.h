/* Code mods (Super Mario Eclipse and BetterSunshineEngine): the registry
 * their patches go into.
 *
 * On the GameCube those mods overwrite instructions of the retail game at
 * fixed addresses: a call redirected to their function, a branch, or a word
 * replaced. The port has no retail machine code, so each patch is recorded
 * here under its retail address instead, and the decomp source asks for it
 * at the matching place (a port patch naming the same address): the call
 * goes to the mod's function when one is registered and enabled, and to the
 * original otherwise. With no code mod linked in, nothing is registered and
 * every lookup answers "none".
 */
#ifndef SMS_MOD_MODHOOKS_H
#define SMS_MOD_MODHOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	SMS_MOD_BRANCH = 0, /* b target: the function is replaced from here */
	SMS_MOD_CALL   = 1, /* bl target: this call goes to the mod */
	SMS_MOD_WORD   = 2, /* a 32-bit word (an instruction or a constant) */
};

/* Record a patch; returns its id. `enabled` is its initial state. */
int sms_mod_register(int kind, uint32_t addr, uintptr_t value, int enabled,
                     const char* file, int line);
void sms_mod_set_enabled(int id, int on);
int sms_mod_is_enabled(int id);

/* The enabled branch or call target registered at `addr`, else 0. */
void* sms_mod_target(uint32_t addr);
/* The enabled word registered at `addr`: 1 and *value, else 0. */
int sms_mod_word(uint32_t addr, uint32_t* value);

/* A mod rewriting the retail game's code at run time (BetterSunshineEngine's
 * PowerPC::writeU8/U16/U32): `size` bytes of `value` at the retail address
 * `addr`, recorded as a word patch there, which hooks read back with
 * sms_mod_word. Nothing is written: the address is not code in the port. */
void sms_mod_code_write(uint32_t addr, uint32_t value, int size);
/* Game data a mod rewrites in place (SMS_WRITE_32 on a variable, not code):
 * the port's variable for the retail address `addr`. When the mods start, a
 * word registered there is stored into it (`size` 4). */
void sms_mod_bind_data(uint32_t addr, void* var, int size);
/* Kuribo's by-name linking between modules. */
void sms_mod_export(const char* name, void* fn);
void* sms_mod_import(const char* name);

/* A module's entry: called with attach = 1 at boot (in registration order,
 * BetterSunshineEngine first) and attach = 0 at exit. */
typedef int (*sms_mod_entry_t)(int attach);
void sms_mod_add_module(const char* name, sms_mod_entry_t entry);
/* Turns the registry on (lookups answer) when a code mod is linked in and
 * not switched off with SMS_CODE_MODS=0; the port calls it before the game
 * starts. */
void sms_mod_activate(void);
/* Runs the module entries once, from TApplication::initialize once the
 * game's heaps, DVD and GX are up (patch modhook-01). */
void sms_mod_start(void);
/* 1 once a code mod is linked in and started. */
int sms_mod_active(void);

#ifdef __cplusplus
}
#endif

#endif
