// MTX/VEC: C implementations of the paired-single matrix library. Matrices are
// row-major 3x4 (implicit last row 0 0 0 1) acting on column vectors, as in the
// SDK; projection matrices follow the GX clip-space convention (z in [-w, 0]).
//
// Precision: the 32-bit x86 build compiles this file for the x87 FPU (the
// platform layer does not take the game's -mfpmath=sse), so its sums of
// products are evaluated in 80-bit extended precision and rounded to f32 once,
// when stored. Its frames are the port's references. The arithmetic is spelled
// out here in that precision (xf, long double: the x87 format on every x86
// host) with the same evaluation order, so the 64-bit build, which would
// otherwise round every operation to f32 in SSE, gives bit-identical matrices.
// The few f32 roundings inside an expression (spill()) are the intermediates
// g++ 13 spilled to f32 stack slots in the 32-bit build. tanf is the host's,
// whose i386 and x86-64 glibc results differ in the last bit for some inputs.
#include "port_compat.h"
#include <dolphin/mtx.h>

#define DEF2(name, args, ...) \
	extern "C" void C_##name args __VA_ARGS__ extern "C" void PS##name args __VA_ARGS__

extern "C" {
void PSMTXMultVecSR(Mtx m, Vec* src, Vec* dst);
void C_MTXMultVecSR(Mtx m, Vec* src, Vec* dst);
}

typedef long double xf;

// An f32 intermediate, rounded through memory: under g++'s default
// -fexcess-precision=fast an x87 build could keep it in extended precision.
static inline f32 spill(xf v)
{
	f32 r = (f32)v;
	__asm__("" : "+m"(r));
	return r;
}

static void identity(Mtx m)
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 4; j++)
			m[i][j] = i == j ? 1.0f : 0.0f;
}
static void concat(Mtx a, Mtx b, Mtx ab)
{
	Mtx t;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 4; j++)
			t[i][j] = (xf)a[i][0] * b[0][j] + (xf)a[i][1] * b[1][j] + (xf)a[i][2] * b[2][j]
			        + (j == 3 ? (xf)a[i][3] : 0.0L);
	}
	memcpy(ab, t, sizeof(Mtx));
}
static u32 inverse(Mtx src, Mtx inv)
{
	f32 a = src[0][0], b = src[0][1], c = src[0][2];
	f32 d = src[1][0], e = src[1][1], f = src[1][2];
	f32 g = src[2][0], h = src[2][1], i = src[2][2];
	xf ei_fh = (xf)e * i - (xf)f * h;
	f32 di = spill((xf)d * i), fg = spill((xf)f * g), dh_eg = spill((xf)d * h - (xf)e * g);
	xf det = ei_fh * a - ((xf)di - fg) * b + (xf)dh_eg * c;
	if (det == 0.0L)
		return 0;
	xf r = 1.0L / det;
	Mtx t;
	t[0][0] = ei_fh * r;
	t[0][1] = ((xf)c * h - (xf)b * i) * r;
	t[0][2] = ((xf)b * f - (xf)c * e) * r;
	t[1][0] = ((xf)fg - di) * r;
	t[1][1] = ((xf)a * i - (xf)c * g) * r;
	t[1][2] = ((xf)c * d - (xf)a * f) * r;
	t[2][0] = (xf)dh_eg * r;
	t[2][1] = ((xf)b * g - (xf)a * h) * r;
	t[2][2] = ((xf)a * e - (xf)b * d) * r;
	for (int k = 0; k < 3; k++)
		t[k][3] = -((xf)t[k][0] * src[0][3] + (xf)t[k][1] * src[1][3] + (xf)t[k][2] * src[2][3]);
	memcpy(inv, t, sizeof(Mtx));
	return 1;
}
static void rottrig(Mtx m, char axis, f32 s, f32 c)
{
	identity(m);
	switch (axis) {
	case 'x':
	case 'X':
		m[1][1] = c;
		m[1][2] = -s;
		m[2][1] = s;
		m[2][2] = c;
		break;
	case 'y':
	case 'Y':
		m[0][0] = c;
		m[0][2] = s;
		m[2][0] = -s;
		m[2][2] = c;
		break;
	case 'z':
	case 'Z':
		m[0][0] = c;
		m[0][1] = -s;
		m[1][0] = s;
		m[1][1] = c;
		break;
	}
}
static void rotaxis(Mtx m, Vec* axis, f32 rad)
{
	f32 s = sinf(rad), c = cosf(rad);
	xf t   = 1.0L - c;
	xf len = sqrtl((xf)axis->x * axis->x + (xf)axis->y * axis->y + (xf)axis->z * axis->z);
	xf x = axis->x / len, y = axis->y / len, z = axis->z / len;
	xf tx = t * x, ty = t * y;
	f32 txz = spill(tx * z);
	m[0][0] = tx * x + c;
	m[0][1] = tx * y - s * z;
	m[0][2] = tx * z + s * y;
	m[0][3] = 0;
	m[1][0] = tx * y + s * z;
	m[1][1] = ty * y + c;
	m[1][2] = ty * z - s * x;
	m[1][3] = 0;
	m[2][0] = txz - s * y;
	m[2][1] = ty * z + s * x;
	m[2][2] = t * z * z + c;
	m[2][3] = 0;
}
static void quat(Mtx m, Quaternion* q)
{
	xf n = (xf)q->x * q->x + (xf)q->y * q->y + (xf)q->z * q->z + (xf)q->w * q->w;
	xf s = n > 0.0L ? 2.0L / n : 0.0L;
	xf xs = q->x * s, ys = q->y * s, zs = q->z * s;
	f32 wx = spill(q->w * xs), wy = spill(q->w * ys), xz = spill(q->x * zs);
	xf wz = q->w * zs;
	xf xx = q->x * xs, xy = q->x * ys;
	xf yy = q->y * ys, yz = q->y * zs, zz = q->z * zs;
	m[0][0] = 1.0L - (yy + zz);
	m[0][1] = xy - wz;
	m[0][2] = (xf)xz + wy;
	m[0][3] = 0;
	m[1][0] = xy + wz;
	m[1][1] = 1.0L - (xx + zz);
	m[1][2] = yz - wx;
	m[1][3] = 0;
	m[2][0] = (xf)xz - wy;
	m[2][1] = yz + wx;
	m[2][2] = 1.0L - (xx + yy);
	m[2][3] = 0;
}

