# Decompilation status

Target: `GMSE01`. Decomp checkout: `5e7b48ca4352985439b73fe636a490d505474735`.
Build report generated: 2026-09-29T16:06:06+00:00.
Port decomp pin: `5e7b48ca4352985439b73fe636a490d505474735`.
Completion: **incomplete**.

| Category | Exact code | Source-linked code | Source-linked units |
| --- | ---: | ---: | ---: |
| Game Code | 67.65962% | 20.26703% | 207 / 385 |
| JSystem Middleware | 93.90541% | 81.21971% | 186 / 198 |
| SDK Code | 99.71085% | 99.54403% | 148 / 149 |
| All | 73.57174% | 34.33548% | 541 / 732 |

Exact functions: 12,047 / 12,904.
Exact data: 99.84277%.
Code outside exact functions: 952,408 bytes.
Units awaiting source linking: 191.

Rebuilt DOL SHA-1: `a6782903ef79d4196c8489ecb1b57decb5b3728f`.
Original DOL hash verification: **PASS**.

A matching DOL can still contain original binary objects.
Completion requires closing and source-linking every remaining unit; the DOL hash alone is insufficient.

## Closest units to source linking

Use this fresh ranking to choose experiments and compare source directly with the original binary.
Historical notes can supply hypotheses; new compiler and binary evidence determines the result.

| Unit | Non-exact functions | Code outside exact functions | Exact data |
| --- | ---: | ---: | ---: |
| `M3DUtil/MActorData` | 1 | 100 B | 100.00000% |
| `THPPlayer/THPAudioDecode` | 1 | 176 B | 100.00000% |
| `System/PerformList` | 1 | 216 B | 100.00000% |
| `Player/MarioParticle` | 1 | 388 B | 100.00000% |
| `JSystem/JDrama/JDREfbSetting` | 1 | 404 B | 100.00000% |
| `Map/MapWireManager` | 1 | 424 B | 100.00000% |
| `MoveBG/MapObjTree` | 1 | 432 B | 100.00000% |
| `Strategic/MirrorActor` | 1 | 508 B | 100.00000% |
| `NPC/NpcCollision` | 1 | 536 B | 100.00000% |
| `JSystem/JDrama/JDRFrmGXSet` | 1 | 548 B | 100.00000% |
| `MSound/MAnmSound` | 1 | 600 B | 100.00000% |
| `MarioUtil/MathUtil` | 1 | 676 B | 100.00000% |
| `Enemy/wireBinder` | 1 | 696 B | 100.00000% |
| `MoveBG/MapObjFlag` | 1 | 696 B | 100.00000% |
| `TRK_MINNOW_DOLPHIN/debugger/embedded/MetroTRK/Portable/support` | 1 | 728 B | 100.00000% |
| `JSystem/JKernel/JKRExpHeap` | 1 | 736 B | 100.00000% |
| `MoveBG/MapObjCloud` | 1 | 772 B | 100.00000% |
| `Camera/CameraDemo` | 1 | 852 B | 100.00000% |
| `NPC/NpcWalkTurn` | 1 | 992 B | 100.00000% |
| `Enemy/enemymanager` | 1 | 1,012 B | 100.00000% |
| `Enemy/BossHanachanEffect` | 1 | 1,044 B | 100.00000% |
| `NPC/NpcCallback` | 1 | 1,064 B | 100.00000% |
| `Enemy/BathtubBinder` | 1 | 1,148 B | 100.00000% |
| `Camera/CameraNormal` | 1 | 1,204 B | 100.00000% |
| `MoveBG/MapObjItem2` | 1 | 1,300 B | 100.00000% |
| `Enemy/BathtubPeach` | 1 | 1,324 B | 100.00000% |
| `Camera/lensglow` | 1 | 1,328 B | 100.00000% |
| `Enemy/spider` | 1 | 1,396 B | 100.00000% |
| `JSystem/JAudio/JALibrary/JALModSe` | 1 | 1,484 B | 100.00000% |
| `Camera/lensflare` | 1 | 1,512 B | 100.00000% |

Regenerate after a successful build:

```sh
python3 tools/decomp_status.py --decomp ../sms --markdown docs/DECOMP_STATUS.md --queue build/decomp-remaining.tsv
```
