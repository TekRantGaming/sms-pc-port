// JAudio data: the AAF init data (mSound.aaf) and what it embeds (sound
// tables, IBNK instrument banks, WSYS wave systems, sequence-archive header,
// sound/FX scene tables), and BAS animation-sound tables.
// Layouts: JAIBasic.cpp (checkInitDataOnMemory), JAIData.hpp/.cpp,
// JASBNKParser.hpp/.cpp, JASWSParser.hpp/.cpp, JASVload.cpp,
// JASDSPInterface.hpp (FxlineConfig_), JAIAnimation.hpp.
#include <vector>

#include "endian_util.h"
#include "port_endian.h"

using namespace pe;

namespace {

// Offsets already converted, for structures shared between parents.
struct Visited {
	std::vector<u8> bits;
	explicit Visited(u32 size) : bits(size / 8 + 1, 0) { }
	// Returns true the first time `off` is seen.
	bool first(u32 off)
	{
		u32 i = off / 8;
		if (i >= bits.size())
			return false;
		u8 m = (u8)(1u << (off & 7));
		if (bits[i] & m)
			return false;
		bits[i] |= m;
		return true;
	}
};

// --- Sound table (JAISoundTable image) --------------------------------------------

void sound_table(const Buf& b)
{
	if (!b.has(0, 0x50))
		return;
	b.sw16(0x04);
	b.swn(0x06, 36, 2); // 18 x {u16 count, u16 first index}
	for (u32 e = 0x50; e + 0x10 <= b.size; e += 0x10) {
		b.sw32(e);       // swBit
		b.sw16(e + 6);   // offsetNo
		b.sw32(e + 8);   // pitch
	}
}

// --- IBNK ---------------------------------------------------------------------

struct Bank {
	Buf b;
	Visited seen;
	explicit Bank(const Buf& buf) : b(buf), seen(buf.size) { }

	bool claim(u32 off, u32 n) { return off && b.has(off, n) && seen.first(off); }

	void osc_table(u32 off)
	{
		if (!off || !seen.first(off))
			return;
		// s16 {mode, time, value} triplets, up to and including the first
		// with mode > 10 (JASBNKParser getOscTableEndPtr).
		for (u32 p = off; b.has(p, 6); p += 6) {
			s16_t mode = (s16_t)b.sw16(p);
			b.sw16(p + 2);
			b.sw16(p + 4);
			if (mode > 10)
				break;
		}
	}
	typedef int16_t s16_t;

	void osc(u32 off)
	{
		if (!claim(off, 0x18))
			return;
		b.sw32(off + 0x04);
		u32 ads = b.sw32(off + 0x08);
		u32 rel = b.sw32(off + 0x0C);
		b.sw32(off + 0x10);
		b.sw32(off + 0x14);
		osc_table(ads);
		osc_table(rel);
	}
	void rand(u32 off)
	{
		if (!claim(off, 0x0C))
			return;
		b.sw32(off + 4);
		b.sw32(off + 8);
	}
	void sense(u32 off) { rand(off); }
	void vmap(u32 off)
	{
		if (!claim(off, 0x10))
			return;
		b.swn(off + 4, 3, 4);
	}
	void keymap(u32 off)
	{
		if (!claim(off, 8))
			return;
		u32 n = b.sw32(off + 4);
		for (u32 i = 0; i < n && b.has(off + 8 + i * 4, 4); i++)
			vmap(b.sw32(off + 8 + i * 4));
	}
	void inst(u32 off)
	{
		if (!claim(off, 0x2C))
			return;
		b.swn(off, 4, 4); // magic, unk, volume, pitch
		u32 o[6];
		for (int i = 0; i < 6; i++)
			o[i] = b.sw32(off + 0x10 + i * 4);
		osc(o[0]);
		osc(o[1]);
		rand(o[2]);
		rand(o[3]);
		sense(o[4]);
		sense(o[5]);
		u32 n = b.sw32(off + 0x28);
		for (u32 i = 0; i < n && b.has(off + 0x2C + i * 4, 4); i++)
			keymap(b.sw32(off + 0x2C + i * 4));
	}
	void pmap(u32 off)
	{
		if (!claim(off, 0x14))
			return;
		b.swn(off, 2, 4);
		rand(b.sw32(off + 0x08));
		rand(b.sw32(off + 0x0C));
		u32 n = b.sw32(off + 0x10);
		for (u32 i = 0; i < n && b.has(off + 0x14 + i * 4, 4); i++)
			vmap(b.sw32(off + 0x14 + i * 4));
	}
	void perc(u32 off)
	{
		if (!claim(off, 0x288))
			return;
		u32 magic = b.sw32(off);
		for (u32 i = 0; i < 0x80; i++)
			pmap(b.sw32(off + 0x88 + i * 4));
		if (magic == 'PER2')
			b.swn(off + 0x308, 0x80, 2);
	}
	void run()
	{
		b.swn(0, 9, 4); // header
		for (u32 i = 0; i < 0xF0; i++) {
			u32 off = b.sw32(0x24 + i * 4);
			if (!off || !b.has(off, 4))
				continue;
			// Slots 0-0x7F are instruments, 0xE4-0xEF percussion sets;
			// the struct magic confirms which.
			const u8* m = b.at(off);
			if (magic_is(m, "PER", 3) || (i >= 0xE4 && !magic_is(m, "INST", 4)))
				perc(off);
			else if (i < 0x80 || magic_is(m, "INST", 4))
				inst(off);
		}
	}
};

// --- WSYS ---------------------------------------------------------------------

struct WaveSys {
	Buf b;
	Visited seen;
	explicit WaveSys(const Buf& buf) : b(buf), seen(buf.size) { }

