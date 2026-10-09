/* Included by msl_math.c, once or twice (see MSL_HOT there). */
MSL_HOT_ATTR float MSL_HOT(sms_msl_expf)(float x)
{
	int32_t n;
	uint32_t index;
	float pow2, f, p;
	if (x > expf_max.f)
		return float_huge.f;
	if (x < expf_min.f)
		return 0.0f;
	n     = fctiwz(x); /* NaN gets here: fctiwz gives INT_MIN */
	index = (uint32_t)n + 88;
	pow2  = u2f((index + 39) << 23); /* 2^n */
	f     = x - (float)n;
	p     = HOT_FMADDS(f, exp_to_x[7].f, exp_to_x[6].f);
	p     = HOT_FMADDS(f, p, exp_to_x[5].f);
	p     = HOT_FMADDS(f, p, exp_to_x[4].f);
	p     = HOT_FMADDS(f, p, exp_to_x[3].f);
	p     = HOT_FMADDS(f, p, exp_to_x[2].f);
	p     = HOT_FMADDS(f, p, exp_to_x[1].f);
	p     = HOT_FMADDS(f, p, exp_to_x[0].f);
	p     = f * p;
	p     = expf_c2.f + p;
	p     = expf_c1.f + p;
	p     = pow2 * p;
	/* slwi by 2 wraps: a NaN's index 0x80000058 reads entry 88 */
	return two_to_log2e_m1_tI[index & 0x3fffffff].f * p;
}

/* The inline __log2f, as powf expands it (x > 0). */
MSL_HOT_ATTR static float MSL_HOT(msl_log2f)(float x)
{
	const uint32_t b = f2u(x);
	uint32_t index   = (b >> 16) & 0x7f;
	const float e    = (float)((int32_t)(b >> 23) - 128);
	if (b & 0xffff) {
		uint32_t hi      = (b & 0x7f0000) | 0x3f800000;
		const uint32_t lo = (b & 0x7fffff) | 0x3f800000;
		float fr, fr2, p;
		if (b & 0x8000) {
			index++;
			hi += 0x10000;
		}
		fr  = u2f(lo) - u2f(hi);
		fr  = fr * one_over_F[index].f;
		fr2 = fr * fr;
		p   = HOT_FMADDS(fr, log2_poly[1].f, log2_poly[0].f);
		p   = fr2 * p;
		p   = HOT_FMADDS(log2e_m1[1].f, fr, p);
		p   = HOT_FMADDS(log2e_m1[0].f, fr, p);
		p   = fr + p;
		p   = log2_F[index].f + p;
		return (log2_bias.f + e) + p;
	}
	return (log2_bias.f + e) + log2_F[index].f;
}

/* The inline __exp2f, as powf expands it. */
MSL_HOT_ATTR static float MSL_HOT(msl_exp2f)(float t)
{
	const int32_t n = fctiwz(t);
	const float f   = t - (float)n;
	float pow2, p;
	if (n > 128)
		return float_huge.f;
	if (n < -127)
		return 0.0f;
	pow2 = u2f((uint32_t)(n + 127) << 23);
	p    = HOT_FMADDS(f, two_to_x[8].f, two_to_x[7].f);
	p    = HOT_FMADDS(f, p, two_to_x[6].f);
	p    = HOT_FMADDS(f, p, two_to_x[5].f);
	p    = HOT_FMADDS(f, p, two_to_x[4].f);
	p    = HOT_FMADDS(f, p, two_to_x[3].f);
	p    = HOT_FMADDS(f, p, two_to_x[2].f);
	p    = HOT_FMADDS(f, p, two_to_x[1].f);
	p    = HOT_FMADDS(f, p, two_to_x[0].f);
	p    = f * p;
	p    = exp2_c025.f + p;
	p    = exp2_c075.f + p;
	return pow2 * p;
}

MSL_HOT_ATTR float MSL_HOT(sms_msl_powf)(float x, float y)
{
	uint32_t by;
	if (x > 0.0f)
		return MSL_HOT(msl_exp2f)(y * MSL_HOT(msl_log2f)(x));
	if (x < 0.0f) {
		const int32_t iy = fctiwz(y);
		if (y - (float)iy != 0.0f) /* also NaN, inf and y >= 2^31 but for 2^31 */
			return msl_nan.f;
		if (iy % 2 != 0)
			return -MSL_HOT(msl_exp2f)(y * MSL_HOT(msl_log2f)(-x));
		return MSL_HOT(msl_exp2f)(y * MSL_HOT(msl_log2f)(-x));
	}
	if (x != x) /* x is +-0 or NaN from here */
		return x;
	by = f2u(y) & 0x7fffffff;
	if (by == 0)
		return 1.0f;
	if (by >= 0x7f800000) /* NaN, inf */
		return msl_nan.f;
	if (y < 0.0f) /* x == -0.0f compares true for +0 too */
		return x == -0.0f ? -float_huge.f : float_huge.f;
	return 0.0f;
}

