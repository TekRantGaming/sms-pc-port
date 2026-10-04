// Independent, exact impulse expectations for the Zelda DSP reverb protocol.
// No disc assets or game headers are needed.
#include "../dsp_mixer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint8_t voices[0x180], fx[4][0x20], pcm[4096];
static int16_t delay[4][160], left[80], right[80];
static uint32_t resampler[128];
template<class T> static void put(uint8_t* p, int offset, T value)
{
	memcpy(p + offset, &value, sizeof value);
}
template<class T> static T get(const uint8_t* p, int offset)
{
	T value;
	memcpy(&value, p + offset, sizeof value);
	return value;
}
static const uint8_t* sample_memory(uint32_t addr) { return addr < sizeof pcm ? pcm + addr : NULL; }
static int16_t* effect_memory(uint32_t addr, uint32_t size)
{
	return addr >= 1 && addr <= 4 && size <= 160 ? delay[addr - 1] : NULL;
}
static void reset()
{
	memset(voices, 0, sizeof voices);
	memset(fx, 0, sizeof fx);
	memset(delay, 0, sizeof delay);
	memset(pcm, 64, sizeof pcm);
	// Exact 1:1 resampling, with one Q15 rounding step.
	for (int i = 0; i < 64; i++) {
		resampler[i * 2] = 32767;
		resampler[i * 2 + 1] = 0;
	}
	port_dspmix_setup(1, voices, resampler, NULL, fx);
	port_dspmix_set_aram(sample_memory);
	port_dspmix_set_mram(effect_memory);
}
static void line(int i, int mode, uint16_t bus, int16_t gain)
{
	put<uint16_t>(fx[i], 0, mode);
	put<uint16_t>(fx[i], 2, 1);
	put<uint32_t>(fx[i], 4, i + 1);
	put<uint16_t>(fx[i], 8, bus);
	put<int16_t>(fx[i], 10, gain);
}
static void render() { port_dspmix_render(left, right, 80, 0x8000); }
static void voice(bool automatic)
{
	put<uint16_t>(voices, 0, 1);
	put<uint16_t>(voices, 4, 0x1000);
	put<uint16_t>(voices, 8, 1);
	put<uint16_t>(voices, 0x100, 8);
	put<uint16_t>(voices, 0x102, 1);
	put<uint32_t>(voices, 0x114, sizeof pcm);
	if (automatic) {
		put<uint16_t>(voices, 0x52, 127 << 8);
		put<uint16_t>(voices, 0x54, 0x4000);
		put<uint16_t>(voices, 0x56, 0x4000);
		put<uint16_t>(voices, 0x58, 1);
	} else {
		put<uint16_t>(voices, 0x10, 0xD00);
		put<uint16_t>(voices, 0x12, 0x4000);
		put<uint16_t>(voices, 0x14, 0x4000);
	}
}
int main()
{
	unsetenv("SMS_AUDIO_FX");
	unsetenv("SMS_AUDIO_MASTER_SHIFT");
	unsetenv("SMS_AUDIO_SLOT_SHIFT");
	reset();
	line(0, 2, 0xD00, 4096);
	delay[0][0] = 1024;
	put<int16_t>(fx[0], 0x10 + 7 * 2, 0x4000);
	render();
	assert(left[8] == 128 && left[1] == 0); // Q15 return precedes the post-filter.
	assert(delay[0][1] == 512 && delay[0][8] == 0); // Forward FIR, written after mixing.
	reset();
	line(0, 1, 0xD60, 4096);
	delay[0][0] = 1024;
	put<int16_t>(fx[0], 0x10 + 7 * 2, 0x4000);
	render();
	assert(right[1] == 64 && right[8] == 0 && left[1] == 0);
	reset();
	line(0, 1, 0xD00, 4096);
	delay[0][79] = 2000;
	put<int16_t>(fx[0], 0x10, 0x4000);
	render();
	render();
	assert(left[7] == 125); // Last eight raw samples cross subframes.
	reset();
	line(0, 2, 0xD00, 4096);
	delay[0][0] = 1024;
	render();
	assert(left[8] == 128);
	memset(delay, 0, sizeof delay);
	render();
	for (int i = 0; i < 80; i++)
		assert(left[i] == 0); // CPU clear must remove old echo.
	reset();
	line(0, 2, 0, 0);
	line(2, 2, 0, 0);
	delay[2][0] = 1024;
	put<uint16_t>(fx[2], 0xC, 0xDC0);
	put<int16_t>(fx[2], 0xE, 8192);
	render();
	assert(delay[0][8] == 256); // Second return may feed another reverb bus.
	reset();
	for (int i = 0; i < 4; i++)
		line(i, 2, 0, 0);
	voice(true);
	render();
	assert(left[79] == 8191 && right[79] == 0);
	assert(delay[2][79] == 4063 && delay[3][79] == 0 && delay[0][79] == 0 && delay[1][79] == 0);
	// The largest game-generated factor (127 << 8) is just under 0.5,
	// not a full-strength wet send. Changing it must leave dry levels alone.
	put<uint16_t>(voices, 0x52, 0x4000);
	render();
	assert(left[79] == 8191 && delay[2][79] == 2047);
	put<uint16_t>(voices, 0x52, 1);
	render();
	assert(left[79] == 8191 && delay[2][79] == 0);
	for (int automatic = 0; automatic < 2; automatic++) {
		reset();
		voice(automatic);
		render();
		assert(left[79] == (automatic ? 8191 : 16383)); // Preserve dry path levels.
		put<uint16_t>(voices, 0x10A, 1);
		int frames = 0;
		do {
			render();
			frames++;
			if (frames == 1) {
				assert(get<uint16_t>(voices, 2) == 0);
				assert(left[79] > 0);
			}
			assert(frames <= 16);
		} while (!get<uint16_t>(voices, 2));
		assert(frames == 15); // Halve the current volume until zero, then finish.
		render();
		for (int i = 0; i < 80; i++)
			assert(left[i] == 0 && right[i] == 0);
	}
	puts("PASS: Q15 returns, filter modes/history, RAM clears, cross-bus feedback, "
	     "Q16 footstep sends, unchanged dry levels, smooth release and silence");
}
