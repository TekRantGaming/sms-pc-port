# Puffers exploding before reaching Petey

The reported 120 fps failure is a self-collision, rather than an accelerated flight timer.
`TPopo::calcRootMatrix` updates the auxiliary `TPopoCollision` position from the previous center-joint matrix on each displayed frame.
At higher frame rates, that position trails the flying Puffer by less distance.
Once `TNervePopoFly` enables the body's collisions, the auxiliary hitbox can overlap its owner.
Its `checkHit` then calls the owner's `isCollidMove` with the owner itself as the target.
The Puffer sends itself `HIT_MESSAGE_TRAMPLE`, accepts the message, and enters `TNervePopoExplosion`.

A gdb probe in Bianco Hills episode 5 reproduced the actual overlap at 120 fps after nine flight updates.
Both `this` and the collision target were the same Puffer, followed by `TSmallEnemy::receiveMessage` with that same sender and message 0.
The corresponding 30 fps launch did not generate the self-hit.

`decomp-patches/framerate-39-Popo-self-collision.patch` ignores only `param_1 == this` in `TPopo::isCollidMove` on the PC port.
Other collision targets still receive the existing message, and accepted hits still explode the Puffer.
The upstream decompilation is unchanged.

Run the regression after building both word sizes:

```sh
python3 tools/regress/popo.py --disc /path/to/GMSE01.iso
```

An extracted disc `files/` folder is also accepted.
The check runs at 30, 60, and 120 fps in each word size.
It attaches a real Puffer to FLUDD through a debugger fixture, charges and releases it through the game's trigger logic, and lifts it clear of the terrain.
After 20 flight updates, including an injected auxiliary self-hit, an injected collision with the real sleeping Petey must wake him and explode the Puffer.
The self-hit assertion fails on the executable before the patch.
This exercises the actual game code and collision response, rather than completing a manually played boss fight.

## Broader self-collision audit

Higher frame rates can expose other gameplay dependencies on animation or draw updates.
For this particular failure, both an overlapping auxiliary hitbox and an owner forwarding damage to itself are needed.
The shared collision grid compares distinct actors; it does not understand which hitboxes belong to which owner.
Ordinary actors are checked before insertion into each grid cell, so the normal registration path does not compare a standalone actor with itself.
A global owner filter would need explicit ownership information and evidence that the filtered interactions are unwanted.

Review of the 25 out-of-line `isCollidMove` handlers and auxiliary hitbox dispatch found no second confirmed instance of the Puffer failure.
The closest forwarding hitbox is the Cataquack's `TPoiHanaCollision`, whose attack mask selects Mario rather than its enemy owner.
Rockets restrict damage targets to bosses and a specific projectile type, excluding themselves.
Fire Chain Chomps and water striders already exclude themselves explicitly.
Other boss hitboxes reviewed restrict their interactions to Mario or specific map objects.
Some projectile handlers can explode on another projectile of the same type; the absence of a Puffer-like auxiliary forwarding hitbox matters there.

`tools/regress/self_damage.py` is a reusable runtime diagnostic for this class of bug:

```sh
python3 tools/regress/self_damage.py --disc /path/to/GMSE01/files
python3 tools/regress/self_damage.py --disc /path/to/GMSE01/files --fixture popo --arch 32,64 --fps 120
```

It discovers source-defined message receivers and collision handlers, attaches to their out-of-line entries, and stops with a backtrace on self-directed damage-like messages.
It also counts self-target collision requests, which may be harmless when a handler rejects them.
Each log lists unavailable symbols, observed functions, call counts and the gameplay duration.
The default samples Bianco Hills episode 5 and episode 1 of Ricco Harbor, Gelato Beach, Pinna Park, Sirena Beach and Noki Bay for eight gameplay seconds each at 30 and 120 fps in the 64-bit build.
Use `--arch 32,64`, `--seconds`, and `--scenes 'stage,episode;stage,episode'` to expand the samples.

The detector caught the original self-directed trample in the pre-fix executable.
The fixed Puffer fixture passed with the detector active in both word sizes at 120 fps.
All twelve default stage/rate samples completed without a self-damage report.
Five samples generated 2,115 observed calls across Puffer and Cataquack collision handlers and the small-enemy message receiver.
The other seven samples generated no observed calls and provide no combat coverage.
Short stage samples are an observation of the contacts that occur, not a full boss or enemy coverage test.
Inlined calls are outside the detector's coverage, and some starting locations generate no combat contacts.
This diagnostic addresses self-collision and self-damage; timing, movement and animation regressions still need their own behavior checks.
