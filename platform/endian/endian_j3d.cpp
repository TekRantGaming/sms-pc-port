// J3D resources: models/material tables (J3D2 bmd*/bdl*/bmt*) and animations
// (J3D1 b?k1/b?a1), plus the ResNTAB and ResTIMG structures they embed.
// Layouts follow the decomp headers (include/JSystem/J3D/J3DGraphLoader/*,
// J3DGraphBase/J3DStruct.hpp, JUtility/JUTDataHeader.hpp, ResTIMG.hpp).
#include "endian_util.h"
#include "port_endian.h"

using namespace pe;

namespace pe {

int layout_sections(Section* s, int n, u32 limit)
{
	// insertion sort by offset (n is small)
	for (int i = 1; i < n; i++) {
		Section t = s[i];
		int j     = i - 1;
		while (j >= 0 && s[j].off > t.off) {
			s[j + 1] = s[j];
			j--;
		}
		s[j + 1] = t;
	}
	int m = 0;
	for (int i = 0; i < n; i++) {
		if (s[i].off == 0 || s[i].off >= limit)
			continue;
		// Sections at the same offset: the empty ones in front point where the
		// next table starts (some tools write that instead of 0, Super Mario
		// Eclipse's models among them), so the last one owns the data.
		if (m > 0 && s[m - 1].off == s[i].off) {
			s[m - 1] = s[i];
			continue;
		}
		s[m++] = s[i];
	}
	for (int i = 0; i < m; i++)
		s[i].end = (i + 1 < m) ? s[i + 1].off : limit;
	return m;
}

} // namespace pe

// --- Shared structures ----------------------------------------------------------

// ResNTAB: u16 count, u16 pad, count x {u16 hash, u16 offset}, strings.
static void ntab(const Buf& b, u32 off)
{
	if (!off || !b.has(off, 4))
		return;
	u16 n = b.sw16(off);
	b.sw16(off + 2);
	b.swn(off + 4, n * 2u, 2);
}

extern "C" int port_endian_ntab(void* p, uint32_t size)
{
	Buf b(p, size);
	ntab(b, 0);
	return 1;
}

// ResTIMG header (0x20 bytes); image and palette data stay big-endian.
static void timg(const Buf& b, u32 off)
{
	if (!b.has(off, 0x20))
		return;
	b.sw16(off + 0x02); // width
	b.sw16(off + 0x04); // height
	b.sw16(off + 0x0A); // numColors
	b.sw32(off + 0x0C); // paletteOffset
	b.sw16(off + 0x1A); // lodBias
	b.sw32(off + 0x1C); // imageDataOffset
}

extern "C" int port_endian_timg_header(void* p)
{
	timg(Buf(p, 0x20), 0);
	return 1;
}

// A plausible big-endian ResTIMG header (BTI files have no magic).
static bool timg_plausible_be(const Buf& b)
{
	if (!b.has(0, 0x20))
		return false;
	const u8* h = b.base;
	u16 w = be16(h + 2), hgt = be16(h + 4);
	u32 img = be32(h + 0x1C), pal = be32(h + 0x0C);
	if (h[0] > 0x0E || w == 0 || hgt == 0 || w > 1024 || hgt > 1024)
		return false;
	if (h[6] > 2 || h[7] > 2 || h[8] > 1)
		return false;
	if (img != 0 && (img < 0x20 || img >= b.size))
		return false;
	if (h[8] && (pal < 0x20 || pal >= b.size))
		return false;
	return true;
}

// Standalone .bti files have no magic; the (always zero, never read) pad byte
// at 0x19 marks a converted header so a second conversion is refused.
extern "C" int port_endian_bti_is_native(const void* p) { return ((const u8*)p)[0x19] == PE_NATIVE_MARK; }

extern "C" int port_endian_bti(void* p, uint32_t size)
{
	Buf b(p, size);
	if (!b.has(0, 0x20) || b.base[0x19] == PE_NATIVE_MARK || !timg_plausible_be(b))
		return 0;
	timg(b, 0);
	b.base[0x19] = PE_NATIVE_MARK;
	return 1;
}

