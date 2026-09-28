// The code-mod patch registry (see sms_mod/modhooks.h).
#include "sms_mod/modhooks.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Patch {
	int kind;
	uint32_t addr;
	uintptr_t value;
	bool enabled;
	bool consulted; // the game asked for this address at least once
	const char* file;
	int line;
};

struct Module {
	std::string name;
	sms_mod_entry_t entry;
};

// Function-local statics: patches register from static constructors, in
// whatever order the linker runs them.
std::vector<Patch>& patches()
{
	static std::vector<Patch>* p = new std::vector<Patch>;
	return *p;
}
std::unordered_multimap<uint32_t, int>& byAddr()
{
	static std::unordered_multimap<uint32_t, int>* m = new std::unordered_multimap<uint32_t, int>;
	return *m;
}
std::unordered_map<std::string, void*>& exports()
{
	static std::unordered_map<std::string, void*>* m = new std::unordered_map<std::string, void*>;
	return *m;
}
struct DataBinding {
	uint32_t addr;
	void* var;
	int size;
};
std::vector<DataBinding>& data_bindings()
{
	static std::vector<DataBinding>* v = new std::vector<DataBinding>;
	return *v;
}

std::vector<Module>& modules()
{
	static std::vector<Module>* m = new std::vector<Module>;
	return *m;
}
bool s_active = false;  // lookups answer
void bump();
bool s_enabled = false; // code mods linked in and not switched off

// The last enabled patch of `kind` at `addr` wins, as the last write would.
const Patch* find(uint32_t addr, int kindMask)
{
	auto range = byAddr().equal_range(addr);
	const Patch* best = nullptr;
	int bestId = -1;
	for (auto it = range.first; it != range.second; ++it) {
		Patch& p = patches()[it->second];
		if (!p.enabled || !((1 << p.kind) & kindMask))
			continue;
		if (it->second > bestId) {
			best = &p;
			bestId = it->second;
		}
	}
	return best;
}

void report()
{
	if (!getenv("SMS_MOD_REPORT"))
		return;
	int total = 0, used = 0;
	for (const Patch& p : patches()) {
		total++;
		used += p.consulted;
	}
	fprintf(stderr, "[mod] %d patches registered, %d reached by the game\n", total, used);
	for (const Patch& p : patches())
		if (!p.consulted)
			fprintf(stderr, "[mod]   not reached: %s %08x (%s:%d)\n",
			        p.kind == SMS_MOD_BRANCH ? "b " : p.kind == SMS_MOD_CALL ? "bl" : "w ",
			        (unsigned)p.addr, p.file ? p.file : "?", p.line);
}

} // namespace

// The retail registers a mod function reads at a call site (SMS_FROM_GPR),
// set by the port's hook there (SMS_MOD_GPR in sms_modhook.h).
extern "C" uintptr_t sms_mod_gpr[32] = {};
extern "C" double sms_mod_fpr[32]    = {};

// Bumped whenever an answer may change; hook sites cache per generation.
extern "C" unsigned int sms_mod_generation = 1;
namespace {
void bump() { sms_mod_generation++; }
} // namespace

extern "C" int sms_mod_register(int kind, uint32_t addr, uintptr_t value, int enabled,
                                const char* file, int line)
{
	int id = (int)patches().size();
	patches().push_back({kind, addr, value, enabled != 0, false, file, line});
	byAddr().emplace(addr, id);
	bump();
	return id;
}

extern "C" void sms_mod_code_write(uint32_t addr, uint32_t value, int size)
{
	const char* what = size == 1 ? "PowerPC::writeU8" : size == 2 ? "PowerPC::writeU16" : "PowerPC::writeU32";
	sms_mod_register(SMS_MOD_WORD, addr, value, 1, what, 0);
}

extern "C" void sms_mod_set_enabled(int id, int on)
{
	if (id >= 0 && id < (int)patches().size() && patches()[id].enabled != (on != 0)) {
		patches()[id].enabled = on != 0;
		bump();
	}
}

extern "C" int sms_mod_is_enabled(int id)
{
	return id >= 0 && id < (int)patches().size() && patches()[id].enabled;
}

extern "C" void* sms_mod_target(uint32_t addr)
{
	if (!s_active)
		return nullptr;
	const Patch* p = find(addr, (1 << SMS_MOD_BRANCH) | (1 << SMS_MOD_CALL));
	if (!p)
		return nullptr;
	const_cast<Patch*>(p)->consulted = true;
	return (void*)p->value;
}

extern "C" int sms_mod_word(uint32_t addr, uint32_t* value)
{
	if (!s_active)
		return 0;
	const Patch* p = find(addr, 1 << SMS_MOD_WORD);
	if (!p)
		return 0;
	const_cast<Patch*>(p)->consulted = true;
	*value = (uint32_t)p->value;
	return 1;
}

