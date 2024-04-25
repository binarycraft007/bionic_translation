#include <math.h>

#define BIT(x) (1 << x)

#define	BIONIC_FP_INFINITE  BIT(0)
#define	BIONIC_FP_NAN       BIT(1)
#define	BIONIC_FP_NORMAL    BIT(2)
#define	BIONIC_FP_SUBNORMAL BIT(3)
#define	BIONIC_FP_ZERO      BIT(4)

#define bionic_fpclassify(x) __builtin_fpclassify(BIONIC_FP_NAN, \
                                                  BIONIC_FP_INFINITE, \
                                                  BIONIC_FP_NORMAL, \
                                                  BIONIC_FP_SUBNORMAL, \
                                                  BIONIC_FP_ZERO, \
                                                  x)

int bionic_isnan(double val) {
	return isnan(val);
}

int bionic___fpclassifyf(float f)
{
	return bionic_fpclassify(f);
}
int bionic___fpclassifyd(double d)
{
	return bionic_fpclassify(d);
}
int bionic___fpclassifyl(long double e)
{
	return bionic_fpclassify(e);
}
