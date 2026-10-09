#!/usr/bin/env python3
"""Mechanical fixes applied to the fetched Eclipse, BetterSunshineEngine and
SunshineHeaderInterface sources before the port compiles them (nothing of
theirs is kept in this repository). Each entry: a glob under the source root,
a regular expression and its replacement, and why. Entries with the same why
form a group, which must match at least once (unless marked OPTIONAL), so an
upstream change that makes a rule miss stops the build instead of passing
silently. The sources are reset to their checkouts first, then fixed up and
patched (shi-layout.patch, mods-port.patch); a marker records the state, so
an unchanged checkout is not redone.

    fixup_sources.py ECLIPSE_ROOT BSE_ROOT SHI_ROOT [MOVESET_ROOT]
"""
import glob
import os
import re
import sys

# A call through a literal retail address: the port's function for it
# (platform/mods/eclipse/rawfn_trampolines.cpp, tools/mods/gen_rawfn.py).
OPTIONAL = "optional"


def optional(fixes):
    """Rules shared by several modules, which need not all use what they fix."""
    return [f[:4] + (OPTIONAL,) for f in fixes]


RAWADDR_FIX = ("src/**/*.cpp", r"(\(\s*\([^;{}()]*\(\s*\*\s*\)\s*\([^;{}()]*\)\s*\)\s*)(0x8[0-3][0-9A-Fa-f]{6})(\s*\)\s*\()",
               r"\1sms_mod_rawaddr(\2)\3", "retail addresses called go to the port's functions")

# Game data by retail address, *(u32 **)0x8040E0BC: the port's object at that
# address instead (platform/mods/eclipse/rawdata.cpp lists them).
RAWDATA = "0x803ACA68|0x803ACAB0|0x803AFB48|0x803C0CC8|0x803C0CF0|0x803C0D18|0x803C0E30|0x803DF498|0x803DF4D8|0x803DFA00|0x8040DAB4|0x8040DABC|0x8040DFD4|0x8040DFE4|0x8040DFF4|0x8040E03C|0x8040E0BC|0x8040FA90"
RAWDATA_FIX = ("src/**/*.cpp", r"(\(\s*(?:const\s+)?[A-Za-z_][\w:<> ]*?\s*\*+\s*\))\s*(?i:(" + RAWDATA + r"))\b",
               lambda m: "%ssms_mod_rawdata(%s)" % (m.group(1), m.group(2)), "retail data addresses go to the port's objects")
# The same through SMS_PORT_REGION(ntscu, pal, ntscj, ntsck), as BSE's
# initAreaInfo names the game's shine and stage tables (area.cpp): unrouted,
# it read the game's heap where the tables are on the console, and every area
# of the base game had no scenarios and no shine select panes.
RAWDATA_REGION_FIX = ("src/**/*.cpp", r"SMS_PORT_REGION\(\s*(?i:(" + RAWDATA + r"))\s*,[^()]*\)",
                      lambda m: "sms_mod_rawdata(%s)" % m.group(1), "retail data addresses go to the port's objects")

# Textures built into the code as byte arrays are converted to host byte
# order in place when the game first stores them (JUTTexture::storeTIMG), so
# they cannot be read-only; static keeps the internal linkage const gave them.
# (The memory card banner and icon are declared extern and never stored.)
TEXTURE_FIXES = [
    (glob_, r"(?<!static )\bconst u8 SMS_ALIGN\(32\) (?!gSaveBnr\b|gSaveIcon\b)(\w+)\[\]",
     r"static u8 SMS_ALIGN(32) \1[]", "embedded textures are converted in place")
    for glob_ in ("src/**/*.cpp", "src/**/*.hxx", "include/**/*.hxx")
]

# The memory card banner and icons are copied to the card from these BTI files
# at their image offset (0x20) for as many bytes as the card holds (BSE's
# UpdateSavedSettings: 0xE00 for the CI8 banner, 0x500 per CI8 icon, two
# icons), but each file ends with its palette's last used colour, up to 0x1A2
# bytes short. The copy then read past the array into whatever the build put
# after it, so the 32 and 64-bit builds wrote different card files. Sized to
# what is copied, the rest is zeros: palette entries no pixel uses. (On the
# console those entries hold the bytes after the array in the module's image.)
CARD_IMAGE_FIXES = [
    (glob_, r"\bconst u8 (SMS_ALIGN\(32\) )?gSave(Bnr|Icon)\[\] = \{",
     lambda m: "const u8 %sgSave%s[%s] = {" % (m.group(1) or "", m.group(2),
                                              "0x20 + 0xE00" if m.group(2) == "Bnr" else "0x20 + 0x500 * 2"),
     "the card banner and icons are as long as the copy to the card")
    for glob_ in ("src/**/*.cpp", "src/**/*.hxx")
]

# Its caller passes the particle id in a full register (0x113); declared u8,
# it only works on the PowerPC, where the value is used unmasked.
PARTICLE_FIXES = [
    (glob_, r"(smParticleInit\(JPAResourceManager \*\s*\w*,\s*const char \*\s*\w*,\s*)u8(\s*\w*\))",
     r"\1u32\2", "the particle id is 16 bits")
    for glob_ in ("src/**/*.cpp", "include/**/*.hxx")
]

# Two TGCConsole2::checkChangeTelopArray switch-table entries are PowerPC
# assembly: store a news list in the console (r30) and jump back to the end
# of the switch. The port calls the entry with the console and continues
# after the switch itself, so they become the store alone.
DEBS_FIXES = [
    ("src/stage/behavior.cpp",
     r"SMS_ASM_FUNC static void (set\w+DEBSList)\(TGCConsole2 \*console2\) \{\n"
     r"\s*SMS_ASM_BLOCK\(\"lis 3, (\w+)@h[^;]*\);\n\}",
     r"static void \1(TGCConsole2 *console2) {\n    *(s32 **)((u8 *)console2 + 0x574) = \2;\n}",
     "news list setters without assembly"),
]

