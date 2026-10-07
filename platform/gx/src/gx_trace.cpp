// SMS_GX_TRACE_FRAME=n: writes every draw of display frame n (the draws after
// the n-th GXCopyDisp) with the state that shapes it to a text file:
// $SMS_GX_TRACE_FILE, or $SMS_GX_DUMP_DIR/gx_trace_frame<n>.txt (default ".").
// SMS_GX_TRACE_FRAME=a-b traces a range of frames into one file per frame.
#include "gx_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <execinfo.h>

namespace gx {

static int s_first = -2, s_last = -2;
static uint32_t s_curFrame = 0;
static FILE* s_file = nullptr;
static uint32_t s_drawNo = 0;
uint32_t g_traceLastVat = 0;

static void traceInit() {
    s_first = s_last = -1;
    const char* e = getenv("SMS_GX_TRACE_FRAME");
    if (!e || !*e) return;
    s_first = atoi(e);
    const char* dash = strchr(e, '-');
    s_last = dash ? atoi(dash + 1) : s_first;
}

static void openFor(uint32_t frame) {
    if (s_file) {
        fclose(s_file);
        s_file = nullptr;
    }
    if (int(frame) < s_first || int(frame) > s_last) return;
    char path[1024];
    const char* f = getenv("SMS_GX_TRACE_FILE");
    if (f && s_first == s_last) snprintf(path, sizeof path, "%s", f);
    else {
        const char* dir = getenv("SMS_GX_DUMP_DIR");
        snprintf(path, sizeof path, "%s/gx_trace_frame%u.txt", dir ? dir : ".", frame);
    }
    s_file = fopen(path, "w");
    if (s_file) {
        fprintf(s_file, "# sms_gx trace of display frame %u\n", frame);
        logmsg("tracing frame %u to %s", frame, path);
    }
    s_drawNo = 0;
}

FILE* traceFile() {
    if (s_first == -2) {
        traceInit();
        openFor(0);
    }
    return s_file;
}

void traceFrameAdvance() {
    if (s_first == -2) traceInit();
    s_curFrame++;
    if (s_first >= 0) openFor(s_curFrame);
}

static const char* kCArg[16] = {"CPREV", "APREV", "C0", "A0", "C1", "A1", "C2", "A2",
                                "TEXC", "TEXA", "RASC", "RASA", "ONE", "HALF", "KONST", "ZERO"};
static const char* kAArg[8] = {"APREV", "A0", "A1", "A2", "TEXA", "RASA", "KONST", "ZERO"};
static const char* kReg[4] = {"PREV", "REG0", "REG1", "REG2"};
static const char* kCmp[8] = {"NEVER", "LESS", "EQUAL", "LEQUAL", "GREATER", "NEQUAL", "GEQUAL", "ALWAYS"};

static void envStr(char* out, size_t n, uint32_t env, bool alpha) {
    uint32_t bias = (env >> 16) & 3, op = (env >> 18) & 1, clampv = (env >> 19) & 1, scale = (env >> 20) & 3,
             dest = (env >> 22) & 3;
    const char *a, *b, *c, *d;
    if (alpha) {
        d = kAArg[(env >> 4) & 7]; c = kAArg[(env >> 7) & 7]; b = kAArg[(env >> 10) & 7]; a = kAArg[(env >> 13) & 7];
    } else {
        d = kCArg[env & 15]; c = kCArg[(env >> 4) & 15]; b = kCArg[(env >> 8) & 15]; a = kCArg[(env >> 12) & 15];
    }
    if (bias == 3)
        snprintf(out, n, "%s = %s + (cmp%u(%s,%s) ? %s : 0)%s", kReg[dest], d, (scale << 1) | op, a, b, c, clampv ? " clamp" : "");
    else
        snprintf(out, n, "%s = (%s %c lerp(%s,%s,%s)%s)*%s%s", kReg[dest], d, op ? '-' : '+', a, b, c,
                 bias == 1 ? "+0.5" : bias == 2 ? "-0.5" : "", scale == 0 ? "1" : scale == 1 ? "2" : scale == 2 ? "4" : "0.5",
                 clampv ? " clamp" : "");
}

static int sext11(uint32_t v) { return int32_t(v << 21) >> 21; }
static float xfregf(int r) {
    float f;
    memcpy(&f, &g.xfReg[r], 4);
    return f;
}

// SMS_GX_TRACE_BT=1: each traced draw also lists the host call stack of the
// last GXBegin before it (module+offset; resolve with addr2line -e sms).
static void* s_beginBt[12];
static int s_beginBtN = 0;
void traceNoteBegin() {
    static int on = -1;
    if (on < 0) on = getenv("SMS_GX_TRACE_BT") != nullptr;
    if (!on || !s_file) return;
    s_beginBtN = backtrace(s_beginBt, 12);
}

// With SMS_GX_TRACE_BT, projection loads are logged with their caller too.
void traceNoteProjection(const float* p) {
    static int on = -1;
    if (on < 0) on = getenv("SMS_GX_TRACE_BT") != nullptr;
    if (!on || !s_file) return;
    void* bt[8];
    int n = backtrace(bt, 8);
    char** names = backtrace_symbols(bt, n);
    fprintf(s_file, "\n== projection %s %g %g %g %g %g %g caller:", p[0] != 0.0f ? "ORTHO" : "PERSP", p[1], p[2], p[3],
            p[4], p[5], p[6]);
    for (int i = 1; names && i < n; i++) {
        const char* nm = strrchr(names[i], '/');
        fprintf(s_file, " %s", nm ? nm + 1 : names[i]);
    }
    fprintf(s_file, "\n");
    free(names);
}

void traceDraw(int prim, uint32_t nverts, uint32_t nidx, const HostVertex* v, int progId) {
    FILE* f = s_file;
    if (!f) return;
    static const char* kPrim[3] = {"TRIS", "LINES", "POINTS"};
    fprintf(f, "\n== draw %u: %s, %u vertices, %u indices, program %d\n", s_drawNo++, kPrim[prim], nverts, nidx, progId);
    if (s_beginBtN > 0) {
        char** names = backtrace_symbols(s_beginBt, s_beginBtN);
        fprintf(f, "  caller:");
        for (int i = 1; names && i < s_beginBtN; i++) {
            const char* n = strrchr(names[i], '/');
            fprintf(f, " %s", n ? n + 1 : names[i]);
        }
        fprintf(f, "\n");
        free(names);
        s_beginBtN = 0;
    }
    uint32_t gen = g.bp[BP_GENMODE];
    uint32_t nst = ((gen >> 10) & 15) + 1, nind = (gen >> 16) & 7;
    fprintf(f, "  genmode: texgens=%u chans=%u tevstages=%u indstages=%u cull=%u | xf texgens=%u chans=%u\n", gen & 15,
            (gen >> 4) & 7, nst, nind, (gen >> 14) & 3, g.xfReg[XFR_NUMTEXGENS], g.xfReg[XFR_NUMCHANS]);
    fprintf(f, "  proj: %s %.4g %.4g %.4g %.4g %.4g %.4g  viewport: scale %.1f %.1f %.1f off %.1f %.1f %.1f  clip=%s\n",
            g.xfReg[XFR_PROJ + 6] ? "ORTHO" : "PERSP", xfregf(XFR_PROJ), xfregf(XFR_PROJ + 1), xfregf(XFR_PROJ + 2),
            xfregf(XFR_PROJ + 3), xfregf(XFR_PROJ + 4), xfregf(XFR_PROJ + 5), xfregf(XFR_VIEWPORT),
            xfregf(XFR_VIEWPORT + 1), xfregf(XFR_VIEWPORT + 2), xfregf(XFR_VIEWPORT + 3) - 342, xfregf(XFR_VIEWPORT + 4) - 342,
            xfregf(XFR_VIEWPORT + 5), g.xfReg[5] ? "off" : "on");
    uint32_t tl = g.bp[BP_SCISSOR_TL], br = g.bp[BP_SCISSOR_BR];
    fprintf(f, "  scissor: %d,%d - %d,%d\n", int((tl >> 12) & 0x7FF) - 342, int(tl & 0x7FF) - 342,
            int((br >> 12) & 0x7FF) - 342, int(br & 0x7FF) - 342);
    uint32_t z = g.bp[BP_ZMODE], cm = g.bp[BP_CMODE0], cm1 = g.bp[BP_CMODE1], pe = g.bp[BP_PE_CONTROL];
    fprintf(f, "  z: %s %s %s  zcomploc=%s  pixfmt=%u\n", z & 1 ? "test" : "notest", kCmp[(z >> 1) & 7],
            (z >> 4) & 1 ? "write" : "nowrite", (pe >> 6) & 1 ? "early" : "late", pe & 7);
    fprintf(f, "  blend: en=%u logic=%u sub=%u src=%u dst=%u lop=%u cupd=%u aupd=%u dstalpha=%s%u\n", cm & 1,
            (cm >> 1) & 1, (cm >> 11) & 1, (cm >> 8) & 7, (cm >> 5) & 7, (cm >> 12) & 15, (cm >> 3) & 1, (cm >> 4) & 1,
            (cm1 >> 8) & 1 ? "on:" : "off:", cm1 & 0xFF);
    uint32_t ac = g.bp[BP_ALPHACOMPARE];
    fprintf(f, "  alphacmp: %s %u %s %s %u\n", kCmp[(ac >> 16) & 7], ac & 0xFF,
            ((const char*[]){"AND", "OR", "XOR", "XNOR"})[(ac >> 22) & 3], kCmp[(ac >> 19) & 7], (ac >> 8) & 0xFF);
    uint32_t f3 = g.bp[BP_FOG3];
    if ((f3 >> 21) & 7) {
        auto fogFloat = [](uint32_t r) {
            uint32_t bits = ((r >> 19) & 1) << 31 | ((r >> 11) & 0xFF) << 23 | (r & 0x7FF) << 12;
            float v;
            memcpy(&v, &bits, 4);
            return v;
        };
        int bs = int(g.bp[BP_FOG2] & 31);
        fprintf(f, "  fog: type %u color %06X A=%g B=%g C=%g proj=%u rangeadj=%03X\n", (f3 >> 21) & 7,
                g.bp[BP_FOG_COLOR], ldexp(fogFloat(g.bp[BP_FOG0]), bs),
                ldexp(double(g.bp[BP_FOG1] & 0xFFFFFF) / 8388638.0, bs - 1), fogFloat(f3), (f3 >> 20) & 1,
                g.bp[BP_FOG_RANGE] & 0x7FF);
    }
    for (int c = 0; c < 2; c++)
        fprintf(f, "  chan%d: color ctrl %04X alpha ctrl %04X amb %08X mat %08X\n", c, g.xfReg[XFR_COLOR0CTRL + c],
                g.xfReg[XFR_ALPHA0CTRL + c], g.xfReg[XFR_AMB0 + c], g.xfReg[XFR_MAT0 + c]);
    uint32_t lmask = 0;
    for (int c = 0; c < 4; c++) {
        uint32_t ctrl = g.xfReg[XFR_COLOR0CTRL + c];
        if (ctrl & 2) lmask |= ((ctrl >> 2) & 15) | ((ctrl >> 11) & 15) << 4;
    }
    for (int l = 0; l < 8; l++) {
        if (!(lmask & (1u << l))) continue;
        const uint32_t* w = &g.xfMem[0x600 + l * 16];
        float v[12];
        memcpy(v, w + 4, sizeof v);
        fprintf(f, "  light%d: color %08X a(%g %g %g) k(%g %g %g) pos(%g %g %g) dir(%g %g %g)\n", l, w[3], v[0], v[1],
                v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
    }
    for (uint32_t i = 0; i < g.xfReg[XFR_NUMTEXGENS] && i < 8; i++) {
        uint32_t tg = g.xfReg[XFR_TEXGEN + i];
        fprintf(f, "  texgen%u: %s type=%u src_row=%u form=%s post=%03X (dualtex=%u)\n", i, (tg >> 1) & 1 ? "3x4" : "2x4",
                (tg >> 4) & 7, (tg >> 7) & 31, (tg >> 2) & 1 ? "ABC1" : "AB11", g.xfReg[XFR_POSTTEX + i],
                g.xfReg[XFR_DUALTEX] & 1);
    }
    for (uint32_t s = 0; s < nst; s++) {
        uint32_t ord = (g.bp[BP_TREF + (s >> 1)] >> ((s & 1) * 12)) & 0x3FF;
        uint32_t ce = g.bp[BP_TEV_COLOR_ENV + 2 * s], ae = g.bp[BP_TEV_COLOR_ENV + 2 * s + 1];
        uint32_t ks = g.bp[BP_TEV_KSEL + (s >> 1)];
        uint32_t kc = (s & 1) ? (ks >> 14) & 31 : (ks >> 4) & 31, ka = (s & 1) ? (ks >> 19) & 31 : (ks >> 9) & 31;
        char cs[160], as[160];
        envStr(cs, sizeof cs, ce, false);
        envStr(as, sizeof as, ae, true);
        fprintf(f, "  stage%u: map=%u coord=%u tex=%s chan=%u kc=%02X ka=%02X swap ras=%u tex=%u ind=%06X\n", s, ord & 7,
                (ord >> 3) & 7, (ord >> 6) & 1 ? "on" : "off", (ord >> 7) & 7, kc, ka, ae & 3, (ae >> 2) & 3,
                nind ? g.bp[BP_IND_CMD + s] & 0xFFFFFF : 0);
        fprintf(f, "     color: %s\n     alpha: %s\n", cs, as);
    }
    fprintf(f, "  tevregs:");
    for (int i = 0; i < 4; i++) {
        uint32_t ra = g.bp[BP_TEV_REG + 2 * i], bg = g.bp[BP_TEV_REG + 2 * i + 1];
        fprintf(f, " %s(%d,%d,%d,%d)", kReg[i], sext11(ra & 0x7FF), sext11((bg >> 12) & 0x7FF), sext11(bg & 0x7FF),
                sext11((ra >> 12) & 0x7FF));
    }
    fprintf(f, "\n  konst:");
    for (int i = 0; i < 4; i++) {
        uint32_t ra = g.kreg[2 * i], bg = g.kreg[2 * i + 1];
        fprintf(f, " K%d(%u,%u,%u,%u)", i, ra & 0xFF, (bg >> 12) & 0xFF, bg & 0xFF, (ra >> 12) & 0xFF);
    }
    fprintf(f, "\n  swaptables:");
    for (int t = 0; t < 4; t++) {
        uint32_t k0 = g.bp[BP_TEV_KSEL + 2 * t], k1 = g.bp[BP_TEV_KSEL + 2 * t + 1];
        fprintf(f, " %c%c%c%c", "RGBA"[k0 & 3], "RGBA"[(k0 >> 2) & 3], "RGBA"[k1 & 3], "RGBA"[(k1 >> 2) & 3]);
    }
    fprintf(f, "\n");
    uint32_t used = 0;
    for (uint32_t s = 0; s < nst; s++) {
        uint32_t ord = (g.bp[BP_TREF + (s >> 1)] >> ((s & 1) * 12)) & 0x3FF;
        if ((ord >> 6) & 1) used |= 1u << (ord & 7);
    }
    for (uint32_t i = 0; i < nind && i < 4; i++) used |= 1u << ((g.bp[BP_RAS1_IREF] >> (6 * i)) & 7);
    for (int m = 0; m < 8; m++) {
        if (!(used & (1u << m))) continue;
        uint32_t img0 = g.bp[bpTexReg(BP_TX_IMAGE0, m)], m0 = g.bp[bpTexReg(BP_TX_MODE0, m)],
                 m1 = g.bp[bpTexReg(BP_TX_MODE1, m)], tl2 = g.bp[bpTexReg(BP_TX_TLUT, m)];
        int cw = 0, ch = 0;
        bool copy = efbCopyLookup(g.texImage[m], &cw, &ch) != 0;
        fprintf(f, "  texmap%d: %p fmt=%u %ux%u wrap=%u/%u filt=mag%u min%u lod=%u..%u bias=%d tlut=%03X/%u%s\n", m,
                (const void*)g.texImage[m], (img0 >> 20) & 15, (img0 & 0x3FF) + 1, ((img0 >> 10) & 0x3FF) + 1, m0 & 3,
                (m0 >> 2) & 3, (m0 >> 4) & 1, (m0 >> 5) & 7, m1 & 0xFF, (m1 >> 8) & 0xFF, int(int8_t((m0 >> 9) & 0xFF)),
                tl2 & 0x3FF, (tl2 >> 10) & 3, copy ? " [EFB copy]" : "");
    }
    fprintf(f, "  vcd: lo=%05X hi=%04X  matidx A=%08X B=%08X  vat%u: %08X %08X %08X\n", g.cpVcdLo, g.cpVcdHi,
            g.xfReg[XFR_MATIDX_A], g.xfReg[XFR_MATIDX_B], g_traceLastVat, g.cpVatA[g_traceLastVat],
            g.cpVatB[g_traceLastVat], g.cpVatC[g_traceLastVat]);
    for (int a = 0; a < 12; a++) {
        if (!g.arrayBase[a]) continue;
        uint32_t mode = a == 0 ? (g.cpVcdLo >> 9) & 3 : a == 1 ? (g.cpVcdLo >> 11) & 3 : a < 4 ? (g.cpVcdLo >> (13 + 2 * (a - 2))) & 3
                                                                                    : (g.cpVcdHi >> (2 * (a - 4))) & 3;
        if (mode < 2) continue;
        const uint8_t* b = g.arrayBase[a];
        fprintf(f, "  array%d: %p stride %u %s bytes %02X %02X %02X %02X %02X %02X %02X %02X\n", a, (const void*)b,
                g.arrayStride[a], g.arrayBigEndian[a] ? "BE" : "host", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    }
    for (uint32_t i = 0; i < nverts && i < 3; i++) {
        const HostVertex& hv = v[i];
        fprintf(f, "  v%u: pos(%.3f %.3f %.3f) nrm(%.2f %.2f %.2f) c0(%u %u %u %u) t0(%.3f %.3f) t1(%.3f %.3f) mtx %u/%u\n", i,
                hv.pos[0], hv.pos[1], hv.pos[2], hv.nrm[0], hv.nrm[1], hv.nrm[2], hv.clr[0][0], hv.clr[0][1],
                hv.clr[0][2], hv.clr[0][3], hv.tex[0][0], hv.tex[0][1], hv.tex[1][0], hv.tex[1][1], hv.mtx[0], hv.mtx[1]);
    }
    fflush(f);
}

// SMS_GX_TRACE_PROBE=x,y[;x,y...]: after each traced draw, log the EFB colour at those points.
void traceProbe() {
    FILE* f = s_file;
    if (!f) return;
    static int n = -1;
    static int pts[16][2];
    if (n < 0) {
        n = 0;
        const char* e = getenv("SMS_GX_TRACE_PROBE");
        while (e && *e && n < 16) {
            int x, y;
            if (sscanf(e, "%d,%d", &x, &y) != 2) break;
            pts[n][0] = x;
            pts[n][1] = y;
            n++;
            e = strchr(e, ';');
            if (e) e++;
        }
    }
    if (!n) return;
    fprintf(f, "  probe:");
    for (int i = 0; i < n; i++) {
        FILE* saved = s_file;
        s_file = nullptr;  // keep the peeks' own trace lines out of the probe
        uint32_t c = peekColor(pts[i][0], pts[i][1]);
        uint32_t z = peekZ(pts[i][0], pts[i][1]);
        s_file = saved;
        fprintf(f, " (%d,%d)=%02X%02X%02X/%02X z=%06X", pts[i][0], pts[i][1], (c >> 16) & 255, (c >> 8) & 255, c & 255,
                c >> 24, z);
    }
    fprintf(f, "\n");
    fflush(f);
}

void traceCopy(bool disp, int x, int y, int w, int h, const void* dest, uint32_t ctrl) {
    FILE* f = s_file;
    if (!f) return;
    fprintf(f, "\n== %s copy src %d,%d %dx%d -> %p ctrl=%06X (fmt %u%s%s%s)\n", disp ? "display" : "texture", x, y, w, h,
            dest, ctrl, ((ctrl >> 3) & 1) << 3 | ((ctrl >> 4) & 7), ((ctrl >> 15) & 3) == 3 ? " intensity" : "",
            (ctrl >> 9) & 1 ? " half" : "", (ctrl >> 11) & 1 ? " clear" : "");
    if ((ctrl >> 11) & 1)
        fprintf(f, "  clear writes: colour=%u alpha=%u depth=%u pixfmt=%u\n",
                (g.bp[BP_CMODE0] >> 3) & 1, (g.bp[BP_CMODE0] >> 4) & 1,
                (g.bp[BP_ZMODE] >> 4) & 1, g.bp[BP_PE_CONTROL] & 7);
    fflush(f);
}

}  // namespace gx
