#ifndef __GTA_ANGLE_WRAP_H__
#define __GTA_ANGLE_WRAP_H__

/*
 * Radian-angle constants and wrapping helpers, split out of common.h so the
 * pure wrapping logic can be unit-tested on the host without pulling in the
 * renderware/window headers (see tests/amigaos/anglewrap_test.c).
 *
 * This header only depends on <math.h>. Include it (directly or via common.h)
 * wherever PI/TWOPI/HALFPI/DEGTORAD/RADTODEG or WrapRadianAngle/WrapTwoPi are
 * needed.
 */

#include <math.h>

#ifdef __MWERKS__
#define M_E        2.71828182845904523536   // e
#define M_LOG2E    1.44269504088896340736   // log2(e)
#define M_LOG10E   0.434294481903251827651  // log10(e)
#define M_LN2      0.693147180559945309417  // ln(2)
#define M_LN10     2.30258509299404568402   // ln(10)
#define M_PI       3.14159265358979323846   // pi
#define M_PI_2     1.57079632679489661923   // pi/2
#define M_PI_4     0.785398163397448309616  // pi/4
#define M_1_PI     0.318309886183790671538  // 1/pi
#define M_2_PI     0.636619772367581343076  // 2/pi
#define M_2_SQRTPI 1.12837916709551257390   // 2/sqrt(pi)
#define M_SQRT2    1.41421356237309504880   // sqrt(2)
#define M_SQRT1_2  0.707106781186547524401  // 1/sqrt(2)
#endif

#ifndef M_PI
#define M_PI       3.14159265358979323846   // pi
#endif
#ifndef M_PI_2
#define M_PI_2     1.57079632679489661923   // pi/2
#endif
#ifndef M_PI_4
#define M_PI_4     0.785398163397448309616  // pi/4
#endif

#ifndef PI
#define PI (float)M_PI
#endif
#ifndef TWOPI
#define TWOPI (PI*2)
#endif
#ifndef HALFPI
#define HALFPI (PI/2)
#endif
#ifndef DEGTORAD
#define DEGTORAD(x) ((x) * PI / 180.0f)
#endif
#ifndef RADTODEG
#define RADTODEG(x) ((x) * 180.0f / PI)
#endif

// Bring a radian angle into [-PI, PI] robustly. A naive
// "while(angle > PI) angle -= TWOPI" loop never terminates when the FPU value is
// huge (subtracting TWOPI no longer changes it, e.g. fp0 = 1e30), which hangs
// the game. Reduce with fmod first, then one +/- TWOPI.
static inline float WrapRadianAngle(float angle)
{
	if (angle < -PI || angle > PI) {
		angle = fmodf(angle, TWOPI);
		if (angle < -PI)
			angle += TWOPI;
		else if (angle > PI)
			angle -= TWOPI;
	}
	return angle;
}

// Same, but into [0, TWOPI).
static inline float WrapTwoPi(float angle)
{
	angle = WrapRadianAngle(angle);
	return angle < 0.0f ? angle + TWOPI : angle;
}

#endif /* __GTA_ANGLE_WRAP_H__ */
