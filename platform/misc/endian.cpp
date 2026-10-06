// Byte-order conversion of on-disc structures that the game reads in place.
#include "port_compat.h"
#include "port_platform.h"

namespace {
inline void sw32(void* p) { *(u32*)p = port_bswap32(*(u32*)p); }
inline void sw16(void* p) { *(u16*)p = port_bswap16(*(u16*)p); }
}

// RARC layout: 0x20-byte header (8 x u32), then the info block at
// header_length: 6 x u32 + u16 next free id; node table (16 bytes each:
// u32 type, u32 name offset, u16 hash, u16 file count, u32 first file) and
// file entry table (20 bytes each: u16 id, u16 hash, u32 flags|name offset,
// u32 data offset, u32 size, u32 runtime pointer), both relative to the info block.
extern "C" void port_rarc_header_to_native(void* hdr)
{
	u8* h = (u8*)hdr;
	if (memcmp(h, "RARC", 4) != 0)
		return; // already native (or not a RARC)
	for (int i = 0; i < 8; i++)
		sw32(h + i * 4);
}

extern "C" void port_rarc_info_to_native(void* info)
{
	u8* in = (u8*)info;
	// A native info block has a sane node count in host order already; the
	// big-endian one reads as a huge number for any count < 0x01000000.
	u32 nodes_be = port_be32(in);
	u32 nodes_ne = *(u32*)in;
	if (nodes_ne < nodes_be)
		return;
	for (int i = 0; i < 6; i++)
		sw32(in + i * 4);
	sw16(in + 0x18);
	u32 num_nodes = *(u32*)(in + 0x00), node_off = *(u32*)(in + 0x04);
	u32 num_files = *(u32*)(in + 0x08), file_off = *(u32*)(in + 0x0C);
	for (u32 i = 0; i < num_nodes; i++) {
		u8* n = in + node_off + i * 16;
		sw32(n + 0);
		sw32(n + 4);
		sw16(n + 8);
		sw16(n + 10);
		sw32(n + 12);
	}
	for (u32 i = 0; i < num_files; i++) {
		u8* f = in + file_off + i * 20;
		sw16(f + 0);
		sw16(f + 2);
		sw32(f + 4);
		sw32(f + 8);
		sw32(f + 12);
		sw32(f + 16);
	}
}

extern "C" void port_rarc_to_native(void* arc)
{
	u8* a = (u8*)arc;
	if (memcmp(a, "RARC", 4) != 0)
		return;
	port_rarc_header_to_native(a);
	port_rarc_info_to_native(a + *(u32*)(a + 8));
}

// --- Resource formats -----------------------------------------------------------

namespace {

// BFN font: "FONTbfn1", u32 file size, u32 block count, 16 bytes padding, then
// blocks (u32 magic, u32 size, ...): INF1, WID1 (u8 pairs), GLY1 (texture), MAP1
// (u16 table).
void bfn_to_native(u8* f)
{
	u64 m = *(u64*)f;
	*(u64*)f = __builtin_bswap64(m);
	sw32(f + 8);
	sw32(f + 12);
	u32 nblocks = *(u32*)(f + 12);
	u8* b       = f + 0x20;
	for (u32 i = 0; i < nblocks; i++) {
		sw32(b);
		sw32(b + 4);
		u32 type = *(u32*)b, size = *(u32*)(b + 4);
		switch (type) {
		case 'INF1':
			for (int k = 0; k < 6; k++)
				sw16(b + 8 + k * 2);
			break;
		case 'WID1':
			sw16(b + 8);
			sw16(b + 10);
			break;
		case 'GLY1':
			sw16(b + 8);
			sw16(b + 10);
			sw16(b + 12);
			sw16(b + 14);
			sw32(b + 16);
			for (int k = 0; k < 6; k++)
				sw16(b + 20 + k * 2);
			break;
		case 'MAP1':
			for (u32 k = 8; k + 2 <= size; k += 2)
				sw16(b + k);
			break;
		}
		b += size;
	}
}

struct Unknown {
	char magic[9];
};
Unknown s_seen[64];
int s_nseen;

} // namespace

// platform/endian (the resource converters) is linked when present.
extern "C" __attribute__((weak)) int port_endian_resource(void* data, uint32_t size, const char* name);
extern "C" __attribute__((weak)) void GXPC_PrefetchResource(const void* data, uint32_t size, const char* name);

extern "C" void port_res_to_native(void* data, u32 size) { port_res_to_native_named(data, size, NULL); }

extern "C" void port_res_to_native_named(void* data, u32 size, const char* name)
{
	if (GXPC_PrefetchResource)
		GXPC_PrefetchResource(data, size, name);
	if (port_endian_resource && port_endian_resource(data, size, name) != 0)
		return; // recognised by platform/endian (PE_FMT_UNKNOWN is 0)
	u8* d = (u8*)data;
	if (!d || size < 8)
		return;
	if (memcmp(d, "FONTbfn1", 8) == 0) {
		bfn_to_native(d);
		return;
	}
	if (memcmp(d, "1nfbTNOF", 8) == 0)
		return; // BFN already converted
	if (memcmp(d, "RARC", 4) == 0) {
		port_rarc_to_native(d);
		return;
	}
	// Not converted (yet): log each distinct magic once.
	char m[9];
	for (int i = 0; i < 8; i++)
		m[i] = (d[i] >= 0x20 && d[i] < 0x7F) ? (char)d[i] : '.';
	m[8] = 0;
	for (int i = 0; i < s_nseen; i++)
		if (memcmp(s_seen[i].magic, m, 8) == 0)
			return;
	if (s_nseen < 64)
		memcpy(s_seen[s_nseen++].magic, m, 9);
	port_log("[endian] resource format not converted: '%s' (%u bytes%s%s)\n", m, size, name ? ", " : "", name ? name : "");
}
