const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});

    // const http = b.addSharedLibrary(.{ .name = "http" });
    const http = b.addSharedLibrary(.{
        .name = "http",
        .target = target,
        .optimize = .ReleaseSafe,
    });
    // http.setMode(mode);
    http.linkLibC();
    http.addIncludePath(.{ .path = "../quickjs" });
    http.addLibraryPath(.{ .path = "../quickjs" });
    http.linkSystemLibrary("quickjs");
    http.linkSystemLibrary("c");
    http.addCSourceFiles(.{
        .files = &.{ "http.c", "util.c" },
        .flags = &.{
            "-Wall",
            "-Wno-array-bounds",
            "-fwrapv",
            "-DCONFIG_VERSION=\"2024-02-14\"",
            // "-DCONFIG_CHECK_JSVALUE",
        },
    });

    b.installArtifact(http);
}