# The game's nerves and actors return BOOL from execute and receiveMessage,
# and its callers test the whole word (TSpineBase<TLiveActor>::update's
# `cmpwi r3, 0` after the execute call). SunshineHeaderInterface declares
# them bool. On the PowerPC a bool is a whole register, 0 or 1, so that does
# not matter there; natively a bool is returned in the low byte alone, and
# the game's callers read the rest of the register too: TDarkZhine's nerves
# (`setge %al`) ended at random and left its spine with no nerve. Declared
# int, the mods' overrides return 0 or 1 in the whole register, as they do
# on the console.
BOOL_RET_FIXES = [
    (glob_, r"\bbool(\s+(?:\w+::)?(?:execute\s*\(\s*TSpineBase\b|receiveMessage\s*\(\s*THitActor\b))",
     r"int\1", "execute and receiveMessage return a full word")
    for glob_ in ("src/**/*.cpp", "include/**/*.hxx")
]

# The other way round: game functions the mods call (or could override) that
# return BOOL, int or u32 in the game, where SunshineHeaderInterface says
# bool. The mods would read the low byte of a whole word, which is right only
# while the game returns 0 or 1 (checkGroundAtWalking returns up to 3); and
# declared bool, an override of one of the virtual ones would return a byte to
# a caller that tests the word. Declared as the game declares them, the mods
# read the whole word, and an override returning bool does not compile.
# tools/mods/abi_check.py, run after each Eclipse build, finds new ones; the
# patch targets the game calls are covered by the shim (Kuribo/sdk/kuribo_sdk.h).
SHI_WORD_RET = [
    ("int", "moveToNextNode|entryMatColorAnimator|traceSpline|checkCurAnm|checkCurAnmFromIndex|"
            "checkCurBckFromIndex|curAnmEndsNext|rocketCheck|checkBackTrig|checkGroundPlane|"
            "checkStickRotate|isAnimeLoopOrStop|isLast1AnimeFrame|changePlayerStatus|"
            "changePlayerJumping|changePlayerDropping|checkGroundAtWalking|isMario|jumpMain|"
            "hasMapCollision|onYoshi|isDummy|isPumpOK|DVDOpen|DVDFastOpen|DVDClose|"
            "DVDPrepareStreamAsync|DVDCancelStreamAsync|DVDStopStreamAtEndAsync|"
            "DVDGetStreamErrorStatusAsync|DVDGetStreamPlayAddrAsync|DVDCheckDisk|"
            "OSDisableInterrupts|OSCreateThread|OSJoinThread|OSIsThreadTerminated|calcRecycle|"
            "belongToGround|isReachedToGoal"),
    ("u32", "startVoice|startVoiceIfNoVoice"),
]
SHI_WORD_FIXES = [
    (glob_, r"\bbool(\s+(?:%s)\s*\()" % names, ty + r"\1", "game functions returning a word return one")
    for ty, names in SHI_WORD_RET for glob_ in ("include/**/*.hxx", "include/**/*.h")
] + [
    # And the JStage functions the port declares bool (its default
    # implementations return false), which SunshineHeaderInterface calls u32.
    ("include/JSystem/JStage/*.hxx", r"\bvirtual (?:u32|unsigned long)(\s+JSG(?:GetName|GetData|GetSystemData|CreateObject)\s*\()",
     r"virtual bool\1", "JStage's bool results are bools"),
    # BOOL and s32 arguments: clang passes a bool zero-extended to the word
    # anyway, but the declarations then say what the game reads.
    ("include/Dolphin/OS.h", r"(OSRestoreInterrupts\()bool(\s+enable\))", r"\1int\2",
     "BOOL arguments are words"),
    ("include/Dolphin/OS.h", r"(OS(?:Send|Receive)Message\([^;]*,\s*)bool(\s+block\))", r"\1s32\2",
     "BOOL arguments are words"),
]

# Integer and float widths where the game and the mods call each other
# (tools/mods/abi_check.py lists them). On the PowerPC a callee extends a
# u8, s8, u16 or s16 result to the whole of r3 and the mods, built by clang,
# take r3 to be extended for the type they declare, using it as it is: a mod
# reading a narrower type than the game returns sees the game's whole word
# (patchYStorageWalkEnd hands TMario::walkEnd's word back to the game
# unmasked), and one reading a wider type the extended value. Natively the
# callee leaves the bits above a narrow result undefined and the reader
# extends what it reads itself: a wider read sees garbage, a narrower one
# another value. Declared as the game declares them, the mods read what
# they read on the console.
def game_types(path, rx, repl):
    return (path, rx, repl, "the game's result and argument types: " + path)


