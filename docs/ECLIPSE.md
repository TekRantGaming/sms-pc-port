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
  A `bool` result fills r3 with 0 or 1 on the PowerPC, but natively it is the low byte alone, the rest of the register holding whatever it held, so where one side says `bool` and the other `BOOL`, `int` or `u32` the game and the mods must agree:
  the shim registers every patch target with a `bool` result or argument through a thunk ([kuribo_sdk.h](../platform/mods/eclipse/shim/Kuribo/sdk/kuribo_sdk.h)) that returns a pointer-sized 0 or 1 and reads each `bool` argument from the low byte of the word passed, however the hook calling it declares them;
  and `fixup_sources.py` declares the game's functions and virtual functions the mods call or override as the game does (`execute` and `receiveMessage` among them).
  After each Eclipse build [tools/mods/abi_check.py](../tools/mods/abi_check.py) compares the two sides' declarations in the binary's debug info (the mods are built with `-fstandalone-debug` for it): every game function the mods name and every game virtual function a mod class overrides; it fails the build on a `bool` against a wider integer (`-DSMS_ECLIPSE_ABI_CHECK=OFF` skips it).
  `raw_fn.hxx`'s calls return a pointer-sized integer, and the game's `operator new`, which only guarantees 4-byte alignment, serves every allocation (`-fnew-alignment=4 -fno-aligned-new`).
- **Data.** The Eclipse disc as it is, with the port's byte-order conversion; two converter fixes came from it (JAudio files read straight from disc, and J3D files whose empty sections point at the next table).
  Textures the mods build into their code are converted when the game first stores them, and the boot information the GameCube keeps at the bottom of memory (clocks, console type, disc ID), which the mods read directly, is filled in when they start.
- **Modules.** Each module built on BSE is linked into one object with its own names made local, as Kuribo keeps them apart (Eclipse and the moveset both define `gSettingsGroup`).

For bisecting, `SMS_MOD_LIST=1` prints every registered patch and `SMS_MOD_DISABLE=addr,addr` switches patches off by retail address; `SMS_MOD_REPORT=1` lists, at exit, patches the game never reached.

## Status (2026-10-01)

- The 32- and 64-bit ports both run all three modules on the Eclipse disc: BSE's first-boot settings screen (saved to the memory card), Eclipse's title screen and file select, its Tutorial stage with its dialogue, HUD and the moveset, and its first stage.
  With the same input the two play the same run.