// A texture a code mod builds into its own code (BetterSunshineEngine's
// icons): a whole BTI, header first, of no known size; converted in place the
// first time the game stores it (JUTTexture::storeTIMG).
extern "C" int port_endian_bti_embedded(void* p)
{
	Buf b(p, 1u << 24); // only the header is read; offsets just have to be plausible
	if (b.base[0x19] == PE_NATIVE_MARK || !timg_plausible_be(b))
		return 0;
	timg(b, 0);
	b.base[0x19] = PE_NATIVE_MARK;
	return 1;
}

// ResTLUT (J2D 'TLUT' resources, JUTPalette::storeTLUT): u8 format, u8
// transparency, u16 numColors, pad to 0x20, then TLUT entries (stay
// big-endian for GX). Byte 4 (pad, never read) marks a converted header.
extern "C" int port_endian_tlut(void* p, uint32_t size)
{
	Buf b(p, size);
	if (!b.has(0, 0x20) || b.base[4] == PE_NATIVE_MARK)
		return 0;
	u16 n = be16(b.at(2));
	if (b.base[0] > 2 || n == 0 || 0x20 + n * 2u > size)
		return 0;
	b.sw16(2);
	b.base[4] = PE_NATIVE_MARK;
	return 1;
}

// --- J3D models -------------------------------------------------------------------

// GXAttr / GXCompType values used by VTX1.
enum {
	GXA_POS  = 9,
	GXA_NRM  = 10,
	GXA_CLR0 = 11,
	GXA_CLR1 = 12,
	GXA_TEX0 = 13,
	GXA_TEX7 = 20,
	GXA_NBT  = 25,
	GXA_NULL = 0xFF,
};

static u32 comp_width(u32 attr, u32 type)
{
	if (attr == GXA_CLR0 || attr == GXA_CLR1) {
		// RGB565, RGB8, RGBX8, RGBA4, RGBA6, RGBA8
		return (type == 0 || type == 3) ? 2 : 1;
	}
	switch (type) {
	case 2:
	case 3:
		return 2; // U16, S16
	case 4:
		return 4; // F32
	default:
		return 1; // U8, S8
	}
}

static void vtx1(const Buf& b)
{
	u32 fmt = b.sw32(0x08);
	u32 off[13];
	for (int i = 0; i < 13; i++)
		off[i] = b.sw32(0x0C + i * 4);
	// Element width per array slot, from the attribute format list.
	u32 width[13] = { 0 };
	for (u32 e = fmt; fmt && b.has(e, 16); e += 16) {
		u32 attr = b.sw32(e);
		u32 cnt  = b.sw32(e + 4);
		u32 type = b.sw32(e + 8);
		(void)cnt;
		if (attr == GXA_NULL)
			break;
		int slot = -1;
		if (attr == GXA_POS)
			slot = 0;
		else if (attr == GXA_NRM)
			slot = 1;
		else if (attr == GXA_NBT)
			slot = 2;
		else if (attr == GXA_CLR0 || attr == GXA_CLR1)
			slot = 3 + (attr - GXA_CLR0);
		else if (attr >= GXA_TEX0 && attr <= GXA_TEX7)
			slot = 5 + (attr - GXA_TEX0);
		if (slot >= 0)
			width[slot] = comp_width(attr, type);
	}
	// Arrays run to the next array (or the end of the block).
	Section s[14];
	int n = 0;
	for (int i = 0; i < 13; i++)
		s[n++] = { off[i], i, 0 };
	s[n++] = { fmt, 100, 0 };
	n = layout_sections(s, n, b.size);
	for (int i = 0; i < n; i++)
		if (s[i].kind < 13 && width[s[i].kind] > 1)
			b.swrange(s[i].off, s[i].end, width[s[i].kind]);
}

// Swap the u16 (hierarchy) entries up to and including the (0, 0) terminator.
static void inf1(const Buf& b)
{
	b.sw16(0x08); // flags
	b.sw32(0x0C); // packet count
	b.sw32(0x10); // vertex count
	u32 h = b.sw32(0x14);
	for (; h && b.has(h, 4); h += 4) {
		u16 type = b.sw16(h);
		b.sw16(h + 2);
		if (type == 0)
			break;
	}
}