SHI_GAME_TYPES = [
    game_types("include/SMS/Player/Mario.hxx", r"\bu8(\s+(?:jumpingBasic|jumpProcess|walkEnd)\s*\()", r"int\1"),
    game_types("include/SMS/System/Application.hxx", r"\bu8(\s+gameLoop\s*\()", r"int\1"),
    game_types("include/SMS/Graph/GraphWeb.hxx", r"\bs16(\s+getRandomButDirLimited\s*\()", r"int\1"),
    game_types("include/SMS/rand.h", r'extern "C" u16 rand\(\);', 'extern "C" int rand();'),
    game_types("include/SMS/System/Resolution.hxx", r"\bint(\s+SMSGet(?:Title|Game)Render(?:Width|Height)\s*\(\s*\);)",
               r"u16\1"),
    game_types("include/SMS/MapObj/MapObjBase.hxx", r"\bvirtual u32(\s+getHitObjNumMax\s*\()", r"virtual u16\1"),
    game_types("include/SMS/MoveBG/EggYoshi.hxx", r"\bvirtual u32(\s+getHitObjNumMax\s*\()", r"virtual u16\1"),
    game_types("include/SMS/MSound/MSound.hxx", r"\bbool(\s+getMapInfoGround\s*\(u32\);)", r"u32\1"),
    game_types("include/JSystem/JAudio/JAIBasic.hxx", r"\bvirtual bool(\s+getMapInfoFxline\s*\()", r"virtual u16\1"),
    game_types("include/JSystem/JAudio/JAIBasic.hxx", r"\bvirtual bool(\s+getMapInfoGround\s*\()", r"virtual u32\1"),
    game_types("include/JSystem/JUtility/JUTFont.hxx", r"\bvirtual bool(\s+getLeading\s*\(\s*\)\s*const)", r"virtual int\1"),
    game_types("include/JSystem/JUtility/JUTResFont.hxx", r"\bvirtual bool(\s+getLeading\s*\(\s*\)\s*const)", r"virtual int\1"),
    game_types("include/JSystem/JKernel/JKRArchivePri.hxx", r"\bvirtual s32(\s+becomeCurrent\s*\()", r"virtual bool\1"),
    game_types("include/JSystem/JKernel/JKRHeap.hxx", r"\bvirtual u32(\s+dump_sort\s*\()", r"virtual bool\1"),
    game_types("include/JSystem/JKernel/JKRHeap.hxx", r"\bvirtual u8(\s+changeGroupID\s*\()", r"virtual s32\1"),
    game_types("include/JSystem/JStage/JSGCamera.hxx", r"\bvirtual s32(\s+JSGGetViewType\s*\(\s*\)\s*const)", r"virtual bool\1"),
    # The game's axis is a (signed) char; the mods pass 'x', 'y' or 'z'.
    game_types("include/Dolphin/MTX.h", r"(PSMTXRotRad\(Mtx \w+, )u8(\s+axis)", r"\1signed char\2"),
]

# The mods' patch targets as the game calls them.
PATCH_TYPE_FIXES = [
    # In place of TMario::walkEnd, which returns BOOL: the console's code
    # (bl walkEnd, then blr) hands walkEnd's whole word back to the game.
    ("src/patches/ystorage.cpp", r"static u8 (patchYStorageWalkEnd\()", r"static int \1",
     "patch targets return what the game reads"),
    # In place of SMSGetAnmFrameRate, whose float the game reads from f1:
    # declared s16, the function converts the rate in f0 and leaves the
    # float the game reads in f1 on the console; natively the result is ax
    # and xmm0 or st(0) holds no f32.
    ("src/patches/sun.cpp", r"static s16 (captureSunData\(\))", r"static f32 \1",
     "patch targets return what the game reads"),
]
ECLIPSE_PATCH_TYPE_FIXES = [
    # In place of TFlagManager::getBool(0x50004); it reads none of its
    # arguments, declared (TNameRef *, u16, const char *).
    ("src/stage/behavior.cpp",
     r"static bool checkForMareGate\(JDrama::TNameRef \*actor, u16 keycode, const char \*name\)",
     r"static bool checkForMareGate(TFlagManager *flags, u32 flag)",
     "patch targets take what the game passes"),
]

# Big-endian data the mods read and write themselves. The scene files (.bin)
# stay big-endian in the port: the game's typed stream reads convert
# (decomp-patches 0013), and its raw multi-byte reads were made big-endian
# reads one by one (endian-08). The mods' objects read their scene
# parameters raw, so natively every float and word came out byte-swapped
# (BetterSunshineEngine's GenericRailObj, ParticleBox, SoundBox and SimpleFog,
# which most Eclipse stages place, and Eclipse's own objects). Their
# multi-byte values are read big-endian here (readBE and readDataBE, which
# SHI_FIXES adds to JSUInputStream); bytes, colours and strings are read as
# they are. Likewise the settings each module saves to the memory card: the
# console writes their words big-endian, and the port keeps card files in the
# console's byte order (the game's own save, endian-11).
def be_reads(path, rx, repl):
    return (path, rx, repl, "big-endian data read and written by the mods: " + path)


ECLIPSE_BE_FIXES = [
    be_reads("src/object/tornado_obj.cpp", r"stream\.read\(&mBlowStrength, 4\)", r"stream.readBE(&mBlowStrength, 4)"),
    be_reads("src/object/blow_wind_obj.cpp",
             r"stream\.read\(&(mStrength|mMode), (sizeof\((?:f32|TBlowWindMapObj::GradientMode)\))\)",
             r"stream.readBE(&\1, \2)"),
    be_reads("src/object/darkness_effect.cpp", r"in\.readData\(&position, sizeof\(TVec3f\)\)",
             r"in.readDataBE(&position, sizeof(TVec3f), sizeof(f32))"),
    be_reads("src/object/darkness_effect.cpp", r"in\.readData\(&(scale|layer_scale), sizeof\(f32\)\)",
             r"in.readDataBE(&\1, sizeof(f32))"),
    be_reads("src/object/button.cpp", r"in\.read\(&(flagID|soundID), 4\)", r"in.readBE(&\1, 4)"),
    be_reads("src/object/launch_star.cpp", r"stream\.read\(&mTravelSpeed, 4\)", r"stream.readBE(&mTravelSpeed, 4)"),
    be_reads("include/settings.hxx", r"in\.read\(&x, 4\);", r"in.readBE(&x, 4);"),
    be_reads("include/settings.hxx", r"out\.write\(&mDarknessValue, 4\);", r"out.writeBE(&mDarknessValue, 4);"),
]
BSE_BE_FIXES = [
    be_reads("src/objects/particle.cpp", r"in\.readData\(&(mID|mSpawnRate|mSpawnScale), 4\)", r"in.readDataBE(&\1, 4)"),
    be_reads("src/objects/fog.cpp", r"in\.readData\(&(mType|mStartZ|mEndZ|mNearZ|mFarZ), 4\)", r"in.readDataBE(&\1, 4)"),
    be_reads("src/objects/generic.cpp",
             r"in\.readData\(&(mBaseRotation\.[xyz]|mFrameRate|mSoundID|mSoundSpeed|mSoundStrength), 4\)",
             r"in.readDataBE(&\1, 4)"),
    be_reads("src/objects/sound.cpp", r"in\.readData\(&(mID|mVolume|mPitch|mSpawnRate), 4\)", r"in.readDataBE(&\1, 4)"),
    be_reads("include/BetterSMS/settings.hxx", r"in\.read\(&(x|f), 4\);", r"in.readBE(&\1, 4);"),
    be_reads("include/BetterSMS/settings.hxx", r"out\.write\(mValuePtr, 4\);", r"out.writeBE(mValuePtr, 4);"),
]

