# SunshineHeaderInterface layouts for the native port

Super Mario Eclipse and the modules it is built on are written against SunshineHeaderInterface ("SHI"), which describes the game's classes as they are laid out on the GameCube.
The port lays the same classes out differently, and the mods' C++ is compiled natively into it, so SHI's view of each class has to be made to match the port's.
These tools do that, and produce `platform/mods/eclipse/shi-layout.patch`, which `fixup_sources.py` applies to the fetched SHI headers.

## Where the layouts differ

- **64-bit hosts**: pointers are 8 bytes, so almost every class moves.
- **The vtable pointer**: CodeWarrior places a class's vtable pointer after the data members it declares before its first virtual function (`TSpcInterp`'s at 0x5C, `TNozzleBase`'s at 0x364: see the constructors' `stw` of `__vt__` in the retail code).
  The Kuribo clang the mods are built with for the GameCube does the same, so SHI's offsets are retail offsets.
  The port's compilers put it at offset 0, so on 32-bit hosts too the members of such classes (and of classes containing them) are elsewhere.
  `vlate.py` finds these classes from the declarations (decomp and SHI) and `retail.py` moves their vtable pointers to rebuild the retail layout.
- **8-byte members** on 32-bit hosts: i386 aligns them to 4; `src/port_include/dolphin/types.h` and `-malign-double` for the mods restore the GameCube's 8.
- Counts SHI declares `size_t` (64-bit on LP64) where the game stores 32-bit words: `fixup_sources.py` retypes the ones in templates; `gen_layout.py` the ones in classes.
- Flag words SHI declares as bitfields (Mario's current and previous attribute flags, `TLiveActor`'s live flags, `TModelWaterManager`'s light flags and `TMarioGamePad`'s flags) are declared from the word's top bit down for the GameCube.
  Native compilers allocate bitfields in the opposite order, so `fixup_sources.py` reverses these declarations after applying the layout patch and gives every field the game word's type (`u32` or `u16`) for Windows compatibility.
  This preserves the game's numeric masks: enabling FLUDD must set `0x8000`, which the tutorial uses when entering its rocket and turbo sections, and the NPC carry tests must read `mCanBeTaken` as `LIVE_FLAG_UNK100000` (`0x100000`), which Shadow Mario sets on Peach before he takes her in Delfino Plaza.

Check the fixed-up flag declarations with `python3 tools/mods/test_shi_flags.py build-ecl/eclipse-src/shi --compiler clang++`.
The check covers both host widths and native and Microsoft bitfield allocation rules, including reading, setting, and clearing each known flag without changing neighbouring bits.

## How a class is re-laid out

For each SHI class with a port counterpart, every member at retail offset R is placed where the port keeps whatever is at retail offset R (the retail layout of the port's class, then the same member in the port's 32- or 64-bit layout).
The class body gets, under `#if __SIZEOF_POINTER__ == 8` / `#else`, explicit `u8 _pc64_N[...]` / `_pc32_N` padding before members that move, the retail padding members (`_XX`) removed where nothing uses them, and a tail pad up to where the retail end of the class lands.
Members SHI places where retail has nothing of the kind are left in place and logged.
Only the members the mods use (`used_members.py`) and the size of the classes they derive from are checked by `verify.py`; the rest follows from the same mapping.

## Regenerating the patch

Needed after moving to another SHI, BetterSunshineEngine, Moveset or Eclipse revision, or after a decomp change to a class the mods use:

```
cmake -S . -B build-ecl -DSMS_ECLIPSE=ON ...   # fetches the sources to build-ecl/eclipse-src
./build.sh && SMS_ARCH=64 ./build.sh           # build/linux-32 and build/linux-64: the plain builds (their debug info gives the port's layouts)
tools/mods/shi_layout/regen.sh /tmp/shi-layout-work
```

`regen.sh` resets the fetched SHI to its mechanical fixups, probes SHI's layouts (`shi_probe.cpp`, compiled with clang for both hosts, read back with gdb through `export.py`), exports the port's layouts from the two builds, finds the used members, writes the re-laid-out headers, compiles and checks them, and writes the patch.
The work directory keeps everything it computed; `gen_layout.log` lists the members left in place.

## Retail offsets in the mods' code

Where mod code addresses a game object by retail byte offset (`*(u16 *)((u8 *)director + 0x50)`), its source says `SMS_OFFSET(TMarDirector, 0x50)` instead (`platform/mods/eclipse/mods-port.patch`).
`offsets.py header` generates the table (`platform/mods/eclipse/shim/sms_offsets.h`) from the uses; `offsets.py query Class offset` shows what is at a retail offset and where each port keeps it.