static void evp1(const Buf& b)
{
	b.sw16(0x08);
	Section s[4];
	for (int i = 0; i < 4; i++)
		s[i] = { b.sw32(0x0C + i * 4), i, 0 };
	int n = layout_sections(s, 4, b.size);
	for (int i = 0; i < n; i++) {
		static const u32 w[4] = { 1, 2, 4, 4 }; // mix count u8, index u16, weight f32, inv Mtx f32
		b.swrange(s[i].off, s[i].end, w[s[i].kind]);
	}
}

static void drw1(const Buf& b)
{
	u16 n     = b.sw16(0x08);
	b.sw32(0x0C); // flags (u8)
	u32 index = b.sw32(0x10);
	b.swn(index, n, 2);
}

static void jnt1(const Buf& b)
{
	u16 num = b.sw16(0x08);
	(void)num;
	u32 init = b.sw32(0x0C), index = b.sw32(0x10), names = b.sw32(0x14);
	Section s[3] = { { init, 0, 0 }, { index, 1, 0 }, { names, 2, 0 } };
	int n = layout_sections(s, 3, b.size);
	for (int i = 0; i < n; i++) {
		switch (s[i].kind) {
		case 0: // J3DJointInitData, 0x40 bytes
			for (u32 e = s[i].off; e + 0x40 <= s[i].end; e += 0x40) {
				b.sw16(e);                // kind
				b.swn(e + 0x04, 3, 4);    // scale
				b.swn(e + 0x10, 3, 2);    // rotation
				b.swn(e + 0x18, 10, 4);   // translate, radius, min, max
			}
			break;
		case 1:
			b.swrange(s[i].off, s[i].end, 2);
			break;
		case 2:
			ntab(b, s[i].off);
			break;
		}
	}
}

static void shp1(const Buf& b)
{
	b.sw16(0x08);
	enum { INIT, INDEX, NAMES, VTXDESC, MTXTAB, DLIST, MTXINIT, DRAWINIT };
	Section s[8];
	for (int i = 0; i < 8; i++)
		s[i] = { b.sw32(0x0C + i * 4), i, 0 };
	int n = layout_sections(s, 8, b.size);
	for (int i = 0; i < n; i++) {
		u32 o = s[i].off, e = s[i].end;
		switch (s[i].kind) {
		case INIT: // J3DShapeInitData, 0x28 bytes
			for (; o + 0x28 <= e; o += 0x28) {
				b.swn(o + 0x02, 4, 2);
				b.swn(o + 0x0C, 7, 4);
			}
			break;
		case INDEX:
		case MTXTAB:
			b.swrange(o, e, 2);
			break;
		case NAMES:
			ntab(b, o);
			break;
		case VTXDESC: // {GXAttr, GXAttrType} u32 pairs
		case DRAWINIT: // {u32 size, u32 offset}
			b.swrange(o, e, 4);
			break;
		case MTXINIT: // {u16, u16, u32}
			for (; o + 8 <= e; o += 8) {
				b.sw16(o);
				b.sw16(o + 2);
				b.sw32(o + 4);
			}
			break;
		case DLIST: // GX display lists: big-endian, left alone
			break;
		}
	}
}

static void tex1(const Buf& b)
{
	u16 n     = b.sw16(0x08);
	u32 res   = b.sw32(0x0C);
	u32 names = b.sw32(0x10);
	for (u32 i = 0; i < n; i++)
		timg(b, res + i * 0x20);
	ntab(b, names);
}

// Material sections (MAT3 order; MAT2 maps onto it).
enum MatKind {
	M_INIT,
	M_ID,
	M_NAMES,
	M_IND,
	M_CULL,
	M_BYTES, // any u8-only table (colours, counts, byte infos)
	M_LIGHT,
	M_TEXMTX,
	M_TEXNO,
	M_TEVCOLOR,
	M_FOG,
	M_NBT,
};

