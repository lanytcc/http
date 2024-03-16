const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});

    const http = b.addSharedLibrary(.{
        .name = "http",
        .target = target,
        .optimize = .ReleaseSafe,
    });

    http.linkLibC();
    http.addIncludePath(.{ .path = "../quickjs" });
    http.addLibraryPath(.{ .path = "../quickjs/zig-out/lib" });
    http.linkSystemLibrary("quickjs");
    http.linkSystemLibrary("curl");
    http.linkSystemLibrary("event");
    http.linkSystemLibrary("c");
    http.addCSourceFiles(.{
        .files = &.{ "http.c", "util.c" },
        .flags = &.{
            "-fPIC",
            "-shared",
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
