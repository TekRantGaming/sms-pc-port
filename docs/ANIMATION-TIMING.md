# High frame rate animation audit

Custom animation rates expressed as frames per native 30 Hz display frame need a `30 / active_fps` conversion when the controller updates once per displayed frame.
The default MActor rates already use `SMSGetAnmFrameRate()` and need no second conversion.
Controllers advanced by the director's 120 Hz movement ticks also need no display-rate conversion.

`framerate-40-native-animation-rates.patch` corrects these overrides:

| Path | Correction |
| --- | --- |
| Petey's `TBossPakkun::changeBck` | Scale the configured goo wind-up rate; animation-driven mouth opening and completion retain native timing. |
| Mecha Bowser's `TLimitKoopa::changeBck` | Scale the supplied rates for idle, turning, fire, damage and recovery animations. |
| Hinokuri's animation selection | Scale both its configured level-zero walk rate and the fixed rate used for other animations. |
| Blooper's beak-damage sequence | Scale the explicitly assigned BTK rate. Frozen BTP controllers stay frozen. |
| Wiggler's walking animation | Scale the minimum rate as well as the already-scaled requested rate, so the clamp cannot restore a native per-frame speed at higher FPS. |
| Script `linSetAnmRate` | Scale explicit BCK and BTP rates supplied by actor scripts. |

The audit reviewed the game's explicit `setRate`/`setFrameRate` writes and their update paths.
Mario and Yoshi's animation updates are called from `TMario::perform`'s movement pass, so their fixed rates retain the 120 Hz simulation clock.
Resume rates copied from existing controllers, zero rates, particle emission rates and rates already derived from `SMSGetAnmFrameRate()` are preserved.
Petey's motion blending also advances on the movement pass and is preserved.
This review covers explicit animation-rate overrides; it is not a claim that every gameplay mechanic has been tested at every frame rate.

## Verification

`tools/framerate/check.sh` checks equal one-second animation advancement at 30, 60 and 120 FPS for normal, frozen and reverse custom rates in both word sizes.

The gameplay fixture selects the normal vomit nerve on the full Petey in Bianco Hills episode 5, reads the configured rate from his loaded parameters and measures actual MActor frame advancement over one simulated second.
It does not inject a playback rate.
With this disc's configured rate of 1.6, the old executable retains 1.6 at 60 and 120 FPS, where the expected rates are 0.8 and 0.4.
The corrected executable advances 47.999989, 47.999973 and 48.000038 animation frames per second at 30, 60 and 120 FPS respectively.

```sh
python3 tools/regress/animation_timing.py --disc /path/to/GMSE01/files
python3 tools/regress/animation_timing.py --fixture duck --fps 60,120 --disc /path/to/GMSE01/files
```

The duck fixture selects the state used after a stomp on a stunned duck and requires its ordinary dizzy emitter to reach the renderer with visible particles at finite positions.
It passed at 60 and 120 FPS on the current code.
The reported missing duck effect was not reproduced; the existing `framerate-36` particle stepping correction remains in place.
The fixture tests the effect path, rather than a full player-input stomp sequence.