	void wave(u32 off)
	{
		if (!off || !b.has(off, 0x2C) || !seen.first(off))
			return;
		b.sw32(off + 0x04);    // sample rate
		b.swn(off + 0x08, 6, 4);
		b.swn(off + 0x20, 2, 2);
		b.swn(off + 0x24, 2, 4);
	}
	void run()
	{
		b.swn(0x00, 4, 4);
		u32 winf = b.sw32(0x10);
		u32 wbct = b.sw32(0x14);
		if (!b.has(wbct, 0x0C) || !b.has(winf, 0x08))
			return;
		b.swn(wbct, 3, 4);
		u32 groups = ld32(b.at(wbct + 8));
		b.swn(winf, 2, 4);
		for (u32 g = 0; g < groups && b.has(wbct + 0x0C + g * 4, 4); g++) {
			u32 scene = b.sw32(wbct + 0x0C + g * 4);
			u32 arc   = b.has(winf + 8 + g * 4, 4) ? b.sw32(winf + 8 + g * 4) : 0;
			u32 nwave = 0;
			if (scene && b.has(scene, 0x10) && seen.first(scene)) {
				b.swn(scene, 4, 4);
				u32 cdf = ld32(b.at(scene + 0x0C));
				if (cdf && b.has(cdf, 8) && seen.first(cdf)) {
					b.swn(cdf, 2, 4);
					nwave = ld32(b.at(cdf + 4));
					for (u32 i = 0; i < nwave && b.has(cdf + 8 + i * 4, 4); i++) {
						u32 cw = b.sw32(cdf + 8 + i * 4);
						if (cw && b.has(cw, 4) && seen.first(cw))
							b.sw32(cw);
					}
					// other control tables (C-EX, C-ST) referenced by the scene
					for (int k = 1; k < 3; k++) {
						u32 t = ld32(b.at(scene + k * 4));
						if (t && b.has(t, 8) && seen.first(t)) {
							b.swn(t, 2, 4);
							u32 m = ld32(b.at(t + 4));
							for (u32 i = 0; i < m && b.has(t + 8 + i * 4, 4); i++) {
								u32 cw = b.sw32(t + 8 + i * 4);
								if (cw && b.has(cw, 4) && seen.first(cw))
									b.sw32(cw);
							}
						}
					}
				} else if (cdf && b.has(cdf, 8)) {
					nwave = ld32(b.at(cdf + 4));
				}
			}
			if (arc && b.has(arc, 0x74) && seen.first(arc)) {
				for (u32 i = 0; i < nwave && b.has(arc + 0x74 + i * 4, 4); i++)
					wave(b.sw32(arc + 0x74 + i * 4));
			}
		}
	}
};

// --- AAF payloads -------------------------------------------------------------

void seq_archive_header(const Buf& b)
{
	if (!b.has(0, 0x20))
		return;
	b.swn(0x00, 4, 4);
	u32 n = ld32(b.at(0x0C));
	for (u32 k = 0; k < n && b.has(0x20 * (k + 1), 0x20); k++) {
		u32 e = 0x20 * (k + 1);
		b.swn(e, 3, 4);
		b.swn(e + 0x0C, 2, 2);
		b.swn(e + 0x10, 4, 4);
	}
}

void sound_scene_table(const Buf& b)
{
	u32 n = b.sw32(0);
	b.swn(4, n, 4); // offsets; the scenes are {u8, u8} JAICategoryInfo arrays
}

void fx_scene_table(const Buf& b)
{
	u32 n = b.sw32(0);
	b.swn(4, 4, 4); // buffer sizes
	for (u32 s = 0; s < n && b.has(0x14 + s * 4, 4); s++) {
		u32 off = b.sw32(0x14 + s * 4);
		for (u32 l = 0; l < 4 && b.has(off + l * 0x20, 0x20); l++) {
			u32 e = off + l * 0x20; // FxlineConfig_
			b.swn(e + 0x02, 4, 2);
			b.sw32(e + 0x0C);
			b.swn(e + 0x10, 8, 2);
		}
	}
}

} // namespace