DEF2(MTXIdentity, (Mtx m), { identity(m); })
DEF2(MTXCopy, (Mtx s, Mtx d), { if (s != d) memcpy(d, s, sizeof(Mtx)); })
DEF2(MTXConcat, (Mtx a, Mtx b, Mtx ab), { concat(a, b, ab); })
extern "C" u32 C_MTXInverse(Mtx s, Mtx i) { return inverse(s, i); }
extern "C" u32 PSMTXInverse(Mtx s, Mtx i) { return inverse(s, i); }
DEF2(MTXTranspose, (Mtx s, Mtx x), {
	Mtx t;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			t[i][j] = s[j][i];
	t[0][3] = t[1][3] = t[2][3] = 0;
	memcpy(x, t, sizeof(Mtx));
})
DEF2(MTXRotRad, (Mtx m, char axis, f32 rad), { rottrig(m, axis, sinf(rad), cosf(rad)); })
DEF2(MTXRotTrig, (Mtx m, char axis, f32 s, f32 c), { rottrig(m, axis, s, c); })
DEF2(MTXRotAxisRad, (Mtx m, Vec* axis, f32 rad), { rotaxis(m, axis, rad); })
DEF2(MTXQuat, (Mtx m, Quaternion* q), { quat(m, q); })
DEF2(MTXTrans, (Mtx m, f32 x, f32 y, f32 z), {
	identity(m);
	m[0][3] = x;
	m[1][3] = y;
	m[2][3] = z;
})
DEF2(MTXTransApply, (Mtx s, Mtx d, f32 x, f32 y, f32 z), {
	if (s != d)
		memcpy(d, s, sizeof(Mtx));
	d[0][3] += x;
	d[1][3] += y;
	d[2][3] += z;
})
DEF2(MTXScale, (Mtx m, f32 x, f32 y, f32 z), {
	identity(m);
	m[0][0] = x;
	m[1][1] = y;
	m[2][2] = z;
})
DEF2(MTXScaleApply, (Mtx s, Mtx d, f32 x, f32 y, f32 z), {
	f32 k[3] = { x, y, z };
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 4; j++)
			d[i][j] = s[i][j] * k[i];
})
static void multvec(f32 (*m)[4], Vec* s, Vec* d)
{
	Vec t;
	t.x = (xf)m[0][0] * s->x + (xf)m[0][1] * s->y + (xf)m[0][2] * s->z + m[0][3];
	t.y = (xf)m[1][0] * s->x + (xf)m[1][1] * s->y + (xf)m[1][2] * s->z + m[1][3];
	t.z = (xf)m[2][0] * s->x + (xf)m[2][1] * s->y + (xf)m[2][2] * s->z + m[2][3];
	*d  = t;
}
static void multvecsr(f32 (*m)[4], Vec* s, Vec* d)
{
	Vec t;
	t.x = (xf)m[0][0] * s->x + (xf)m[0][1] * s->y + (xf)m[0][2] * s->z;
	t.y = (xf)m[1][0] * s->x + (xf)m[1][1] * s->y + (xf)m[1][2] * s->z;
	t.z = (xf)m[2][0] * s->x + (xf)m[2][1] * s->y + (xf)m[2][2] * s->z;
	*d  = t;
}
extern "C" void C_MTXMultVec(Mtx44 m, Vec* s, Vec* d) { multvec(m, s, d); }
extern "C" void PSMTXMultVec(Mtx44 m, Vec* s, Vec* d) { multvec(m, s, d); }
extern "C" void C_MTXMultVecSR(Mtx m, Vec* s, Vec* d) { multvecsr(m, s, d); }
extern "C" void PSMTXMultVecSR(Mtx m, Vec* s, Vec* d) { multvecsr(m, s, d); }
DEF2(MTXMultVecArray, (Mtx m, Vec* s, Vec* d, u32 n), {
	for (u32 i = 0; i < n; i++)
		multvec(m, &s[i], &d[i]);
})

