# platform/audio — software DSP and audio output

SMS's audio driver (JAudio, in the decomp) runs unchanged on the emulated CPU.
This module replaces the hardware it talks to: the DSP microcode that mixes voices (a software mixer that reads the same voice blocks the game writes), and the AI DMA engine that plays the mixed blocks (SDL2 output at 32 kHz).
`PROTOCOL.md` documents the mail protocol, the voice-block layout and the evidence behind each choice.

| File | Owner | What |
| --- | --- | --- |
| `dsp_mixer.{h,cpp}` | audio | The mixer: ADPCM4/ADPCM2/PCM8/PCM16 from ARAM, DirectPCM stream rings from main memory, 4-tap polyphase resampling with the game's `DSPRES_FILTER`, per-send Q15 volume ramps, the auto mixer, the four FX lines, the master level. No game headers. |
| `dsp_hle.cpp` | audio | The DSP task's side of the mail protocol: setup (0x81), sync frame (0x82), release-halt → render one subframe → `0xF355FF00` through `__DSPHandler`. The SDK-named `DSP*` functions are built only with `SMS_AUDIO_DSP_HLE` (see Integration). |
| `ai.cpp` | audio | `AI*`: DMA latch/callback emulation, SDL2 output (loaded with `dlopen`), host-clock pacing without a device, WAV recording. |
| `noaudio.cpp` | bring-up lead | `SMS_NO_AUDIO` (empty sound configuration). |
| `tests/` | audio | `audio_test`: offline mixer test against the disc; `mixer_regression`: asset-free reverb and release checks (not part of the CMake build). |
| `../../decomp-patches/audio-01-*.patch`, `audio-02-*.patch` | audio | JAudio bitfield/byte views that assumed big-endian layout (mix-config bus numbers, BMS note-on flags). |
| `../../decomp-patches/audio-03-*.patch` | audio | No host-time "DSP overload" voice stealing on TARGET_PC. |

## Integration (for the bring-up lead)

The AI layer is already live: `ai.cpp`'s strong `AI*` definitions replace the weak stubs, so JAudio's DAC loop now runs on its real clock.
With the current handshake-only fake DSP, that loop outputs silence.
`SMS_AUDIO=0` drops the output but keeps that loop running (see Environment).

To switch to the software DSP (two edits, both in your files):

1. In `platform/misc/sdk_data.cpp`, remove or `#ifndef SMS_AUDIO_DSP_HLE` the fake DSP block: the `__DSP_curr_task`/`__DSP_first_task`/`__DSP_last_task` globals, the mail queue, `DSPCheckMailFromDSP`, `DSPReadMailFromDSP`, `DSPCheckMailToDSP`, `DSPSendMailToDSP`, `DSPAssertInt`, `DSPInit`, `__DSP_boot_task`, `__DSP_insert_task`, `__DSP_exec_task`, `__DSP_remove_task`.
   `dsp_hle.cpp` defines the same set.
2. Add `SMS_AUDIO_DSP_HLE=1` to the `sms` target's compile definitions in `CMakeLists.txt`.

If you would rather keep your functions, forward to the hook instead.
Each of these is `extern "C"` in `dsp_hle.cpp` and always built:

```c
void port_audio_dsp_boot(DSPTaskInfo* task);   /* from __DSP_boot_task, after setting __DSP_curr_task */
u32  port_audio_dsp_check_mail_from(void);     /* DSPCheckMailFromDSP */
u32  port_audio_dsp_read_mail_from(void);      /* DSPReadMailFromDSP */
void port_audio_dsp_mail_to(u32 mail);         /* DSPSendMailToDSP */
void port_audio_dsp_assert_int(void);          /* DSPAssertInt */
/* DSPCheckMailToDSP must return 0 (mail is consumed at once). */
```

Replies are delivered with `port_irq_defer` → the decomp's `__DSPHandler`.
That means the handshake, the setup acknowledgement and every subframe reach the game at the next interrupt check point, which is `OSRestoreInterrupts` at the end of `DSPSendCommands2`.
The busy-wait loops in `DsetupTable` and `DspBoot` rely on this.
Nothing else in the OS layer is needed.
The ARAM lookup uses `port_aram_ptr` from `platform/ar`.

