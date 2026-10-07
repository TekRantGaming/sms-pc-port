#!/usr/bin/env python3
"""Check fixed-up SHI Mario flags against the game's masks.

    python3 tools/mods/test_mario_flags.py BUILD/eclipse-src/shi [--compiler clang++]

Tests every generated current/previous flag declaration on both host widths,
including Microsoft's bitfield allocation rules used by Windows compilers.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


MASKS = {
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shi", type=Path)
    parser.add_argument("--compiler", default="c++")
    parser.add_argument("--arch", nargs="+", type=int, choices=(32, 64), default=(32, 64))
    args = parser.parse_args()
    header = (args.shi / "include/SMS/Player/Mario.hxx").read_text()
    declarations = re.findall(
        r"struct \{\n((?:\s*(?:u32|bool) \w+\s*: \d+;\n)+)\s*\} (mAttributes|mPrevAttributes);",
        header,
    )
    assert [name for _, name in declarations] == [
        "mAttributes", "mAttributes", "mPrevAttributes"
    ], "expected both generated host layouts and the previous flag word"
    game_flags = Path(__file__).resolve().parents[2] / "decomp/include/Player/MarioFlags.hpp"
    code = ['#include <cassert>', '#include <cstring>',
            f'#include "{game_flags}"', 'using u32 = unsigned int;']
    for i, (fields, _) in enumerate(declarations):
        code.append(f"struct Flags{i} {{\n{fields}\n}};")
        code.append(f'static_assert(sizeof(Flags{i}) == sizeof(u32));')
    code.append("int main() {")
    for i in range(len(declarations)):
        for field, mask in MASKS.items():
            code.extend([
                "{", f"Flags{i} flags{{}}; u32 bits = 0;",
                f"flags.{field} = true;",
                "std::memcpy(&bits, &flags, sizeof(bits));",
                f"assert(bits == {mask});",
                f"bits = {mask};",
                "std::memcpy(&flags, &bits, sizeof(bits));",
                f"assert(flags.{field});",
                # Entry/exit must change just this bit, preserving other flags.
                "bits = ~u32(0);",
                "std::memcpy(&flags, &bits, sizeof(bits));",
                f"flags.{field} = false;",
                "std::memcpy(&bits, &flags, sizeof(bits));",
                f"assert(bits == ~u32({mask}));", "}",
            ])
    code.append("}")
    with tempfile.TemporaryDirectory(prefix="sms-mario-flags-") as directory:
        work = Path(directory)
        source, binary = work / "check.cpp", work / "check"
        source.write_text("\n".join(code) + "\n")
        for arch in args.arch:
            for ms_layout in (False, True):
                options = [f"-m{arch}"] + (["-mms-bitfields"] if ms_layout else [])
                subprocess.run([args.compiler, "-std=c++17", *options,
                                str(source), "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True)
                print(f"PASS: {arch}-bit {'MS' if ms_layout else 'native'} bitfields")


if __name__ == "__main__":
    main()