// --- VEC ---
DEF2(VECAdd, (Vec* a, Vec* b, Vec* c), {
	c->x = a->x + b->x;
	c->y = a->y + b->y;
	c->z = a->z + b->z;
})
DEF2(VECSubtract, (Vec* a, Vec* b, Vec* c), {
	c->x = a->x - b->x;
	c->y = a->y - b->y;
	c->z = a->z - b->z;
})
DEF2(VECScale, (Vec* s, Vec* d, f32 k), {
	d->x = s->x * k;
	d->y = s->y * k;
	d->z = s->z * k;
})
static xf squaremag(Vec* v) { return (xf)v->x * v->x + (xf)v->y * v->y + (xf)v->z * v->z; }
DEF2(VECNormalize, (Vec* s, Vec* d), {
	xf r = 1.0L / sqrtl(squaremag(s));
	d->x = s->x * r;
	d->y = s->y * r;
	d->z = s->z * r;
})
extern "C" f32 C_VECSquareMag(Vec* v) { return squaremag(v); }
extern "C" f32 PSVECSquareMag(Vec* v) { return C_VECSquareMag(v); }
extern "C" f32 C_VECMag(Vec* v) { return sqrtl(squaremag(v)); }
extern "C" f32 PSVECMag(Vec* v) { return C_VECMag(v); }
static xf dot(Vec* a, Vec* b) { return (xf)a->x * b->x + (xf)a->y * b->y + (xf)a->z * b->z; }
extern "C" f32 C_VECDotProduct(Vec* a, Vec* b) { return dot(a, b); }
extern "C" f32 PSVECDotProduct(Vec* a, Vec* b) { return C_VECDotProduct(a, b); }
DEF2(VECCrossProduct, (Vec* a, Vec* b, Vec* c), {
	Vec t;
	t.x = (xf)a->y * b->z - (xf)a->z * b->y;
	t.y = (xf)a->z * b->x - (xf)a->x * b->z;
	t.z = (xf)a->x * b->y - (xf)a->y * b->x;
	*c  = t;
})
static xf squaredistance(Vec* a, Vec* b)
{
	xf x = (xf)a->x - b->x, y = (xf)a->y - b->y, z = (xf)a->z - b->z;
	return x * x + y * y + z * z;
}
extern "C" f32 C_VECSquareDistance(Vec* a, Vec* b) { return squaredistance(a, b); }
extern "C" f32 PSVECSquareDistance(Vec* a, Vec* b) { return C_VECSquareDistance(a, b); }
extern "C" f32 C_VECDistance(Vec* a, Vec* b) { return sqrtl(squaredistance(a, b)); }
extern "C" f32 PSVECDistance(Vec* a, Vec* b) { return C_VECDistance(a, b); }

