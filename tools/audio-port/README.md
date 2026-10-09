# Pending audio command regression

The PC can run another game frame before JAudio consumes its port commands.
Re-inserting the same intrusive-list node truncates the queue, leaving Wiggler's tempo and pitch ramps at their first step.
The audio patch preserves queue membership and combines dirty parameter flags until the callback consumes them.

Configure the port, then run the asset-free test against the patched sources:

```sh
cmake -S . -B build/linux-64 -DSMS_ARCH=64
tools/audio-port/check.sh build/linux-64 64
cmake -S . -B build/linux-32 -DSMS_ARCH=32
tools/audio-port/check.sh build/linux-32 32
```

The test uses the real JAudio command queue and parameter setter, with interrupt and callback registration stubs.
It also runs the real `outerInit` and `setSePortParameter` functions against a minimal track test double.
It checks delayed music updates between SE commands, preservation of distinct dirty flags, the latest ramp endpoint, queue reuse, and replacement semantics for other U32 arguments.
Additional cases cover all six scalar parameters (volume, pitch, pan, FX mix, Dolby and tempo), one-shot interrupt consumption, and rebinding a queued command without losing its neighbors.
The rebinding case also checks first use on non-zero heap memory.

For an integration check with your extracted disc files, run from the repository root:

```sh
SMS_HEADLESS=1 SMS_SETTINGS=tools/regress/empty-settings.txt \
SMS_TEXTURE_PACKS=0 SMS_SKIP_MOVIES=1 SMS_AUDIO_OUT=null \
SMS_VI_DETERMINISTIC=1 SMS_QUIET_STUBS=1 SMS_SAVE_DIR=/tmp/wiggler-audio-save \
SMS_WARP=4,2,1 SMS_FRAME_RATE=60 \
SMS_AUTOPRESS='START@1400,STICK_LEFT@2000,A@2400,STICK_LEFT@3100,A@3250,A@3450' \
gdb -q -batch -nx -ex 'set debuginfod enabled off' -ex 'set confirm off' \
    -ex 'handle SIG34 nostop noprint' -ex 'source tools/audio-port/wiggler.py' \
    -ex run --args build/linux-64/sms /path/to/disc/files
```

Repeat with `build/linux-32/sms` for the 32-bit executable.
The fixture injects Wiggler's recovery states at two and one remaining HP, then the death state.
It checks the real audio thread's hit tempos (1.07894 and 1.15789), the initial defeat pitch rise, the subsequent tempo/pitch slowdown, and the stopped music handle.
It exits with status 1 on a failed assertion and prints `wiggler audio: PASS` on success.
This checks the existing fight-to-audio paths; player attacks and boss animations are outside its coverage.
