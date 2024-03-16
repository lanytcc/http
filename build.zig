const std = @import("std");
const quickjs = @import("../quickjs/build.zig");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});

    const q = quickjs.build_quickjs(b, target);
    // const http = b.addSharedLibrary(.{ .name = "http" });
    const http = b.addSharedLibrary(.{
        .name = "http",
        .target = target,
        .optimize = .ReleaseSafe,
    });
    // http.setMode(mode);

    http.linkLibC();
    http.linkLibrary(q);
    http.addIncludePath(.{ .path = "../quickjs" });
    // http.addLibraryPath(.{ .path = "../quickjs/zig-out/lib" });
    // http.linkSystemLibrary("quickjs");
    http.linkSystemLibrary("c");
    http.addCSourceFiles(.{
        .files = &.{ "http.c", "util.c" },
        .flags = &.{
            "-Wall",
            "-Wno-array-bounds",
            "-fwrapv",
            "-fdeclspec",
            "-fvisibility=hidden",
            "-DCONFIG_VERSION=\"2024-02-14\"",
            // "-DCONFIG_CHECK_JSVALUE",
        },
    });

    b.installArtifact(http);
}
