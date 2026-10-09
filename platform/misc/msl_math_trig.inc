/* Included by msl_math.c, once or twice (see MSL_HOT there). */
/* The argument reduction shared by sinf and cosf: x less the nearest multiple
 * of pi/2, in units of pi/4 (frac_part), and that multiple's quadrant. */
MSL_HOT_ATTR static float MSL_HOT(trig_reduce)(float x, int* quadrant)
{
	float z = two_over_pi.f * x;                                    /* fmuls */
	int32_t n = (f2u(x) & 0x80000000) ? fctiwz(z - half.f) : fctiwz(half.f + z);
	float n2  = (float)(int32_t)((uint32_t)n << 1); /* slwi, the 0x43300000 conversion, fsubs */
	float f   = x - n2;                                             /* fsubs */
	f         = HOT_FMADDS(four_over_pi_m1[0].f, x, f);
	f         = HOT_FMADDS(four_over_pi_m1[1].f, x, f);
	f         = HOT_FMADDS(four_over_pi_m1[2].f, x, f);
	f         = HOT_FMADDS(four_over_pi_m1[3].f, x, f);
	*quadrant = (int)(n & 3);
	return f;
}

MSL_HOT_ATTR float MSL_HOT(sms_msl_cosf)(float x)
{
	int n;
	float f = MSL_HOT(trig_reduce)(x, &n), xsq, t;
	n <<= 1;
	if (fabsf(f) < sqrt_eps.f)
		return HOT_FNMSUBS(f, Q(n), Q(n + 1));
	xsq = f * f;
	if (n & 2) {
		t = HOT_FMADDS(P(1), xsq, P(3));
		t = HOT_FMADDS(xsq, t, P(5));
		t = HOT_FMADDS(xsq, t, P(7));
		t = HOT_FNMADDS(xsq, t, P(9));
		t = f * t;
		return t * Q(n);
	}
	t = HOT_FMADDS(P(0), xsq, P(2));
	t = HOT_FMADDS(xsq, t, P(4));
	t = HOT_FMADDS(xsq, t, P(6));
	t = HOT_FMADDS(xsq, t, P(8));
	return t * Q(n + 1);
}

MSL_HOT_ATTR float MSL_HOT(sms_msl_sinf)(float x)
{
	int n;
	float f = MSL_HOT(trig_reduce)(x, &n), xsq, t;
	n <<= 1;
	if (fabsf(f) < sqrt_eps.f)
		return HOT_FMADDS(P(9), f * Q(n + 1), Q(n));
	xsq = f * f;
	if (n & 2) {
		t = HOT_FMADDS(P(0), xsq, P(2));
		t = HOT_FMADDS(xsq, t, P(4));
		t = HOT_FMADDS(xsq, t, P(6));
		t = HOT_FMADDS(xsq, t, P(8));
		return t * Q(n);
	}
	t = HOT_FMADDS(P(1), xsq, P(3));
	t = HOT_FMADDS(xsq, t, P(5));
	t = HOT_FMADDS(xsq, t, P(7));
	t = HOT_FMADDS(xsq, t, P(9));
	t = f * t;
	return t * Q(n + 1);
}

