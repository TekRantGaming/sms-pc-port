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
RAWDATA = "0x803ACA68|0x803ACAB0|0x803AFB48|0x803DFA00|0x8040DAB4|0x8040DABC|0x8040DFD4|0x8040DFE4|0x8040DFF4|0x8040E03C|0x8040E0BC|0x8040FA90"
RAWDATA_FIX = ("src/**/*.cpp", r"(\(\s*(?:const\s+)?[A-Za-z_][\w:<> ]*?\s*\*+\s*\))\s*(?i:(" + RAWDATA + r"))\b",
               lambda m: "%ssms_mod_rawdata(%s)" % (m.group(1), m.group(2)), "retail data addresses go to the port's objects")

# Textures built into the code as byte arrays are converted to host byte
# order in place when the game first stores them (JUTTexture::storeTIMG), so
# they cannot be read-only; static keeps the internal linkage const gave them.
# (The memory card banner and icon are declared extern and never stored.)
TEXTURE_FIXES = [
    (glob_, r"(?<!static )\bconst u8 SMS_ALIGN\(32\) (?!gSaveBnr\b|gSaveIcon\b)(\w+)\[\]",
     r"static u8 SMS_ALIGN(32) \1[]", "embedded textures are converted in place")
    for glob_ in ("src/**/*.cpp", "src/**/*.hxx", "include/**/*.hxx")
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

ECLIPSE_FIXES = optional(TEXTURE_FIXES) + PARTICLE_FIXES + DEBS_FIXES + [RAWDATA_FIX] + BOOL_RET_FIXES + [
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

    # SunshineHeaderInterface named obj_hit_info's third field (May 2026);
    # Eclipse still initialises it by its old placeholder name.
    ("src/*/*.cpp", r"(obj_hit_info\s+\w+\s*=?\s*\{[^}]*?)\._08(\s*=)", r"\1.mVisualOfsY\2",
     "obj_hit_info._08 is mVisualOfsY"),
]
BSE_FIXES = TEXTURE_FIXES + optional([RAWADDR_FIX]) + [RAWDATA_FIX] + [
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
]
MOVESET_FIXES = optional(TEXTURE_FIXES + [RAWADDR_FIX])
SHI_FIXES = BOOL_RET_FIXES + SHI_WORD_FIXES + [
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
    # The mods' own sources where they need more than a pattern: members they
    # address by retail offset (SMS_OFFSET, tools/mods/shi_layout/offsets.py).
    if len(roots) == 4 and os.path.exists(patches[1]):
        n += apply_patch(os.path.dirname(roots[0]), patches[1])
    with open(marker, "w") as f:
        f.write(digest + "\n")
    print("fixup_sources: %d replacements" % n)