extern "C" void sms_mod_bind_data(uint32_t addr, void* var, int size)
{
	data_bindings().push_back({addr, var, size});
}

extern "C" void sms_mod_export(const char* name, void* fn)
{
	exports()[name] = fn;
}

extern "C" void* sms_mod_import(const char* name)
{
	auto it = exports().find(name);
	return it == exports().end() ? nullptr : it->second;
}

extern "C" void sms_mod_add_module(const char* name, sms_mod_entry_t entry)
{
	modules().push_back({name, entry});
}

// The code mods' static constructors (patch registrations, module entries,
// their globals), moved out of the process's own by cmake/eclipse.cmake.
typedef void (*ctor_t)(void);
extern "C" ctor_t __start_sms_mod_ctors[] __attribute__((weak));
extern "C" ctor_t __stop_sms_mod_ctors[] __attribute__((weak));

extern "C" void sms_mod_activate(void)
{
	if (s_enabled || __start_sms_mod_ctors == __stop_sms_mod_ctors)
		return;
	const char* e = getenv("SMS_CODE_MODS");
	if (e && !strcmp(e, "0")) {
		fprintf(stderr, "[mod] code mods linked in but switched off (SMS_CODE_MODS=0)\n");
		return;
	}
	s_enabled = true;
}

extern "C" uint32_t __OSBusClock;
extern "C" uint32_t __OSCoreClock;
extern "C" unsigned long OSGetConsoleType(void);
extern "C" void* DVDGetCurrentDiskID(void);

namespace {
// The boot information the GameCube's OS keeps at the bottom of MEM1, which
// the mods read directly (the bus clock for OSTicksToSeconds, the console
// type, the disc's ID). The port's own code never reads it there, so it is
// only filled in for a code mod, in host byte order as the mods read it.
void fill_boot_info()
{
	uint8_t* low = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(0x80000000u));
	memcpy(low, DVDGetCurrentDiskID(), 0x20);
	const uint32_t words[][2] = {
		{0x28, 0x01800000u},               // physical memory size
		{0x2C, (uint32_t)OSGetConsoleType()},
		{0xF0, 0x01800000u},               // simulated memory size
		{0xF8, __OSBusClock},
		{0xFC, __OSCoreClock},
	};
	for (const auto& w : words)
		memcpy(low + w[0], &w[1], 4);
}
} // namespace

extern "C" void sms_mod_start(void)
{
	static bool started = false;
	if (!s_enabled || started)
		return;
	started = true;
	fill_boot_info();
	for (ctor_t* c = __start_sms_mod_ctors; c < __stop_sms_mod_ctors; ++c)
		if (*c && *c != (ctor_t)-1)
			(*c)();
	// SMS_MOD_DISABLE=addr,addr,...: switch off the patches at these retail
	// addresses (hex), to bisect a mod's patches while porting it.
	if (const char* e = getenv("SMS_MOD_DISABLE")) {
		for (const char* q = e; *q;) {
			char* end;
			unsigned long a = strtoul(q, &end, 16);
			if (end == q)
				break;
			for (Patch& p : patches())
				if (p.addr == a && p.enabled) {
					p.enabled = false;
					fprintf(stderr, "[mod] patch at %08lx switched off (SMS_MOD_DISABLE)\n", a);
				}
			q = *end == ',' ? end + 1 : end;
		}
	}
	if (getenv("SMS_MOD_LIST"))
		for (const Patch& p : patches())
			fprintf(stderr, "[mod] %s %08x -> %08lx%s (%s:%d)\n",
			        p.kind == SMS_MOD_BRANCH ? "b " : p.kind == SMS_MOD_CALL ? "bl" : "w ", (unsigned)p.addr,
			        (unsigned long)p.value, p.enabled ? "" : " (off)", p.file ? p.file : "?", p.line);
	s_active = true;
	bump();
	// Game data the mods rewrote in place.
	for (const DataBinding& d : data_bindings()) {
		uint32_t w;
		if (d.size == 4 && sms_mod_word(d.addr, &w))
			memcpy(d.var, &w, 4);
	}
	fprintf(stderr, "[mod] %zu patches registered by %zu module(s)\n", patches().size(), modules().size());
	// BetterSunshineEngine first: the other modules register with it.
	std::vector<Module> order;
	for (const Module& m : modules())
		if (m.name.find("Better") != std::string::npos)
			order.push_back(m);
	for (const Module& m : modules())
		if (m.name.find("Better") == std::string::npos)
			order.push_back(m);
	for (const Module& m : order) {
		int rc = m.entry(1);
		fprintf(stderr, "[mod] module \"%s\" started (%d)\n", m.name.c_str(), rc);
	}
	atexit(report);
}

extern "C" int sms_mod_active(void)
{
	return s_active;
}