// J3DMaterialInitData (MAT3, 0x14C): u8[8], u16 [0x08,0x9C), u8 [0x9C,0xBC), u16 [0xBC,0x14C).
// J3DMaterialInitData_v21 (MAT2, 0x138): u8[8], u16 [0x08,0x88), u8 [0x88,0xA8), u16 [0xA8,0x138).
static void mat_init(const Buf& b, u32 o, u32 e, bool v21)
{
	u32 stride = v21 ? 0x138 : 0x14C;
	u32 a0 = 0x08, a1 = v21 ? 0x88 : 0x9C, b0 = v21 ? 0xA8 : 0xBC;
	for (; o + stride <= e; o += stride) {
		b.swrange(o + a0, o + a1, 2);
		b.swrange(o + b0, o + stride, 2);
	}
}

static void mat_section(const Buf& b, const Section& s, bool v21)
{
	u32 o = s.off, e = s.end;
	switch (s.kind) {
	case M_INIT:
		mat_init(b, o, e, v21);
		break;
	case M_ID:
	case M_TEXNO:
	case M_TEVCOLOR: // GXColorS10
		b.swrange(o, e, 2);
		break;
	case M_NAMES:
		ntab(b, o);
		break;
	case M_IND: // J3DIndInitData, 0x138: 3 x J3DIndTexMtxInfo (f32[2][3], s8, pad) at 0x14
		for (; o + 0x138 <= e; o += 0x138)
			for (int k = 0; k < 3; k++)
				b.swn(o + 0x14 + k * 0x1C, 6, 4);
		break;
	case M_CULL: // GXCullMode (u32 enum)
		b.swrange(o, e, 4);
		break;
	case M_LIGHT: // J3DLightInfo, 0x34: Vec pos, Vec dir, GXColor, f32 a[3], f32 k[3]
		for (; o + 0x34 <= e; o += 0x34) {
			b.swn(o, 6, 4);
			b.swn(o + 0x1C, 6, 4);
		}
		break;
	case M_TEXMTX: // J3DTexMtxInfo, 0x64
		for (; o + 0x64 <= e; o += 0x64) {
			b.swn(o + 0x04, 5, 4);  // center, SRT scale x/y
			b.sw16(o + 0x18);       // SRT rotation
			b.swn(o + 0x1C, 2, 4);  // SRT translation
			b.swn(o + 0x24, 16, 4); // effect matrix
		}
		break;
	case M_FOG: // J3DFogInfo, 0x2C
		for (; o + 0x2C <= e; o += 0x2C) {
			b.sw16(o + 0x02);
			b.swn(o + 0x04, 4, 4);
			b.swn(o + 0x18, 10, 2);
		}
		break;
	case M_NBT: // J3DNBTScaleInfo, 0x10
		for (; o + 0x10 <= e; o += 0x10)
			b.swn(o + 0x04, 3, 4);
		break;
	case M_BYTES:
	default:
		break;
	}
}

static void mat(const Buf& b, bool v21)
{
	static const u8 mat3[30] = {
		M_INIT, M_ID, M_NAMES, M_IND, M_CULL, M_BYTES /*matColor*/,
		M_BYTES /*colorChanNum*/, M_BYTES /*colorChanInfo*/, M_BYTES /*ambColor*/,
		M_LIGHT, M_BYTES /*texGenNum*/, M_BYTES /*texCoord*/, M_BYTES /*texCoord2*/,
		M_TEXMTX, M_TEXMTX /*post*/, M_TEXNO, M_BYTES /*tevOrder*/, M_TEVCOLOR,
		M_BYTES /*kColor*/, M_BYTES /*tevStageNum*/, M_BYTES /*tevStage*/,
		M_BYTES /*swapMode*/, M_BYTES /*swapTable*/, M_FOG, M_BYTES /*alphaComp*/,
		M_BYTES /*blend*/, M_BYTES /*zMode*/, M_BYTES /*zCompLoc*/, M_BYTES /*dither*/,
		M_NBT,
	};
	static const u8 mat2[27] = {
		M_INIT, M_ID, M_NAMES, M_CULL, M_BYTES /*matColor*/, M_BYTES /*colorChanNum*/,
		M_BYTES /*colorChanInfo*/, M_BYTES /*texGenNum*/, M_BYTES /*texCoord*/,
		M_BYTES /*texCoord2*/, M_TEXMTX, M_TEXMTX /*post*/, M_TEXNO,
		M_BYTES /*tevOrder*/, M_TEVCOLOR, M_BYTES /*kColor*/, M_BYTES /*tevStageNum*/,
		M_BYTES /*tevStage*/, M_BYTES /*swapMode*/, M_BYTES /*swapTable*/, M_FOG,
		M_BYTES /*alphaComp*/, M_BYTES /*blend*/, M_BYTES /*zMode*/,
		M_BYTES /*zCompLoc*/, M_BYTES /*dither*/, M_NBT,
	};
	const u8* kinds = v21 ? mat2 : mat3;
	int nk          = v21 ? 27 : 30;
	b.sw16(0x08);
	Section s[30];
	for (int i = 0; i < nk; i++)
		s[i] = { b.sw32(0x0C + i * 4), kinds[i], 0 };
	int n = layout_sections(s, nk, b.size);
	for (int i = 0; i < n; i++)
		mat_section(b, s[i], v21);
}

