# Online co-op

Players connect from the launcher's **Online** page (`net_mode`, `net_address`, `net_port`, `net_name` in `settings.txt`). One player hosts and up to seven join. Each player sees the others in the same level and episode, with name tags. Everyone plays their own game: levels, enemies and progress are not shared yet.

The port has its own netcode, written for the port. It does not reuse [Better Super Mario Sunshine Online](https://github.com/Daytendo64/Better-Super-Mario-Sunshine-Online-BSMSO-) (BSMSO), for three reasons:

- BSMSO's protocol is gated on its build number, which changes with every release.
- Its launcher and memory bridge run only on Windows.
- Its game module is written for PowerPC against Better Sunshine Engine.

BSMSO's design (60 Hz state snapshots and remote Mario puppets) was the model.

## How it works

**Network** (`platform/netplay/netplay.cpp`, host code)
- UDP on a background thread.
- The host relays every player's pose to everyone else, about 60 times a second.
- Packets start with `"SMSN"`, a protocol version and a type: HELLO/WELCOME/REJECT to join, POSE from each player, STATE from the host (every other player's slot, name and pose), and BYE.
- A player silent for five seconds is dropped. A joining player keeps retrying until the host answers.

**Pose** (`NetPose`). Everything needed to draw a Mario as he looks on his player's screen:
- the body's base matrix after `calcBaseMtx`;
- both animation layers (body and upper body), each with its BCK indices, frame and blend ratio;
- the face texture pattern;
- the FLUDD nozzle;
- the stage and episode.

Sending the finished pose inputs, rather than inputs or physics state, makes the remote Mario look exactly right in every state (running, swimming, poles, cutscenes) with no game logic on the receiving side.

**Puppets** (`platform/netplay/net_game.cpp`, built into the game library)
- A remote Mario is a set of `J3DModel`s made from the local Mario's own model data: body, hands, cap, FLUDD and nozzle.
- Each has its own frame controls and layer state. The animation tables and blend calculators are shared, and the remote's blend ratios are set around its calc.
- `decomp-patches/zzz-pc-netplay.patch` calls into it from `TMario::perform`:
  - after `calcAnim`: publish the local pose, then pose the puppets;
  - after `calcView`: view-calc them, and compute name tags through the game camera;
  - after entry: enter them into the same draw buffers as the local Mario, so they are lit the same.
- The models live on the stage's heap and are rebuilt in each new stage.

**Name tags.** These are projected from the head joint with the game camera's field of view, and drawn over the presented frame by `platform/gx` (`GXPC_SetNameTags`). They follow the renderer's widescreen and letterbox.

## Next

1. **Shared progress.** Shine Sprites, blue coins and story flags collected by one player are applied for everyone (`TFlagManager`), with the host as the authority.
2. **Warps and lobby.** Follow the host into a level, and see a player list with where everyone is.
3. **Smoother remote motion.** Interpolate between snapshots for internet latency, and add shadows under remote Marios.
4. **Yoshi, water and sounds.** Draw a remote player's Yoshi and spray, and play their sounds positionally.
5. **Hands.** Hand model visibility lives on shared model data, so remote Marios currently show the local Mario's hand pose.
