//! By convention, main.zig is where your main function lives in the case that
//! you are building an executable. If you are making a library, the convention
//! is to delete this file and start with root.zig instead.

pub fn main() !void {
    _ = c.bionic_dlopen("ndk_test", 0);
}

const std = @import("std");
const c = @import("c");
