# Online co-op: plan

Goal: play [Better Super Mario Sunshine Online](https://github.com/Daytendo64/Better-Super-Mario-Sunshine-Online-BSMSO-) (BSMSO, GPL-3.0) co-op on the native port, chosen from the launcher, with the port's PC options and frame rate.

## How BSMSO works

```
BSMSO launcher (C#/.NET) <-> TCP/UDP relay server (C#) <-> other launchers
        | ReadProcessMemory / WriteProcessMemory
        v
   Dolphin: game RAM, mailbox at 0x817FC000 (CommBuffer, 5493 bytes, big-endian)
        ^
   _BSMSO.kxe: about 28k lines of C++ built for PowerPC on Better Sunshine Engine (BSE)
```

The game-side module copies the local Mario's state into the mailbox, and draws remote players, applies world and flag sync and runs Hide & Seek from what the launcher writes back.
The launcher does the networking, interpolation and session logic.

## What carries over

- **Game memory addresses.** The port maps MEM1 at `0x80000000` in its own process, so the mailbox can sit at the same `0x817FC000`.
- **Launcher, server and protocol.** If the native module writes the mailbox big-endian, BSMSO's networking, server and UI stay as they are. Only `SMSO.Bridge/DolphinBridge.cs` changes: it attaches to `sms.exe` and reads the guest address directly, with no Dolphin RAM base.
- **The game-side module.** It must be ported. It is written against BSE's own headers (`SMS/*.hxx`, `raw_fn` calls at retail addresses, Kuribo module hooks), not the decomp's classes. Each use becomes a decomp class or member, or an `SMS_MOD_SITE` hook like those already in `decomp-patches/modhook-*`.

## Milestones

1. **Presence.** Port `comm_buffer`, `module` and `remote_mario` in reduced form. Export the local Mario (position, angle, animation, FLUDD) and draw remote Marios as stock `TMario` puppets. Adapt the bridge. Test: two PCs see each other move in Delfino Plaza.
2. **Gameplay sync.** `world_sync`, `story_flag_sync`, `red_coin_sync`, `fruit_sync`, `npc_sync`, `yoshi_sync`, `remote_water_sync`, voice and audio. Shines and blue coins are shared.
3. **Modes and polish.** Hide & Seek, name tags, the connection HUD, custom character packs (BSMSO's assets, installed from its release), and warps.
4. **Launcher.** An Online page: host or join (address, port 27015, name), start the bundled bridge and server, then Play. Linux needs a cross-platform bridge: the C# code runs on .NET 7+, and only the memory access is Win32. It can be replaced by an in-process client, since the module lives in the same process.

## Licensing

BSMSO and BSE are GPL-3.0. Shipping a port that includes their code makes the combined work GPL-3.0. The fork must then publish its source under GPL-3.0 and keep BSMSO's notices and credits. The upstream port has no LICENSE file, so its authors' terms should be confirmed too.
