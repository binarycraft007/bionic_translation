const std = @import("std");
const math = std.math;

const FP_INFINITE = 0x01;
const FP_NAN = 0x02;
const FP_NORMAL = 0x04;
const FP_SUBNORMAL = 0x08;
const FP_ZERO = 0x10;

inline fn fpclassify(x: anytype) c_int {
    if (math.isInf(x)) {
        return FP_INFINITE;
    } else if (math.isNan(x)) {
        return FP_NAN;
    } else if (math.isNormal(x)) {
        return FP_NORMAL;
    } else if (math.isNegativeZero(x) or math.isPositiveZero(x)) {
        return FP_ZERO;
    } else {
        return FP_SUBNORMAL;
    }
}

export fn bionic_isnan(val: f64) c_int {
    return @intFromBool(math.isNan(val));
}

export fn bionic___fpclassifyf(f: f32) c_int {
    return fpclassify(f);
}

export fn bionic___fpclassifyd(d: f64) c_int {
    return fpclassify(d);
}

export fn bionic___fpclassifyl(e: f80) c_int {
    return fpclassify(e);
}