// --- J3D animations -----------------------------------------------------------

enum AnmKind {
	A_U8,
	A_U16,
	A_F32,
	A_S16,
	A_NTAB,
	A_TPT, // J3DAnmTexPatternFullTable {u16, u16, u8, pad, u16}
	A_REG, // J3DAnm{C,K}RegKeyTable {12 x u16, u8, pad[3]}
	A_VCIDX, // J3DAnmVtxColorIndexData {u16 num, pad, u32 offset}
};

struct AnmLayout {
	u32 tag;
	u16 u16_begin, u16_end; // header u16 fields [begin, end)
	u16 off_begin;          // first u32 offset field
	u8 nsec;
	u8 kind[14];
};

static const AnmLayout kAnm[] = {
	{ 'ANK1', 0x0A, 0x14, 0x14, 4, { A_U16, A_F32, A_S16, A_F32 } },
	{ 'ANF1', 0x0A, 0x14, 0x14, 4, { A_U16, A_F32, A_S16, A_F32 } },
	{ 'PAK1', 0x0C, 0x18, 0x18, 7, { A_U16, A_U16, A_NTAB, A_S16, A_S16, A_S16, A_S16 } },
	{ 'PAF1', 0x0C, 0x18, 0x18, 7, { A_U16, A_U16, A_NTAB, A_U8, A_U8, A_U8, A_U8 } },
	{ 'TPT1', 0x0A, 0x10, 0x10, 4, { A_TPT, A_U16, A_U16, A_NTAB } },
	{ 'CLF1', 0x0A, 0x0C, 0x10, 2, { A_U16, A_F32 } },
	{ 'CLK1', 0x0A, 0x0C, 0x10, 2, { A_U16, A_F32 } },
	{ 'VAF1', 0x0A, 0x10, 0x10, 2, { A_U16, A_U8 } },
	{ 'TTK1', 0x0A, 0x14, 0x14, 8,
	  { A_U16, A_U16, A_NTAB, A_U8, A_F32, A_F32, A_S16, A_F32 } },
	{ 'TRK1', 0x0A, 0x20, 0x20, 14,
	  { A_REG, A_REG, A_U16, A_U16, A_NTAB, A_NTAB, A_S16, A_S16, A_S16, A_S16, A_S16,
	    A_S16, A_S16, A_S16 } },
	{ 'VCF1', 0x0A, 0x14, 0x18, 10,
	  { A_U16, A_U16, A_VCIDX, A_VCIDX, A_U16, A_U16, A_U8, A_U8, A_U8, A_U8 } },
	{ 'VCK1', 0x0A, 0x10, 0x18, 10,
	  { A_U16, A_U16, A_VCIDX, A_VCIDX, A_U16, A_U16, A_S16, A_S16, A_S16, A_S16 } },
};