- Rechecked after the maths, memory and decomp changes up to `a695da2`: a headless scripted run (`SMS_VI_DETERMINISTIC`, the settings saved by a first boot, then `SMS_AUTOPRESS` through the Tutorial's dialogue, a walk and a jump, the pause menu's Exit Area, and the title that follows) gives byte-identical frames in the 32- and 64-bit builds.
  `tools/regress/regress.py eclipse` makes that run and the Fire Petey and Dark Zhine warps below in both builds and compares them with its baseline ([DEVELOPMENT.md](DEVELOPMENT.md#tools)).
  Leaving the Tutorial had found three faults: BSE's sun code passed a `Vec` through a `(...)` cast by value, two `f32` raw_fn macros still called retail addresses, and the decomp's 64-bit `JKRArchive` had outgrown SunshineHeaderInterface's (the layout patch is regenerated after any decomp change to a class the mods see or allocate).
- SunshineHeaderInterface had `TBossPakkun` 4 bytes short (460 bytes; retail's `new` asks for 0x1D0, and the byte at 0x1CC is the boss music flag), so Eclipse's `TFireyPetey` put its first member on that flag, and `TMapObjBall` 4 bytes long (`_198` is `TResetFruit`'s); `fixup_sources.py` corrects both and `verify.py` now reports no difference.
  It also puts `J3DTevBlock`'s by-pointer and by-value setters in retail's vtable order: natively, Dark Zhine's colour change called the by-pointer one with the colour as its address.
- `SMS_WARP=72,0` reaches the Fire Petey fight (`yoshiBoss`) and `SMS_WARP=79,0` Dark Zhine's (`lighthouseBoss`) from a file-select load, after the Tutorial's Exit Area.
  The 64-bit build had crashed loading the Fire Petey stage: BSE builds the indirect sea in the 0x80 bytes of retail's `TMapStaticObj`, which is 168 bytes there, so the hook in `modhook-36-Map.patch` now allocates it from the same heap instead.
  With the same input the two builds then play the same run in both stages.
- The Yoshis of Eclipse's Yoshi village (`yoshi` and `yoshiBoss`) are Pianta NPCs (`NPCMonteMA`) whose body colour indices run from 0 to 11 in a 10-entry table (`sMonteM_BodyColorBuf`).
  On the console entries 10 and 11 are the bytes after it in the DOL (the string `_hand_mat`, then `sMonteM_BodyColor`): a green Yoshi and a black one.
  The port read its own neighbours instead, another table's white in the 32-bit build and zero padding in the 64-bit build, so the Fire Petey stage opened on a white or a black Yoshi; `bounds-03` gives such entries retail's bytes, and the Fire Petey frames are now byte-identical between the builds.
  A scan of every scene on the Eclipse disc found the other indices past their tables (Piantas in `cruiser`, `peachBeach`, `montePit`, `redCity`, `junctionRoom7`, `coro_ex3`, `peachCastle_ex7` and `_ex22`, and two `dolpic` scenes), and `bounds-03` covers them too; the retail disc has none outside Nintendo's `test11`.
- Dark Zhine's frames differed between the builds (a mean 0.1 of 255, its pose): in the 64-bit build its nerves ended at random, and its spine then had no nerve to run (`TSpineBase : broken nerve chain`, about 6,300 times a run).
  SunshineHeaderInterface declares `TNerveBase::execute` and `THitActor::receiveMessage` `bool`, where the game's are `BOOL`, and the game's caller tests the whole register (`TSpineBase<TLiveActor>::update`'s `cmpwi r3, 0` after the call).
  On the PowerPC a bool is a whole register, 0 or 1; natively clang returns it in the low byte alone (`setge %al`), and the game read whatever the rest of the register held: in the 64-bit build often not zero, so a nerve that had returned false was taken as finished.
  `fixup_sources.py` declares both `int` in SunshineHeaderInterface and in the mods' 41 nerves and 11 actors (Dark Zhine, Fire Petey and its two hit parts, the Bowser car and six objects), which return 0 or 1 in the whole register as on the console.
  The Dark Zhine frames are now byte-identical between the builds and the 32-bit ones unchanged; the Fire Petey, Tutorial and vanilla runs are unchanged in both builds.
- The same mismatch, audited at every boundary between the game and the mods (2026-10-01):
  58 of the 401 functions the mods register as patch targets (70 sites) return `bool`, and six of those sites read more than a byte (the hooks in place of `ViewFrustumClipCheck` in `TLiveManager::clipActorsAux`, `isLast1AnimeFrame`, `onYoshi`, `checkStickRotate`, which the game compares with 1, `JKRGetResource`, whose pointer `TMarDirector` tests, and the entry hook of `TMarioAnimeData::isPumpOK`); the shim's thunk covers all 70 and the 5 targets with `bool` arguments.
  Of the 670 game functions the mods call and the 209 game virtual functions their classes override, SunshineHeaderInterface declared 41 `bool` where the game returns a word and 4 the other way round (the port's `JStage` `JSG*` functions), and three arguments `bool` that the game reads as a word; `fixup_sources.py` declares them as the game does.
  The raw_fn trampolines convert by their C++ types already, and the function pointers the mods hand the game (SunScript builtins, the demo camera callback, the flag tables) have the same type on both sides.
  The Tutorial, Fire Petey and Dark Zhine runs and the vanilla runs are unchanged.
- The memory card BSE's first boot saves (`better_sunshine_engine.dat`, `better_sunshine_moveset.dat`, `super_mario_sunshine.dat`) differed between the 32 and 64-bit builds in a few hundred bytes, though the frames matched.
  Each module copies its banner and two icons to the card from BTI files built into its code, 0xE00 and 0xA00 bytes from the image offset, as BSE's `UpdateSavedSettings` does for all three, but the files end with the last palette colour their pixels use, up to 0x1A2 bytes sooner (five of the six).
  The copy read on into whatever followed each array in the build, which differs between the builds (and between any two builds), and on the console is whatever follows it in the module's image.
  `fixup_sources.py` sizes the six arrays to the copy, so the tail is zeros: palette entries that no pixel uses, which the console's icon and banner never show either.
  The two builds now write byte-identical card files, which `tools/regress/regress.py`'s `ecl-firstboot` run hashes; its frames and the other Eclipse runs are unchanged.
- **Byte order of what the mods read themselves.** The port keeps the game's data files big-endian where the game reads them through code it converts: the `.prm` parameter files (`TParamT<T>::load`, `endian-05`), the scene files (`0013`'s typed stream reads and `endian-08`'s `readBE`) and the memory card (`endian-11`).
  Code the mods compile from their own copies of the game's headers, or write themselves, does none of that, so it was audited (2026-10-01) against what the port converts:
  every function SunshineHeaderInterface's headers define that is compiled into the mods, 469, listed from the mods' debug info (371 JGadget container, allocator and utility templates, 34 of `TParams`, `TParamT` and `TParamRT`, 16 `TMario` parameter constructors, 15 `TVec3<f32>` operators, 7 nerve-stack helpers, 22 inline constructors and accessors of game classes, 2 GX FIFO writes and 2 empty `JPACallBackBase` callbacks);
  the 22 symbols the mods' library defines that the game defines too;
  the 77 stream reads and writes in the mods' own sources and their 79 casts to multi-byte pointers that do not name a retail address;
  and the mods' patches inside game functions the port converts in (the `J2DPane` stream constructor, `TFlagManager::load` and `save`, `TShine::loadBeforeInit`).
- `TParamT<T>::load` is the only header function among them that reads file data.
  SunshineHeaderInterface's is inline and reads the value raw; the game's (`ParamInst.cpp`) converts it.
  BetterSunshineEngine's copies are weak, and the game's, linked first, replace them, so BSE's `TParamRT<u16>` stage settings (`mPlayerHealth`, `mPlayerMaxHealth`, `mMusicID`) were read byte-swapped only as long as the game had no `TParamT<u16>::load` of its own: SunshineHeaderInterface's was then the only one.
  The decomp's 5a46e24 instantiated it in `ParamInst.cpp`, and Dark Zhine's stage got its life meter (port 51c7602 recorded the run).
  The moveset and Eclipse keep their definitions private (`--localize-hidden`, above), so theirs were always their own: the moveset read Luigi's and Piantissimo's `better_movement.prm` (jump count and gravity, speed and jump multipliers; Mario's archive has none) byte-swapped, a gravity multiplier of 0.8 coming out as about -4.3e8.
  `fixup_sources.py` now declares the six loads the game instantiates (`u8`, `s16`, `u16`, `s32`, `f32`, `TVec3<f32>`) `extern template` in SunshineHeaderInterface, so every module's parameters load through the game's whatever the link order; `bool` and `TColor` parameters are bytes and keep its raw load.
  The other twelve shared symbols are ten `TMario` parameter constructors, for which the game's strong definitions win, and the two empty callbacks.
- The mods' own scene objects read their parameters with raw multi-byte reads: BetterSunshineEngine's `GenericRailObj`, `ParticleBox`, `SoundBox` and `SimpleFog` (the first is in 146 of the disc's 218 scenes, the Tutorial and the Fire Petey stage among them) and its `CustomScene` table (`customScenes.bin`, which the Eclipse disc does not have), and Eclipse's `Tornado`, `BlowWind`, `DarknessEffect`, `ButtonSwitch` and `LaunchStar`.
  Natively their 28 floats and words and `CustomScene`'s six came out byte-swapped: the Tutorial's rolling cubes and poker chips got a base rotation of about 4e-8 instead of 0.35 and an animation rate of 4.6e-41 instead of 1, and the particle and sound boxes garbage IDs, rates and scales.
  `fixup_sources.py` reads them big-endian, with the decomp's `readBE` (and a `readData` counterpart) added to SunshineHeaderInterface's `JSUInputStream`; bytes, colours and strings stay as they are.
- The settings every module saves to the memory card (BSE's `IntSetting` and `FloatSetting`, Eclipse's darkness setting) were written and read in host order; they are now big-endian, as the console writes them and as the port keeps the game's own save (`endian-11`), so card files move between the port and a GameCube either way.
- BetterSunshineEngine's level select (debug mode only) reads the size of its arrow texture, built into its code, before the game first stores it and converts it (`modhook-14`), and the array was read-only; it is now writable and converted first.
- The rest needs nothing: the other header functions do not read file data, the other stream calls move bytes, strings, colours or BSE's extra shine bits, the casts address the game's objects in memory, and BSE's `.blo` built into its code goes through the game's converting `J2DScreen` reader; BSE's widescreen patch inside the `J2DPane` constructor reads raw but is waived (`tools/mods/not_ported.txt`).
  The scene fixes change the Eclipse runs from the Tutorial on (particle effects and the turning objects in it, and through it the Fire Petey and Dark Zhine runs that start from it), and the card's settings files their bytes, alike in the 32- and 64-bit builds; without the scene fixes the frames of the first boot, Tutorial and Dark Zhine runs are unchanged.
- **Patches.** Every patch of the three modules is either hooked or waived: `tools/mods/port_status.py` lists none left to do (widescreen and frame-rate patches are waived, the port has its own; six more sit in code the mods compile out, which `--registered` shows as inactive).
- **Retail addresses and offsets in the mods' code.** Game data the mods reach by retail address goes to the port's objects ([rawdata.cpp](../platform/mods/eclipse/rawdata.cpp)); members they reach by retail offset go through `SMS_OFFSET`.
  A retail data address not listed there stops the game with a message naming it.
- **Updating Eclipse.** Bump the revisions in [cmake/eclipse.cmake](../cmake/eclipse.cmake); `fixup_sources.py` fails on any rule or patch hunk that no longer applies, `tools/mods/shi_layout/regen.sh` regenerates the layout patch, `gen_hooks.py`/`gen_rawfn.py` the generated hooks and trampolines, `port_status.py` lists new patches to hook, and `abi_check.py` (run by the build) lists new `bool`/`BOOL` disagreements.

## Licensing

A binary that includes BSE or Eclipse is a work under GPL-3.0.
Keeping them out of this repository and fetching them only when the Eclipse component is built keeps the port itself unaffected, but how builds with Eclipse may be shared depends on the port's own licence, which this repository does not state yet.
That is for the port's owner to decide before any Eclipse code is added.