# The frame rate is the port's (frame_rate setting), as for the plain game.
# BetterSunshineEngine replaces TApplication::proc with its own loop, so the
# port's switch to the gameplay rate there never ran and stages stayed at 30;
# the loop now switches it where proc does (sms_port_set_gameplay_frame_rate,
# decomp-patches/modhook-41). BSE's own Frame Rate setting paced the game
# instead: every frame its updateFPS set the display's retrace count (2, or 1
# at 60 FPS, which ran the game at twice its speed while the port stepped it
# for 30) and stored into two retail .sdata2 constants by address, which in
# the port are inside the game's heap. Its patches are waived
# (tools/mods/not_ported.txt) and updateFPS is not run; the setting stays in
# the save, so the card files keep their layout, but is hidden from the menu,
# and getFrameRate reports the port's rate.
FRAME_RATE_FIXES = [
    ("src/application.cpp",
     r"(BETTER_SMS_FOR_CALLBACK bool BetterAppContextDirectStage\(TApplication \*app\) \{\n"
     r"\s*sIsAdditionalMovie = app->checkAdditionalMovie\(\);\n\s*if \(!sIsAdditionalMovie\) \{\n)",
     r'extern "C" void sms_port_set_gameplay_frame_rate(TApplication *app, int gameplay);\n\n'
     r"\1        sms_port_set_gameplay_frame_rate(app, 1);\n",
     "the port's frame rate in stages"),
    ("src/application.cpp", r"\n(\s*)(Application::ContextCallback cb = sContextCBs\[app->mContext\];)",
     r"\n\1sms_port_set_gameplay_frame_rate(app, 0);\n\1\2", "the port's frame rate between stages"),
    ("src/module.cpp", r"\n\s*Game::addLoopCallback\(updateFPS\);", "",
     "BSE's frame rate setting does not pace the port"),
    ("src/p_settings.hxx", r"(\n(\s*)~FPSSetting\(\) override \{\}\n)",
     r"\1\2bool isUnlocked() const override { return false; }\n",
     "BSE's frame rate setting is hidden"),
    ("src/globals.cpp", r"(f32 BetterSMS::getFrameRate\(\) \{\n\s*const f32 FPS = )static_cast<f32>\(30 << gFPSSetting\.getInt\(\)\);",
     r'extern "C" int port_active_frame_rate;\n\1static_cast<f32>(port_active_frame_rate);',
     "BSE reports the port's frame rate"),
]

