# Super Mario Eclipse in the native port

[Super Mario Eclipse](https://github.com/JoshuaMKW/Super-Mario-Eclipse) is a large Sunshine mod: new stages, objects, characters, menus and script functions.
This page records what running it natively takes, what the port already has for it, and a plan.
Nothing of Eclipse is in this repository.

## How Eclipse runs on a GameCube (or Dolphin)

- **Code.** Eclipse is a Kuribo module (PowerPC code loaded at boot by a patched `main.dol`) built on [BetterSunshineEngine](https://github.com/DotKuribo/BetterSunshineEngine) (BSE), another Kuribo module.
  Both change the game by overwriting the retail binary at fixed addresses: a `bl` redirected to their own function (`SMS_PATCH_BL`), a branch (`SMS_PATCH_B`), or an instruction replaced (`SMS_WRITE_32`).
  BSE turns many of those into an API (stage, player and game callbacks, object registration, SunScript functions, THP and music, settings), which Eclipse uses; Eclipse adds its own patches besides.
- **Game classes.** Both are written against [SunshineHeaderInterface](https://github.com/JoshuaMKW/SunshineHeaderInterface), their own declarations of the retail classes: the same memory layout as the decomp's, under other member names (`TMario::mState` there is `mStatus` here, `mSpeed` is `mVel`).
- **Data.** Eclipse's stages, models, text and movies are files on its disc: its `build.py` assembles an extracted game folder and packs it into an ISO.
- **Release.** Players get Eclipse from [GameBanana](https://gamebanana.com/mods/536309) (v1.1.0): a 7z holding an xdelta patch that turns the North American ISO (MD5 `0c6d2edae9fdf40dfc410ff1623e4119`) into a `GMSE04` Super Mario Eclipse ISO, with its code already built into the disc's `main.dol`.
- **Licence.** Eclipse's code, BSE and SunshineHeaderInterface are GPL-3.0; the released mod is CC BY-NC-ND 4.0, and its patcher script MIT.

## Size of the job

Counted with [`tools/mods/patch_inventory.py`](../tools/mods/patch_inventory.py), which resolves every patch address to the game function it lands in and, from the decomp's linked `mario.elf`, the retail instruction it replaces:

| | patches | game functions touched | of which `bl` redirects |
| --- | --- | --- | --- |
| Eclipse ([inventory](mods/eclipse-patches.md)) | 257 | 95 | 144 |
| BSE ([inventory](mods/bse-patches.md)) | 697 | 259 | 377 |

Eclipse's own code is about 18,500 lines and calls 79 BSE API functions (most often `Spc::` script builtins, `Stage::register*Stage` and `add*Callback`, `Objects::registerObjectAs*`, `Player::add*Callback` and per-player data, `THP::addTHP`, `Music::`, `Settings::`).
Much of BSE's own patching is features the port has or does not need (60 fps, 16:9 and 21:9, bug fixes, the Kuribo loader), so only the part of BSE that Eclipse reaches has to come along.

## Building it

```sh
python3 tools/mods/get.py eclipse          # the Eclipse ISO, mods/eclipse/Super Mario Eclipse v1.1.0.iso
cmake -S . -B build-ecl -DSMS_ARCH=32 -DSMS_ECLIPSE=ON     # or -DSMS_ARCH=64
cmake --build build-ecl
SMS_DISC_IMAGE="mods/eclipse/Super Mario Eclipse v1.1.0.iso" build-ecl/sms
```

`-DSMS_ECLIPSE=ON` ([cmake/eclipse.cmake](../cmake/eclipse.cmake)) fetches Eclipse, BSE, [BetterSunshineMoveset](https://github.com/JoshuaMKW/BetterSunshineMoveset) (a third module Eclipse requires) and SunshineHeaderInterface at pinned revisions into the build directory (`SMS_ECLIPSE_SRC_DIR` to put them elsewhere), fixes them up ([fixup_sources.py](../platform/mods/eclipse/fixup_sources.py), which also applies the two patches below) and builds them with clang into the port.
Nothing of theirs is kept in this repository.
Without it, the build is the plain port: every hook below is in the source but finds nothing registered and runs the original code.

## How the port runs it

- **Patches.** Each `SMS_PATCH_BL`/`SMS_PATCH_B`/`SMS_WRITE_32` registers under its retail address in the port's registry ([modhooks.cpp](../platform/mods/modhooks.cpp)) instead of writing to memory, from the mods' static constructors, which run when the modules load (after `TApplication::initialize` has the heaps and DVD up), as Kuribo runs them.
  BSE's run-time instruction rewrites (`PowerPC::writeU32`) are recorded the same way.
- **Hooks.** The decomp source asks the registry at each patched call site ([sms_modhook.h](../src/port_include/sms_modhook.h)).
  [tools/mods/gen_hooks.py](../tools/mods/gen_hooks.py) writes most of them (`decomp-patches/zz-modhook-50-calls.patch`): it finds the retail call in the disassembly, the matching call in the source, and emits a typed hook.
  It also passes on what a mod function reads from its caller's registers (`SMS_FROM_GPR`), worked out from the retail code around the call, and reorders arguments into the mod function's declared order (the PowerPC keeps integer and float arguments apart, so a mod may declare them in any interleaving).
  The rest are hand-written `modhook-*` patches.
- **Game functions by retail name.** SunshineHeaderInterface's `raw_fn.hxx` calls game functions through casts of their retail addresses; those go to typed trampolines into the decomp, generated by [tools/mods/gen_rawfn.py](../tools/mods/gen_rawfn.py).
  Functions the decomp only has inline are in [port_shims.cpp](../platform/mods/eclipse/port_shims.cpp).
- **Layouts.** SunshineHeaderInterface describes the game's classes as the GameCube lays them out; the port lays them out otherwise: with 8-byte pointers on 64-bit hosts, and on every host with the vtable pointer at offset 0 where CodeWarrior (and the Kuribo clang the mods are built with) puts it after the members a class declares before its first virtual function.
  [tools/mods/shi_layout](../tools/mods/shi_layout/README.md) re-lays SunshineHeaderInterface's classes out, member by member, where the port keeps the same retail member ([shi-layout.patch](../platform/mods/eclipse/shi-layout.patch)), and checks every member the mods use and the size of every class they derive from.
  Where mod code addresses a game object by retail byte offset, its source says `SMS_OFFSET(Class, offset)` ([mods-port.patch](../platform/mods/eclipse/mods-port.patch), table generated by `offsets.py`).
  The 32-bit port also keeps 8-byte members 8-aligned (`dolphin/types.h`, `-malign-double` for the mods) and never reuses a base's tail padding (the `layout-01` patch), as CodeWarrior does.
- **Calls.** Classes the game passes through memory because they have a user-written copy constructor (`TVec3<f32>`, `JUTRect`) are declared so in SunshineHeaderInterface too, and aggregates the mods pass through a `(...)` cast go by address, as on the PowerPC.
  `raw_fn.hxx`'s calls return a pointer-sized integer, and the game's `operator new`, which only guarantees 4-byte alignment, serves every allocation (`-fnew-alignment=4 -fno-aligned-new`).
- **Data.** The Eclipse disc as it is, with the port's byte-order conversion; two converter fixes came from it (JAudio files read straight from disc, and J3D files whose empty sections point at the next table).
  Textures the mods build into their code are converted when the game first stores them, and the boot information the GameCube keeps at the bottom of memory (clocks, console type, disc ID), which the mods read directly, is filled in when they start.
- **Modules.** Each module built on BSE is linked into one object with its own names made local, as Kuribo keeps them apart (Eclipse and the moveset both define `gSettingsGroup`).

For bisecting, `SMS_MOD_LIST=1` prints every registered patch and `SMS_MOD_DISABLE=addr,addr` switches patches off by retail address; `SMS_MOD_REPORT=1` lists, at exit, patches the game never reached.

## Status (2026-10-01)

- The 32- and 64-bit ports both run all three modules on the Eclipse disc: BSE's first-boot settings screen (saved to the memory card), Eclipse's title screen and file select, its Tutorial stage with its dialogue, HUD and the moveset, and its first stage.
  With the same input the two play the same run.
- Rechecked after the maths, memory and decomp changes up to `a695da2`: a headless scripted run (`SMS_VI_DETERMINISTIC`, the settings saved by a first boot, then `SMS_AUTOPRESS` through the Tutorial's dialogue, a walk and a jump, the pause menu's Exit Area, and the title that follows) gives byte-identical frames in the 32- and 64-bit builds.
  Leaving the Tutorial had found three faults: BSE's sun code passed a `Vec` through a `(...)` cast by value, two `f32` raw_fn macros still called retail addresses, and the decomp's 64-bit `JKRArchive` had outgrown SunshineHeaderInterface's (the layout patch is regenerated after any decomp change to a class the mods see or allocate).
- Still open: `TBossPakkun` is 460 bytes in SunshineHeaderInterface's 32-bit layout and 464 in the port's (data to 461), and Eclipse's `TFireyPetey` derives from it; `verify.py` reports it.
- **Patches.** Every patch of the three modules is either hooked or waived: `tools/mods/port_status.py` lists none left to do (widescreen and frame-rate patches are waived, the port has its own; six more sit in code the mods compile out, which `--registered` shows as inactive).
- **Retail addresses and offsets in the mods' code.** Game data the mods reach by retail address goes to the port's objects ([rawdata.cpp](../platform/mods/eclipse/rawdata.cpp)); members they reach by retail offset go through `SMS_OFFSET`.
  A retail data address not listed there stops the game with a message naming it.
- **Updating Eclipse.** Bump the revisions in [cmake/eclipse.cmake](../cmake/eclipse.cmake); `fixup_sources.py` fails on any rule or patch hunk that no longer applies, `tools/mods/shi_layout/regen.sh` regenerates the layout patch, `gen_hooks.py`/`gen_rawfn.py` the generated hooks and trampolines, and `port_status.py` lists new patches to hook.

## Licensing

A binary that includes BSE or Eclipse is a work under GPL-3.0.
Keeping them out of this repository and fetching them only when the Eclipse component is built keeps the port itself unaffected, but how builds with Eclipse may be shared depends on the port's own licence, which this repository does not state yet.
That is for the port's owner to decide before any Eclipse code is added.
