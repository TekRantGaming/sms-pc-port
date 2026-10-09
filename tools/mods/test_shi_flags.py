#!/usr/bin/env python3
"""Check fixed-up SHI flag words against the game's masks.

    python3 tools/mods/test_shi_flags.py BUILD/eclipse-src/shi [--compiler clang++]

SunshineHeaderInterface declares these flag words as bitfields from the top
bit down, as the GameCube allocates them; fixup_sources.py reverses them for
native hosts. Tests every generated declaration on both host widths,
including Microsoft's bitfield allocation rules used by Windows compilers.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


MARIO = {
    "mIsPerforming": "MARIO_FLAG_IS_PERFORMING",
    "mIsShineShirt": "MARIO_FLAG_HAS_SHIRT",
    "mIsWater": "MARIO_FLAG_IN_WATER",
    "mIsShallowWater": "MARIO_FLAG_IN_SHALLOW_WATER",
    "mHasFludd": "MARIO_FLAG_HAS_FLUDD",
    "mIsFluddEmitting": "MARIO_FLAG_FLUDD_EMITTING",
    "mGainHelmet": "MARIO_FLAG_HELMET",
    "mGainHelmetFlwCamera": "MARIO_FLAG_HELMET_FLW_CAMERA",
    "mIsGroundPoundSitUp": "MARIO_FLAG_GROUND_POUND_SIT_UP",
    "mIsGameOver": "MARIO_FLAG_GAME_OVER",
    "mLeftRecentWater": "MARIO_FLAG_RECENTLY_LEFT_WATER",
    "mTalkingNPC": "MARIO_FLAG_NPC_TALKING",
}

# TLiveActor::mLiveFlag (decomp include/Strategic/LiveActor.hpp, GMSE01).
LIVE = {
    "mIsSunken": "0x1000000",      # LIVE_FLAG_UNK1000000
    "mIsRide": "0x400000",         # LIVE_FLAG_UNK400000
    "mCanBeTaken": "0x100000",     # LIVE_FLAG_UNK100000, set by TEnemyMario::tryTake
    "mCanTalk": "0x20000",         # LIVE_FLAG_UNK20000
    "mCullModel": "0x100",         # LIVE_FLAG_UNK100
    "mHasPhysics": "0x40",         # LIVE_FLAG_UNK40
    "mClipFromScene": "0x10",      # LIVE_FLAG_UNK10
    "mIsObjDead": "0x1",           # LIVE_FLAG_DEAD
}

# TModelWaterManager::unk5D60 (u16; the game clears 0x100 to hide shadows).
LIGHT = {"mMaskObjects": "0x200", "mShowShadow": "0x100"}

# TMarioGamePad::mFlags (u16).
PAD = {"mDisable": "0x400", "mIsTalking": "0x40", "mReadInput": "0x2"}

FIELDS = r"((?:\s*(?:u8|u16|u32|bool) \w+\s*: \d+;\n)+)\s*"

# header, declaration names (one per generated layout), storage type, masks
WORDS = [
    ("SMS/Player/Mario.hxx", ["mAttributes", "mAttributes", "mPrevAttributes"], "u32", MARIO),
    ("SMS/Strategic/LiveActor.hxx", ["asFlags"], "u32", LIVE),
    ("SMS/Manager/ModelWaterManager.hxx", ["LightType"], "u16", LIGHT),
    ("SMS/Player/MarioGamePad.hxx", ["mState", "mState"], "u16", PAD),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shi", type=Path)
    parser.add_argument("--compiler", default="c++")
    parser.add_argument("--arch", nargs="+", type=int, choices=(32, 64), default=(32, 64))
    args = parser.parse_args()
    game_flags = Path(__file__).resolve().parents[2] / "decomp/include/Player/MarioFlags.hpp"
    code = ['#include <cassert>', '#include <cstring>', f'#include "{game_flags}"',
            'using u8 = unsigned char;', 'using u16 = unsigned short;', 'using u32 = unsigned int;']
    body = []
    n = 0
    for header, names, storage, masks in WORDS:
        text = (args.shi / "include" / header).read_text()
        found = re.findall(r"struct \{\n" + FIELDS + r"\} (" + "|".join(set(names)) + r");", text)
        assert [name for _, name in found] == names, f"{header}: expected {names}, found {[n for _, n in found]}"
        for fields, name in found:
            code.append(f"struct Flags{n} {{\n{fields}\n}};")
            code.append(f"static_assert(sizeof(Flags{n}) == sizeof({storage}), \"{header} {name}\");")
            for field, mask in masks.items():
                body.extend([
                    "{", f"Flags{n} flags{{}}; {storage} bits = 0;",
                    f"flags.{field} = true;",
                    "std::memcpy(&bits, &flags, sizeof(bits));",
                    f"assert(bits == {mask});",
                    f"bits = {mask};",
                    "std::memcpy(&flags, &bits, sizeof(bits));",
                    f"assert(flags.{field});",
                    # Entry/exit must change just this bit, preserving other flags.
                    f"bits = {storage}(~0u);",
                    "std::memcpy(&flags, &bits, sizeof(bits));",
                    f"flags.{field} = false;",
                    "std::memcpy(&bits, &flags, sizeof(bits));",
                    f"assert(bits == {storage}(~{storage}({mask})));", "}",
                ])
            n += 1
    code += ["int main() {"] + body + ["}"]
    with tempfile.TemporaryDirectory(prefix="sms-shi-flags-") as directory:
        work = Path(directory)
        source, binary = work / "check.cpp", work / "check"
        source.write_text("\n".join(code) + "\n")
        for arch in args.arch:
            for ms_layout in (False, True):
                options = [f"-m{arch}"] + (["-mms-bitfields"] if ms_layout else [])
                subprocess.run([args.compiler, "-std=c++17", *options,
                                str(source), "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True)
                print(f"PASS: {arch}-bit {'MS' if ms_layout else 'native'} bitfields, {n} flag words")


if __name__ == "__main__":
    main()