ECLIPSE_FIXES = optional(TEXTURE_FIXES) + CARD_IMAGE_FIXES + PARTICLE_FIXES + DEBS_FIXES + [RAWDATA_FIX] + BOOL_RET_FIXES + ECLIPSE_BE_FIXES + ECLIPSE_PATCH_TYPE_FIXES + [
    # A retail function taking TVec3f references, called through a (...) cast:
    # on the GameCube an aggregate in a variable argument list is passed by
    # address, so the callee's references see the objects. Pass the addresses.
    ("src/enemy/bowser_car.cpp", r"generate__16TEffectExplosionFRQ29JGeometry8TVec3_f\(explosion, bowser->m_rocket_hit_pos,\s*scale\)",
     r"generate__16TEffectExplosionFRQ29JGeometry8TVec3_f(explosion, &bowser->m_rocket_hit_pos, &scale)",
     "aggregates go by address through (...)"),
    ("src/object/*.cpp", r"(generate__\d+\w+FRQ29JGeometry8TVec3_f\(\w+, )mTranslation, mScale\)",
     r"\1&mTranslation, &mScale)",
     "aggregates go by address through (...)"),
    RAWADDR_FIX,

    # Eclipse's ModuleInfo (main.cpp) links its settings group to the module,
    # but the group is defined in settings.cpp, whose constructor ran after
    # main.cpp's here and cleared the link. The group then took the game's
    # name, "Super Mario Sunshine", in the settings menu and as the name of its
    # card file, which is Eclipse's game save: the game found a one-sector file
    # there and could not save. Made on first use, in initModule, the
    # ModuleInfo comes after the group, and the file is super_mario_eclipse
    # (platform/card/card.cpp moves the old one).
    ("src/main.cpp", r"static BetterSMS::ModuleInfo sModuleInfo\((\"Super Mario Eclipse\", \d+, \d+, &gSettingsGroup)\);",
     r"static BetterSMS::ModuleInfo &moduleInfo() {\n    static BetterSMS::ModuleInfo info(\1);\n    return info;\n}",
     "Eclipse's settings group knows its module"),
    ("src/main.cpp", r"(static void initModule\(\) \{\n)", r"\1    moduleInfo();\n",
     "Eclipse's settings group knows its module"),
    ("src/main.cpp", r"BetterSMS::registerModule\(sModuleInfo\);", r"BetterSMS::registerModule(moduleInfo());",
     "Eclipse's settings group knows its module"),

    # SunshineHeaderInterface named obj_hit_info's third field (May 2026);
    # Eclipse still initialises it by its old placeholder name.
    ("src/*/*.cpp", r"(obj_hit_info\s+\w+\s*=?\s*\{[^}]*?)\._08(\s*=)", r"\1.mVisualOfsY\2",
     "obj_hit_info._08 is mVisualOfsY"),
]
BSE_FIXES = TEXTURE_FIXES + CARD_IMAGE_FIXES + optional([RAWADDR_FIX]) + [RAWDATA_FIX, RAWDATA_REGION_FIX] + BSE_BE_FIXES + PATCH_TYPE_FIXES + [
    # BSE's free-fly camera, in place of CPolarSubCamera::perform's test of
    # TGraphics' first-tick flag (bit 0 of the u16 at 0, `lhz`), reads the
    # word's low byte, the GameCube's second; natively that is the high byte,
    # always 0, so the camera never kept its previous frame or reset its up
    # vector after a cutscene (Delfino Plaza's opening ended rolled).
    ("src/debug/freeflycam.cpp", r"return \(graphics->_00\[1\] & 1\);", r"return (*(u16 *)graphics->_00 & 1);",
     "TGraphics' first-tick flag is the game's u16"),
    # The object table holds pointers, not words.
    ("src/object.cpp", r"sizeof\(u32\) \* ObjDataTableSize\);", r"sizeof(ObjData *) * ObjDataTableSize);",
     "the object table is copied a pointer per entry"),
    # Declared bool, but the game reads the float the function leaves in f1.
    ("src/patches/sun.cpp", r"static bool scaleGlowToLightness\(", r"static f32 scaleGlowToLightness(",
     "the lens glow scale is a float"),
    # As ECLIPSE_FIXES' generate calls: the sun's new position, a Vec
    # reference, goes through a (...) cast, so by address as on the GameCube.
    ("src/patches/sun.cpp", r"JSGSetTranslation__Q26JDrama6TActorFRC3Vec\(sun, reinterpret_cast<Vec &>\(spos\)\)",
     r"JSGSetTranslation__Q26JDrama6TActorFRC3Vec(sun, reinterpret_cast<Vec *>(&spos))",
     "aggregates go by address through (...)"),
    # The memory card banner and icon are built into the code as big-endian
    # BTI files and copied to the card as they are; only their image offset
    # is read, and it has to be read in their byte order.
    ("src/settings.cpp", r"\+ info\.(mBannerImage|mIconTable)->mTextureOffset",
     r"+ __builtin_bswap32(info.\1->mTextureOffset)", "card banner and icon offsets are big-endian"),
    # Run-time rewrites of the retail game's instructions: the port has no
    # retail code, so each goes to the patch registry for the decomp hooks
    # that port it (platform/mods/modhooks.cpp) instead of into memory.
    # TMarioAnimeData::isPumpOK's replacement is PowerPC assembly: the FLUDD
    # animation id against BSE's (extended) animation count.
    ("src/player.cpp",
     r"static SMS_ASM_FUNC void isPumpOk\(\) \{\n\s*SMS_ASM_BLOCK\(\"lhz       3, 2 \(3\)[^;]*\);\n\}",
     r"static bool isPumpOk(const u8 *animeData) {\n    return *(const u16 *)(animeData + 2) < sPlayerAnimeInfosSize;\n}",
     "isPumpOk without assembly"),
    ("src/memory.cpp",
     r"(BETTER_SMS_FOR_EXPORT void BetterSMS::PowerPC::writeU(8|16|32)\(u\d+ \*ptr, u\d+ value\) \{\n)"
     r"\s*\*ptr = value;\n\s*BetterSMS::Cache::store\(ptr, sizeof\(u\d+\)\);",
     r'extern "C" void sms_mod_code_write(uint32_t, uint32_t, int);\n'
     r'\1    sms_mod_code_write((uint32_t)(uintptr_t)ptr, value, \2 / 8);',
     "code writes go to the patch registry"),
    # shadowMarioInitHandler stands in for TEMario::loadAfter's
    # SMS_isMultiPlayerMap call and loads Shadow Mario with PowerPC assembly
    # from the TEMario in r31 (its mEnemyMario, retail offset 0x150); the
    # shim's SMS_ASM_BLOCK is empty, so the player was never set and every
    # stage with Shadow Mario crashed loading (Red Lily). The hook passes the
    # TEMario, and port_shims.cpp reads the member.
    ("src/player.cpp", r'SMS_ASM_BLOCK\("lwz %0, 0x150 \(31\)" : "=r"\(player\)\);',
     'void *emario;\n    SMS_FROM_GPR(31, emario);\n    player = (TMario *)sms_mod_emario_mario(emario);',
     "Shadow Mario's player comes from the TEMario"),
    ("src/player.cpp", r"\n(static bool shadowMarioInitHandler\(\) \{)",
     '\nextern "C" void *sms_mod_emario_mario(void *emario);\n\n\\1',
     "Shadow Mario's player comes from the TEMario"),
    # Console::log and its kin pass their va_list to OSReport as a single
    # argument (as on the console, where the values are garbage too); the
    # port's sms_mod_vreport (sdk_extras.cpp) formats with it.
    ("src/logging.cpp", r'(#include "module.hxx"\n)', r'\1\nextern "C" void sms_mod_vreport(const char *, va_list);\n',
     "BSE's console log formats its arguments"),
    ("src/logging.cpp", r"\bOSReport\(msg, vargs\);", r"sms_mod_vreport(msg, vargs);",
     "BSE's console log formats its arguments"),
] + FRAME_RATE_FIXES
MOVESET_FIXES = optional(TEXTURE_FIXES + [RAWADDR_FIX]) + CARD_IMAGE_FIXES
SHI_FIXES = BOOL_RET_FIXES + SHI_WORD_FIXES + SHI_GAME_TYPES + [
    # The SDK's OSMessage is a void *, as the port's OS keeps it; declared u32,
    # it was half the size in the 64-bit build. BSE's music streamer queues 16
    # of them in a u32 array, which the port wrote 8 bytes apiece, over the
    # streamer's file handle after it (a crash stopping the music on a stage
    # change), and OSReceiveMessage wrote 8 bytes into a 4-byte local. An
    # integer the size of a pointer keeps the mods' integer messages.
    ("include/Dolphin/OS.h", r"\btypedef u32 OSMessage;", "typedef __UINTPTR_TYPE__ OSMessage;",
     "OSMessage is pointer-sized"),
    # MWCC's u32/s32 are (unsigned) long, 64 bits on LP64 hosts: the port
    # spells them int there (src/port_include/dolphin/types.h), and so must
    # the mods, or every u32 field and u32-typed call disagrees with the game.
    ("include/Dolphin/types.h", r"(?<!#else\n)typedef unsigned long u32;\n",
     "#if __SIZEOF_POINTER__ == 8\ntypedef unsigned int u32;\n#else\ntypedef unsigned long u32;\n#endif\n",
     "u32 is 32 bits on LP64 hosts"),
    # Counts SunshineHeaderInterface spells size_t are 32-bit words in the game:
    # keep them 32 bits wide on LP64 hosts (the class layouts are fixed up by
    # shi-layout.patch, but templates are not rewritten there).
    ("include/JSystem/JGadget/List.hxx", r"(?m)(?<!#else\n)^([ \t]*)typedef size_t size_type;\n",
     r"#if __SIZEOF_POINTER__ == 8\n\1typedef u32 size_type;\n#else\n\1typedef size_t size_type;\n#endif\n",
     "list sizes are 32-bit in the game"),
    ("include/SMS/SPC/SpcStack.hxx", r"(?m)(?<!#else\n)^([ \t]*)size_t (mMaxSize|mCurSize);\n",
     r"#if __SIZEOF_POINTER__ == 8\n\1u32 \2;\n#else\n\1size_t \2;\n#endif\n",
     "SunScript stack counts are 32-bit in the game"),
    ("include/SMS/Enemy/SpineBase.hxx", r"(?m)(?<!#else\n)^([ \t]*)size_t (mStackCapacity);\n",
     r"#if __SIZEOF_POINTER__ == 8\n\1u32 \2;\n#else\n\1size_t \2;\n#endif\n",
     "spine stack counts are 32-bit in the game"),
    ("include/Dolphin/types.h", r"(?<!#else\n)typedef long s32;\n",
     "#if __SIZEOF_POINTER__ == 8\ntypedef int s32;\n#else\ntypedef long s32;\n#endif\n",
     "s32 is 32 bits on LP64 hosts"),
    # The decomp's JUTRect has a user-provided copy constructor, so the port
    # passes it by value through a hidden reference; SunshineHeaderInterface's
    # must say so too or J2DFillBox(JUTRect, ...) reads garbage.
    ("include/JSystem/JUtility/JUTRect.hxx", r"(\n(\s*)JUTRect\(\);\n)(?!\s*JUTRect\(const JUTRect)",
     r"\1\2JUTRect(const JUTRect &);\n", "JUTRect is not trivially copyable in the port"),
    # Likewise the game's TVec3<f32> (and so TQuat4<f32>) has a user-written
    # copy constructor: passed and returned through memory, never in registers.
    # A constrained copy constructor (C++20) makes only the f32 one non-trivial,
    # as in the game.
    ("include/JSystem/JGeometry/JGMVec.hxx", r"(\n(\s*)TVec3\(const TVec3 &\) = default;\n)(?!\s*TVec3\(const TVec3 &o\) requires)",
     r"\1\2TVec3(const TVec3 &o) requires(__is_same(T, f32)) : x(o.x), y(o.y), z(o.z) {}\n",
     "TVec3<f32> is not trivially copyable in the port"),
    # The write-gather pipe is not memory natively: the port's proxy forwards
    # stores to its graphics layer.
    ("include/Dolphin/GX.h", r"extern WGPipe volatile wgPipe;", r"#include <sms_mod_wgpipe.h>",
     "wgPipe stores go to the port's graphics layer"),
    # Retail functions called by their CodeWarrior name are casts of their
    # retail addresses, where the port has no code: point each at the port's
    # trampoline of that name (platform/mods/eclipse/rawfn_trampolines.cpp,
    # generated by tools/mods/gen_rawfn.py for the names the mods use).
    # Their int is a pointer-sized integer here: the mods cast what these
    # return to pointers, which must survive on 64-bit hosts. A few casts
    # have no space before the (*) (CLBLinearInbetween_f's f32(*)(...)).
    ("include/SMS/raw_fn.hxx", r"#define\s+(\w+)(\s+)\(\((\w+) ?\(\*\)\(\.\.\.\)\)(0x[0-9A-Fa-f]+)\)",
     lambda m: 'extern "C" void sms_rawfn_%s(void);\n#define %s%s((%s (*)(...))sms_rawfn_%s) /* %s */' % (
         m.group(1), m.group(1), m.group(2), "__INTPTR_TYPE__" if m.group(3) == "int" else m.group(3), m.group(1), m.group(4)),
     "retail functions by name go to the port's functions"),
    # Class sizes SunshineHeaderInterface has wrong, against the retail code.
    # TBossPakkun is 0x1D0 bytes (MarNameRefGen's `new` of it asks __nw for
    # 0x1D0; its constructor and TNerveBPFly use the byte at 0x1CC, the boss
    # music flag): without that byte, TFireyPetey's first member lands on it.
    ("include/SMS/Enemy/BossPakkun.hxx", r"(\n([ \t]*)u32 _13;[^\n]*\n)(\};)",
     r"\1\2s8 _14;\n\3", "TBossPakkun ends at 0x1D0 as in the game"),
    # TMapObjBall is 0x198 bytes (MarNameRefGen's `new` of it): SHI's last
    # member, _198, is TResetFruit's first (its constructor's stfs to 0x198;
    # TMapObjBall's own code never touches 0x198), which the mods' own balls
    # need not reserve.
    ("include/SMS/MapObj/MapObjBall.hxx", r"\n[ \t]*f32 _198;\n", "\n",
     "TMapObjBall ends at 0x198 as in the game"),
    ("include/SMS/MoveBG/ResetFruit.hxx", r"(\n([ \t]*))(u16 _19C;\n)", r"\1f32 _198;\1\3",
     "TMapObjBall ends at 0x198 as in the game"),
    # J3DTevBlock's setters come in pairs whose vtable slots hold the
    # by-pointer one first (__vt__13J3DTevBlock16: setTevKColor
    # FUlPC10J3DGXColor, then FUl10J3DGXColor); SHI declares the by-value one
    # first. On the PowerPC both take an address, so either slot works there;
    # natively the by-value call reached the by-pointer function with the
    # colour as the pointer (Eclipse's TDarkZhine::perform, its switch blocks).
    ("include/JSystem/J3D/J3DMaterial.hxx",
     r"(?m)^([ \t]*virtual void (\w+)\((?![^)]*\*)[^)]*\)[ \t]*= 0;\n)([ \t]*virtual void \2\([^)]*\*[^)]*\)[ \t]*= 0;\n)",
     r"\3\1", "J3DTevBlock's vtable as in the game"),
    # The by-value one takes the port's J3DGXColor, which has a user-written
    # copy constructor and so goes by address: SHI's plain union would go by
    # value. A const reference is passed as the port's function expects.
    ("include/JSystem/J3D/J3DMaterial.hxx", r"(setTevKColor\(s32 idx, )J3DGXColor color\)",
     r"\1const J3DGXColor &color)", "J3DTevBlock's vtable as in the game"),
    # The big-endian reads and writes ECLIPSE_BE_FIXES and BSE_BE_FIXES use:
    # the decomp's readBE (0013, endian-18), with each `width`-byte value
    # (all `size` bytes by default) put in big-endian order around the read,
    # so a failed read keeps it, and its raw readData counterpart.
    ("include/JSystem/JSupport/JSUInputStream.hxx", r"(\n([ \t]*)void read\(void \*, s32\);\n)",
     lambda m: m.group(1) + "".join(m.group(2) + l + "\n" for l in (
         "static void swapBE(void *buf, s32 size, s32 width) {",
         "    u8 *b = (u8 *)buf;",
         "    if (width <= 0)",
         "        width = size;",
         "    for (s32 o = 0; o + width <= size; o += width)",
         "        for (s32 i = 0; i < width / 2; i++) {",
         "            u8 t                 = b[o + i];",
         "            b[o + i]             = b[o + width - 1 - i];",
         "            b[o + width - 1 - i] = t;",
         "        }",
         "}",
         "void readBE(void *buf, s32 size, s32 width = 0) {",
         "    swapBE(buf, size, width);",
         "    read(buf, size);",
         "    swapBE(buf, size, width);",
         "}",
         "void readDataBE(void *buf, s32 size, s32 width = 0) {",
         "    swapBE(buf, size, width);",
         "    readData(buf, size);",
         "    swapBE(buf, size, width);",
         "}")),
     "big-endian stream reads and writes"),
    ("include/JSystem/JSupport/JSUOutputStream.hxx", r"(\n([ \t]*)void write\(const void \*, s32\);\n)",
     lambda m: m.group(1) + "".join(m.group(2) + l + "\n" for l in (
         "void writeBE(const void *buf, s32 size) {",
         "    u8 tmp[8];",
         "    for (s32 i = 0; i < size; i++)",
         "        tmp[i] = ((const u8 *)buf)[size - 1 - i];",
         "    write(tmp, size);",
         "}")),
     "big-endian stream reads and writes"),
    # TParamT<T>::load reads a .prm value, which stays big-endian on disc: the
    # game's definitions (ParamInst.cpp, endian-05) convert it, SHI's inline
    # one reads it raw. Every module defined its own copy of the template;
    # BetterSunshineEngine's were weak and lost to the game's, but the moveset
    # and Eclipse keep theirs private, so the moveset read Luigi's and
    # Piantissimo's better_movement.prm byte-swapped. Declared, the types the
    # game instantiates come from the game; bool and TColor are bytes, read
    # as they are, and stay SHI's.
    ("include/SMS/System/Params.hxx", r"(?s)(\ntemplate <typename T> class TParamRT : public TParamT<T> \{.*?\n\};\n)",
     r"\1\n#include <JSystem/JGeometry/JGMVec.hxx>\n"
     + "".join("extern template void TParamT<%s>::load(JSUMemoryInputStream &);\n" % t
               for t in ("u8", "s16", "u16", "s32", "f32", "JGeometry::TVec3<f32>")),
     "the game's TParamT loads convert .prm values"),
]


