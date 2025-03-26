const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const translate_c = b.addTranslateC(.{
        .root_source_file = b.path("linker/dlfcn.h"),
        .target = target,
        .optimize = optimize,
    });

    const exe_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
    });

    const pthread = b.addLibrary(.{
        .linkage = .dynamic,
        .name = "pthread",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    pthread.addCSourceFiles(.{
        .files = &pthread_src,
        .flags = &.{"-fno-exceptions"},
    });
    pthread.root_module.addCMacro("_GNU_SOURCE", "1");
    pthread.linkSystemLibrary("dl");
    pthread.linkSystemLibrary("pthread");
    b.installArtifact(pthread);

    const libc = b.addLibrary(.{
        .linkage = .dynamic,
        .name = "c",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/root.zig"),
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    libc.root_module.addCMacro("_GNU_SOURCE", "1");
    libc.root_module.addCMacro("PAGESIZE", "4096");
    libc.addCSourceFiles(.{
        .files = &wrapper_src,
        .flags = &.{"-fvisibility=hidden"},
    });
    libc.addCSourceFiles(.{
        .files = &libc_src,
        .flags = &.{
            "-fno-exceptions",
            "-Wl,-wrap,_IO_file_xsputn",
            "-Wl,--no-as-needed",
        },
    });
    libc.linkLibrary(pthread);
    libc.linkSystemLibrary("bsd");
    libc.linkSystemLibrary("unwind");
    b.installArtifact(libc);

    const linker = b.addLibrary(.{
        .linkage = .dynamic,
        .name = "dl",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    linker.addCSourceFiles(.{
        .files = &wrapper_src,
        .flags = &.{"-fvisibility=hidden"},
    });
    linker.addCSourceFiles(.{
        .files = &linker_src,
        .flags = &.{"-Wl,--no-as-needed"},
    });
    linker.root_module.addCMacro("_GNU_SOURCE", "1");
    linker.root_module.addCMacro("LINKER_DEBUG", "1");
    linker.root_module.addCMacro("VERBOSE_FUNCTIONS", "1");
    linker.linkSystemLibrary("dl");
    linker.linkSystemLibrary("egl");
    linker.linkSystemLibrary("pthread");
    b.installArtifact(linker);

    const @"libstdc++" = b.addLibrary(.{
        .linkage = .dynamic,
        .name = "stdc++",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            .link_libcpp = true,
        }),
    });
    @"libstdc++".addCSourceFiles(.{
        .files = &libcpp_src,
        .flags = &.{"--std=c++23"},
        .language = .cpp,
    });
    @"libstdc++".addIncludePath(b.path("libstdc++_standalone/include"));
    b.installArtifact(@"libstdc++");

    const exe = b.addExecutable(.{
        .name = "bionic",
        .root_module = exe_mod,
    });
    exe.root_module.addImport("c", translate_c.createModule());
    exe.addCSourceFile(.{
        .file = b.path("main_executable/bionic_compat.c"),
        .flags = &.{},
    });
    exe.linkLibrary(libc);
    exe.linkLibrary(linker);
    exe.linkLibrary(@"libstdc++");
    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the app");
    run_step.dependOn(&run_cmd.step);

    const exe_unit_tests = b.addTest(.{
        .root_module = exe_mod,
    });

    const run_exe_unit_tests = b.addRunArtifact(exe_unit_tests);
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_exe_unit_tests.step);
}

const linker_src = [_][]const u8{
    "linker/config.c",
    "linker/dlfcn.c",
    "linker/linker.c",
    "linker/linker_environ.c",
    "linker/rt.c",
    "linker/strlcpy.c",
};

const wrapper_src = [_][]const u8{
    "wrapper/wrapper.c",
};

const pthread_src = [_][]const u8{
    "pthread_wrapper/libpthread.c",
};

const libc_src = [_][]const u8{
    "libc/libc.c",
    "libc/libc-chk.c",
    //"libc/libc-math.c",
    "libc/libc-misc.c",
    "libc/libc-musl.c",
    "libc/libc-stdio.c",
    "libc/libc-sha1.c",
    "libc/libc-antiantidebug.c",
};

const libcpp_src = [_][]const u8{
    "libstdc++_standalone/new.cpp",
    "libstdc++_standalone/__cxa_pure_virtual.cpp",
    "libstdc++_standalone/__cxa_guard.cpp",
};