// --- Viewing and projection ---
extern "C" void C_MTXLookAt(Mtx m, Point3dPtr camPos, VecPtr camUp, Point3dPtr target)
{
	// look = normalize(camPos - target), right = normalize(camUp x look),
	// up = look x right
	xf lx = (xf)camPos->x - target->x, ly = (xf)camPos->y - target->y, lz = (xf)camPos->z - target->z;
	xf r = 1.0L / sqrtl(lx * lx + ly * ly + lz * lz);
	lx *= r;
	ly *= r;
	lz *= r;
	xf rx = camUp->y * lz - camUp->z * ly, ry = camUp->z * lx - camUp->x * lz, rz = camUp->x * ly - camUp->y * lx;
	r = 1.0L / sqrtl(rx * rx + ry * ry + rz * rz);
	rx *= r;
	ry *= r;
	rz *= r;
	f32 ux = spill(ly * rz - lz * ry), lxry = spill(lx * ry);
	xf uy = lz * rx - lx * rz, uz = lxry - ly * rx;
	m[0][0] = rx;
	m[0][1] = ry;
	m[0][2] = rz;
	m[0][3] = -(rx * camPos->x + ry * camPos->y + rz * camPos->z);
	m[1][0] = ux;
	m[1][1] = uy;
	m[1][2] = uz;
	m[1][3] = -((xf)ux * camPos->x + uy * camPos->y + uz * camPos->z);
	m[2][0] = lx;
	m[2][1] = ly;
	m[2][2] = lz;
	m[2][3] = -(lx * camPos->x + ly * camPos->y + lz * camPos->z);
}
extern "C" void C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f)
{
	xf cot = 1.0L / tanf(fovY * 0.5f * (3.14159265358979323846f / 180.0f));
	memset(m, 0, sizeof(Mtx44));
	xf r    = 1.0L / ((xf)f - n);
	m[0][0] = cot / aspect;
	m[1][1] = cot;
	m[2][2] = -n * r;
	m[2][3] = -((xf)f * n) * r;
	m[3][2] = -1.0f;
}
extern "C" void C_MTXFrustum(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
{
	memset(m, 0, sizeof(Mtx44));
	m[0][0] = 2 * (xf)n / ((xf)r - l);
	m[0][2] = ((xf)r + l) / ((xf)r - l);
	m[1][1] = 2 * (xf)n / ((xf)t - b);
	m[1][2] = ((xf)t + b) / ((xf)t - b);
	m[2][2] = -(xf)n / ((xf)f - n);
	m[2][3] = -((xf)f * n) / ((xf)f - n);
	m[3][2] = -1.0f;
}
extern "C" void C_MTXOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
{
	memset(m, 0, sizeof(Mtx44));
	m[0][0] = 2.0L / ((xf)r - l);
	m[0][3] = -((xf)r + l) / ((xf)r - l);
	m[1][1] = 2.0L / ((xf)t - b);
	m[1][3] = -((xf)t + b) / ((xf)t - b);
	m[2][2] = -1.0L / ((xf)f - n);
	m[2][3] = -(xf)f / ((xf)f - n);
	m[3][3] = 1.0f;
}
extern "C" void C_MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 sS, f32 sT, f32 tS, f32 tT)
{
	xf cot = 1.0L / tanf(fovY * 0.5f * (3.14159265358979323846f / 180.0f));
	memset(m, 0, sizeof(Mtx));
	m[0][0] = cot / aspect * sS;
	m[0][2] = -tS;
	m[1][1] = cot * sT;
	m[1][2] = -tT;
	m[2][2] = -1.0f;
}
extern "C" void C_MTXLightFrustum(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 sS, f32 sT, f32 tS, f32 tT)
{
	memset(m, 0, sizeof(Mtx));
	m[0][0] = 2 * (xf)n / ((xf)r - l) * sS;
	m[0][2] = ((xf)r + l) / ((xf)r - l) * sS - tS;
	m[1][1] = 2 * (xf)n / ((xf)t - b) * sT;
	m[1][2] = ((xf)t + b) / ((xf)t - b) * sT - tT;
	m[2][2] = -1.0f;
}
extern "C" void C_MTXLightOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 sS, f32 sT, f32 tS, f32 tT)
{
	memset(m, 0, sizeof(Mtx));
	m[0][0] = 2.0L / ((xf)r - l) * sS;
	m[0][3] = -((xf)r + l) / ((xf)r - l) * sS + tS;
	m[1][1] = 2.0L / ((xf)t - b) * sT;
	m[1][3] = -((xf)t + b) / ((xf)t - b) * sT + tT;
	m[2][3] = 1.0f;
}