def native_flags(storage):
    """Keep the game's numeric flag masks on little-endian native hosts."""
    def fix(match):
        fields = match.group("fields").strip("\n").splitlines()
        # SHI declares these from the word's top bit down for the GameCube.
        # Native compilers allocate from bit 0 up: keeping that order gives
        # mHasFludd 0x10000 instead of the game's MARIO_FLAG_HAS_FLUDD (0x8000).
        # Use the game word's type for every field too: MS bitfield layout
        # separates bool, u8 and u32 allocation units, whereas the game stores
        # all flags in one u32 or u16.
        fields = [re.sub(r"\b(?:bool|u8|u16|u32)\b", storage, field) for field in reversed(fields)]
        return "struct {\n" + "\n".join(fields) + "\n    } " + match.group("name") + ";"
    return fix


# A run of bitfield declarations, as SHI writes them.
FLAG_FIELDS = r"(?P<fields>(?:\s*(?:u8|u16|u32|bool) \w+\s*: \d+;\n)+)\s*"

# Run after shi-layout.patch, which duplicates some of these for the two host
# widths. Keeping this separate also preserves the fix when layouts regenerate.
SHI_NATIVE_FLAG_FIXES = [
    ("include/SMS/Player/Mario.hxx",
     r"struct \{\n(?P<fields>\s*u32 _04\s*: 10;\n(?:\s*(?:u32|bool) \w+\s*: \d+;\n)+)\s*\} (?P<name>mAttributes|mPrevAttributes);",
     native_flags("u32"), "Mario's current and previous flags use the game's numeric masks"),
    # TLiveActor::mLiveFlag: BetterSunshineEngine's and Eclipse's NPC carry
    # tests read mCanBeTaken, the game's LIVE_FLAG_UNK100000 (0x100000), which
    # Shadow Mario sets on Peach before he takes her; natively it was 0x800,
    # so Peach refused him and he tried to take her forever.
    ("include/SMS/Strategic/LiveActor.hxx", r"struct \{\n" + FLAG_FIELDS + r"\} (?P<name>asFlags);",
     native_flags("u32"), "live actors' flags use the game's numeric masks"),
    # TModelWaterManager's u16 at 0x5D60 (mShowShadow 0x100, as the game's
    # unk5D60 &= ~0x100), which Eclipse's darkness sets.
    ("include/SMS/Manager/ModelWaterManager.hxx", r"struct \{\n" + FLAG_FIELDS + r"\} (?P<name>LightType);",
     native_flags("u16"), "the water manager's light flags use the game's numeric masks"),
    # TMarioGamePad::mFlags (u16): mDisable 0x400, mReadInput 0x2, which
    # BetterSunshineEngine sets around its warps and menus.
    ("include/SMS/Player/MarioGamePad.hxx", r"struct \{\n" + FLAG_FIELDS + r"\} (?P<name>mState);",
     native_flags("u16"), "the game pad's flags use the game's numeric masks"),
]