extern "C" int port_endian_sound_table(void* data, uint32_t size)
{
	sound_table(Buf(data, size));
	return 1;
}

extern "C" int port_endian_ibnk(void* data, uint32_t size)
{
	Buf b(data, size);
	if (!b.has(0, 0x24 + 0xF0 * 4) || !magic_is(data, "IBNK", 4))
		return 0;
	Bank(b).run();
	return 1;
}

extern "C" int port_endian_wsys(void* data, uint32_t size)
{
	Buf b(data, size);
	if (!b.has(0, 0x18) || !magic_is(data, "WSYS", 4))
		return 0;
	WaveSys(b).run();
	return 1;
}

// Region of the AAF a command's payload occupies: IBNK/WSYS carry their own
// size at +4.
static u32 embedded_size(const Buf& f, u32 off)
{
	if (!f.has(off, 8))
		return 0;
	u32 s = be32(f.at(off + 4));
	return f.has(off, s) ? s : f.size - off;
}

extern "C" int port_endian_aaf(void* data, uint32_t size)
{
	Buf f(data, size);
	// The command stream starts with a small command number (1..8) in
	// big-endian order; a converted stream starts with it in host order.
	if (!f.has(0, 4) || be32(f.at(0)) == 0 || be32(f.at(0)) > 8)
		return 0;
	u32 i = 0; // word index
	// The command stream ends before the first payload. Not every AAF ends it
	// with a 0 (Super Mario Eclipse's runs straight into its sound table,
	// which the game then skips over as an unknown command): stop there
	// rather than swapping the payload as command words.
	u32 payload = f.size;
	auto note = [&](u32 off) {
		if (off && off < payload)
			payload = off;
	};
	for (;;) {
		if (!f.has(i * 4, 4) || 4 * i >= payload)
			break;
		u32 cmd = f.sw32(4 * i++);
		if (cmd == 0)
			break;
		switch (cmd) {
		case 1: { // sound tables
			bool separate = f.has(4 * (i + 2), 4) && be32(f.at(4 * (i + 2))) != 0;
			int ntab      = separate ? 3 : 1;
			for (int t = 0; t < ntab; t++) {
				u32 off = f.sw32(4 * i++);
				u32 sz  = f.sw32(4 * i++);
				note(off);
				if (off && f.has(off, sz))
					sound_table(f.sub(off, sz));
			}
			if (!separate)
				f.sw32(4 * i++);
			break;
		}
		case 2:   // IBNK list {off, unk, wave bank no} ... 0
		case 3: { // WSYS list {off, unk, load timing} ... 0
			while (f.has(4 * i, 4) && be32(f.at(4 * i)) != 0) {
				u32 off = f.sw32(4 * i);
				f.sw32(4 * (i + 1));
				f.sw32(4 * (i + 2));
				i += 3;
				note(off);
				u32 sz = embedded_size(f, off);
				if (cmd == 2)
					port_endian_ibnk(f.at(off), sz);
				else
					port_endian_wsys(f.at(off), sz);
			}
			f.sw32(4 * i++); // terminator
			break;
		}
		case 4:
		case 5:
		case 6:
		case 7:
		case 8: {
			u32 off = f.sw32(4 * i);
			u32 sz  = f.sw32(4 * (i + 1));
			f.sw32(4 * (i + 2));
			i += 3;
			note(off);
			if (!off || !f.has(off, 4))
				break;
			Buf p = f.sub(off, sz);
			if (cmd == 4)
				seq_archive_header(p);
			else if (cmd == 6)
				sound_scene_table(p);
			else if (cmd == 7)
				fx_scene_table(p);
			// cmd 5 (stream list): the embedded 0x20-byte stream headers are
			// copied into a StreamHeader alongside headers read from disc,
			// so they are converted where the header is used
			// (JAIGFrameStream), not here. cmd 8: no readers.
			break;
		}
		default: // unknown: words up to a 0
			while (f.has(4 * i, 4) && 4 * i < payload && f.sw32(4 * i++) != 0)
				;
			break;
		}
	}
	return 1;
}