This was verified by linking a private binary with `-DSMS_AUDIO_DSP_HLE=1` and `-Wl,--allow-multiple-definition` (audio objects first) against the `build/linux-32/` game library.
It boots, and the logo, UI sounds, sequences and voice clips play; see Status.

## Environment

| Variable | Effect |
| --- | --- |
| `SMS_AUDIO=0` | no output device and no `SMS_AUDIO_WAV`; the AI DMA keeps its clock (as with no device), so the audio thread, the sequencer and the mixer run as with sound. An idle DMA (the old behaviour) stopped JAudio's sequencer, which is what frees a stopped sequence: after eight music changes no root sequence slot was left, and the game faulted on the next stage exit (docs/64-BIT.md, item 19) |
| `SMS_NO_AUDIO=1` | an empty sound configuration (`noaudio.cpp`); the AI DMA stays idle |
| `SMS_AUDIO_OUT=sdl\|null` | output device; the default is SDL, or `null` when headless. `null` paces DMA from the host clock |
| `SMS_AUDIO_WAV=file.wav` | record everything played (32 kHz stereo) |
| `SMS_AUDIO_TRACE=1` | log each voice start and the voice counts every 5 s |
| `SMS_AUDIO_FX=0` | bypass the FX (echo) lines |
| `SMS_AUDIO_SWAP=0` | keep DMA pair order (the default swaps the (right, left) DMA pairs for SDL) |
| `SMS_AUDIO_MASTER_SHIFT=n` | fixed point of the DSP master level (default 15, measured) |
| `SMS_AUDIO_SLOT_SHIFT=n` | fixed point of `mixChannels` volumes (default 14, measured) |
| `SMS_AUDIO_ARAM_DUMP=file[,n]` | write the ARAM image after `n` subframes (default 16000) |

## Status

Compared against retail running under Dolphin with DSP LLE (the game's own microcode), boot → title → file select; the numbers are in PROTOCOL.md.

- The boot jingle matches retail sample for sample (correlation 1.000, gain 0.99–1.00).
  This fixed the master at Q15 (earlier Q14 was 2× too loud); `mixChannels` volumes are Q14 (auto mixer Q15).
- Voices, waves and volumes match retail's voice blocks (the same ARAM addresses, the same slot volumes).
  File-select music plays with retail's voice count (11–13 vs 10.9) and a continuous floor, after two timing fixes: AI blocks wait for the DSP frame, and `audio-03` stops the host-time "DSP overload" voice stealing that cut notes to about a third.
- Bus 1 is left, bus 2 right, as retail.
- **Decoders are bit-exact against the disc** (all 449 ADPCM4 and 3 ADPCM2 loop histories; `tests/audio_test`).
- FX returns use Q15 gains, mode-specific FIR ordering, and the game's RAM delay buffers. Auto-mixer wet sends use the front-effect buses and Sunshine's Q16 reverb factor (the highest game value is just under 0.5). Stop requests release gradually through zero. Asset-free impulse and release checks cover these rules in both word sizes; see PROTOCOL.md.
- Untested against retail: stream voices (not used in this segment), surround mode, FX lines (no fx sends in this segment), oscillator voices, per-voice filters (retail's IIR coefficients were identity here).

Debug aids: `SMS_AUDIO_TRACE=1` (voice starts, ends with reason and lifetime, counts every 5 s) and `SMS_AUDIO_ARAM_DUMP=file[,subframe]` (writes the ARAM image once, for replaying traced voice blocks offline).

## Offline test

```sh
cd platform/audio/tests
make check                 # no disc required
make audio_test
mkdir -p out
./audio_test /path/to/disc/files out/
```

It reads `mSound.aaf` from `data/nintendo.szs` (Yaz0 + RARC), checks the decoders over all 24 wave archives, and writes WAVs.
The WAVs are one-shots and loops from `w1stLoad_0.aw`/`wScene_0.aw`, a wave an octave up and down (lengths halve and double), and one through the AAF's FX scene 0.
It prints lengths and levels.