def apply(root, fixes, strict=False):
    changed = 0
    groups = {}
    for fix in fixes:
        pattern, rx, repl, why = fix[:4]
        groups.setdefault(why, [0, len(fix) > 4])
        for path in glob.glob(os.path.join(root, pattern), recursive=True):
            with open(path, encoding="utf-8", errors="surrogateescape") as f:
                text = f.read()
            new, n = re.subn(rx, repl, text)
            if n:
                with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
                    f.write(new)
                changed += n
                groups[why][0] += n
    missed = [why for why, (n, opt) in groups.items() if n == 0 and not opt]
    if strict and missed:
        sys.exit("fixup_sources.py: in %s, nothing matched for: %s\n(the upstream source changed: update the rule)"
                 % (root, "; ".join(missed)))
    return changed


def pristine(root):
    """Back to the fetched revision: every rule then applies to what it was written for."""
    import subprocess
    if os.path.isdir(os.path.join(root, ".git")):
        subprocess.check_call(["git", "-C", root, "checkout", "-q", "-f", "--", "."])
        subprocess.check_call(["git", "-C", root, "clean", "-q", "-f", "-d", "-x"])


def state(roots, files):
    """What the fixed-up sources are made from: the revisions, this script and the patches."""
    import hashlib, subprocess
    h = hashlib.sha1()
    for r in roots:
        if os.path.isdir(os.path.join(r, ".git")):
            h.update(subprocess.check_output(["git", "-C", r, "rev-parse", "HEAD"]))
    for f in files:
        if os.path.exists(f):
            h.update(open(f, "rb").read())
    return h.hexdigest()