static void anm_section(const Buf& b, const Section& s)
{
	u32 o = s.off, e = s.end;
	switch (s.kind) {
	case A_U16:
	case A_S16:
		b.swrange(o, e, 2);
		break;
	case A_F32:
		b.swrange(o, e, 4);
		break;
	case A_NTAB:
		ntab(b, o);
		break;
	case A_TPT:
		for (; o + 8 <= e; o += 8) {
			b.sw16(o);
			b.sw16(o + 2);
			b.sw16(o + 6);
		}
		break;
	case A_REG:
		for (; o + 0x1C <= e; o += 0x1C)
			b.swn(o, 12, 2);
		break;
	case A_VCIDX:
		for (; o + 8 <= e; o += 8) {
			b.sw16(o);
			b.sw32(o + 4);
		}
		break;
	default:
		break;
	}
}

static void anm_block(const Buf& b, u32 tag)
{
	const AnmLayout* L = nullptr;
	for (size_t i = 0; i < sizeof(kAnm) / sizeof(kAnm[0]); i++)
		if (kAnm[i].tag == tag)
			L = &kAnm[i];
	if (!L)
		return;
	b.swrange(L->u16_begin, L->u16_end, 2);
	if (tag == 'CLF1' || tag == 'CLK1')
		b.sw32(0x0C);
	if (tag == 'VCF1')
		b.sw32(0x14);
	if (tag == 'VCK1')
		b.swn(0x10, 2, 4);
	Section s[24];
	int n = 0;
	for (int i = 0; i < L->nsec; i++)
		s[n++] = { b.sw32(L->off_begin + i * 4), L->kind[i], 0 };
	if (tag == 'TTK1') {
		// Post-texture-matrix tables (0x34..0x60), present when the header
		// reaches that far (the first data section starts after it).
		u32 first = b.size;
		for (int i = 0; i < n; i++)
			if (s[i].off && s[i].off < first)
				first = s[i].off;
		if (first >= 0x60) {
			b.swn(0x34, 4, 2);
			static const u8 post[9] = { A_U16, A_U16, A_NTAB, A_U8, A_F32, A_F32, A_S16, A_F32, A_U8 };
			for (int i = 0; i < 9; i++)
				s[n++] = { b.sw32(0x3C + i * 4), post[i], 0 };
		}
	}
	n = layout_sections(s, n, b.size);
	for (int i = 0; i < n; i++)
		anm_section(b, s[i]);
}

// --- File level ---------------------------------------------------------------------

static bool j3d_magic_be(const u8* d)
{
	return magic_is(d, "J3D1", 4) || magic_is(d, "J3D2", 4);
}
static bool j3d_magic_native(const u8* d)
{
	return magic_rev(d, "J3D1", 4) || magic_rev(d, "J3D2", 4);
}

extern "C" int port_endian_j3d(void* data, uint32_t size)
{
	Buf f(data, size);
	if (!f.has(0, 0x20) || !j3d_magic_be(f.base))
		return 0;
	f.swn(0x00, 4, 4); // magic, type, file size, block count
	f.sw32(0x1C);      // SE-anim offset (BCK) / padding
	u32 nblocks = ld32(f.at(0x0C));
	u32 pos     = 0x20;
	for (u32 i = 0; i < nblocks && f.has(pos, 8); i++) {
		u32 tag  = f.sw32(pos);
		u32 bsz  = f.sw32(pos + 4);
		if (bsz < 8 || !f.has(pos, bsz))
			bsz = f.size - pos;
		Buf b = f.sub(pos, bsz);
		switch (tag) {
		case 'INF1': inf1(b); break;
		case 'VTX1': vtx1(b); break;
		case 'EVP1': evp1(b); break;
		case 'DRW1': drw1(b); break;
		case 'JNT1': jnt1(b); break;
		case 'SHP1': shp1(b); break;
		case 'MAT3': mat(b, false); break;
		case 'MAT2': mat(b, true); break;
		case 'TEX1': tex1(b); break;
		case 'MDL3': break; // BDL material display lists: not read by this J3D version
		default: anm_block(b, tag); break;
		}
		pos += bsz;
	}
	return 1;
}

// Used by the dispatcher.
bool pe_j3d_is_native(const void* d) { return j3d_magic_native((const u8*)d); }
bool pe_j3d_is_be(const void* d) { return j3d_magic_be((const u8*)d); }
