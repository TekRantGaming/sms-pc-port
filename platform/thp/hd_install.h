// HD cutscene installation helpers for the launcher (hd_install.cpp).
#ifndef PORT_HD_INSTALL_H
#define PORT_HD_INSTALL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PortHdMovie {
	char disc_path[64];   // "data/openingA.thp"
	char patch_file[64];  // "openingA.smpatch"
	char url[256];        // where the patch is published
	char source_sha256[65], target_sha256[65], patch_sha256[65];
	long long target_bytes, patch_bytes;
	int width, height;
} PortHdMovie;

// The movies of the embedded catalog (tools/media/cutscene-release.json);
// returns how many. `release` receives its release name.
int port_hd_catalog(PortHdMovie* out, int max, char* release, int releaseSize);
// The embedded catalog itself (written to the pack as installed.json).
const char* port_hd_catalog_json(void);
// SHA-256 of a file as lowercase hex; 0 when it cannot be read.
int port_hd_sha256_file(const char* path, char hex[65]);
// Reads the movie from the disc image (a path, or "bundled" for the image in
// the executable), checks it, applies the patch and writes outPath, checked
// against the catalog. 1 on success; otherwise 0 and a message in err.
int port_hd_apply(const char* disc, const PortHdMovie* movie, const char* patchPath, const char* outPath,
                  char* err, int errSize);

#ifdef __cplusplus
}
#endif
#endif