def apply_patch(root, patch):
    """Apply a unified diff (paths a/..., b/...) to root, normalising line ends to LF."""
    text = open(patch, encoding="utf-8", errors="surrogateescape").read()
    applied = 0
    for part in re.split(r"(?m)^--- a/", text)[1:]:
        name = part.split("\n", 1)[0].strip()
        path = os.path.join(root, name)
        with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
            cur = f.read().replace("\r\n", "\n").split("\n")
        hunks = re.split(r"(?m)^@@ -(\d+)(?:,\d+)? \+\d+(?:,\d+)? @@.*\n", part)
        pos = 0
        for i in range(1, len(hunks), 2):
            body = hunks[i + 1].split("\n")
            old_l = [l[1:] for l in body if l[:1] in (" ", "-")]
            new_l = [l[1:] for l in body if l[:1] in (" ", "+")]
            k = next((k for k in range(pos, len(cur) - len(old_l) + 1) if cur[k:k + len(old_l)] == old_l), -1)
            if k < 0: raise SystemExit("%s: hunk %d of %s does not apply" % (patch, i // 2 + 1, name))
            cur[k:k + len(old_l)] = new_l
            pos = k + len(new_l); applied += 1
        with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
            f.write("\n".join(cur))
    return applied


if __name__ == "__main__":
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    roots = [os.path.abspath(a) for a in sys.argv[1:]]
    here = os.path.dirname(os.path.abspath(__file__))
    patches = [os.path.join(here, "shi-layout.patch"), os.path.join(here, "mods-port.patch")]
    marker = os.path.join(os.path.dirname(roots[1]), ".sms_port_fixups")
    digest = state(roots, [os.path.abspath(__file__)] + patches)
    if os.path.exists(marker) and open(marker).read().strip() == digest:
        print("fixup_sources: sources already fixed up")
        sys.exit(0)
    if os.path.exists(marker):
        os.remove(marker)  # a run that fails part-way leaves no marker behind
    for r in roots:
        pristine(r)
    n = apply(roots[0], ECLIPSE_FIXES, True) + apply(roots[1], BSE_FIXES, True) + apply(roots[2], SHI_FIXES, True)
    if len(roots) == 4:
        n += apply(roots[3], MOVESET_FIXES, True)
    # SunshineHeaderInterface's classes laid out as the port lays out the game's
    # (tools/mods/shi_layout, which also explains how to regenerate the patch).
    n += apply_patch(roots[2], patches[0])
    n += apply(roots[2], SHI_NATIVE_FLAG_FIXES, True)
    # The mods' own sources where they need more than a pattern: members they
    # address by retail offset (SMS_OFFSET, tools/mods/shi_layout/offsets.py).
    if len(roots) == 4 and os.path.exists(patches[1]):
        n += apply_patch(os.path.dirname(roots[0]), patches[1])
    with open(marker, "w") as f:
        f.write(digest + "\n")
    print("fixup_sources: %d replacements" % n)
