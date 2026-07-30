const std = @import("std");
const bav = @import("bav");

pub fn main(init: std.process.Init) !void {
    const allocator = init.gpa;
    const io = init.io;
    var args_it = try std.process.Args.Iterator.initAllocator(init.minimal.args, allocator);
    defer args_it.deinit();
    _ = args_it.next();
    const cmd = args_it.next() orelse {
        std.debug.print("usage: bav-zig compress|decompress|version ...\n", .{});
        std.process.exit(2);
    };

    if (std.mem.eql(u8, cmd, "version")) {
        try std.Io.File.stdout().writeStreamingAll(io, "bav-zig 0.1.0 (adybag14-cyber/zig)\n");
        return;
    }

    if (std.mem.eql(u8, cmd, "compress")) {
        var in_path: ?[]const u8 = null;
        var out_path: ?[]const u8 = null;
        var method: bav.Method = .auto;
        while (args_it.next()) |a| {
            if (std.mem.eql(u8, a, "-o") or std.mem.eql(u8, a, "--output")) {
                out_path = args_it.next();
            } else if (std.mem.eql(u8, a, "-m") or std.mem.eql(u8, a, "--method")) {
                const m = args_it.next() orelse "auto";
                method = if (std.mem.eql(u8, m, "store"))
                    .store
                else if (std.mem.eql(u8, m, "deflate"))
                    .deflate
                else if (std.mem.eql(u8, m, "research"))
                    .research
                else
                    .auto;
            } else if (in_path == null) {
                in_path = a;
            }
        }
        const in = in_path orelse {
            std.debug.print("missing input\n", .{});
            std.process.exit(2);
        };
        const in_owned = try allocator.dupe(u8, in);
        defer allocator.free(in_owned);
        const out_owned = if (out_path) |o|
            try allocator.dupe(u8, o)
        else
            try std.fmt.allocPrint(allocator, "{s}.bav", .{in_owned});
        defer allocator.free(out_owned);

        const data = try std.Io.Dir.cwd().readFileAlloc(io, in_owned, allocator, .unlimited);
        defer allocator.free(data);
        const frame = try bav.compress(allocator, data, method);
        defer allocator.free(frame);
        {
            const file = try std.Io.Dir.cwd().createFile(io, out_owned, .{});
            defer file.close(io);
            try file.writeStreamingAll(io, frame);
        }
        var buf: [512]u8 = undefined;
        const msg = try std.fmt.bufPrint(&buf, "compressed {d} -> {d} bytes ({s})\n", .{ data.len, frame.len, out_owned });
        try std.Io.File.stdout().writeStreamingAll(io, msg);
        return;
    }

    if (std.mem.eql(u8, cmd, "decompress")) {
        var in_path: ?[]const u8 = null;
        var out_path: ?[]const u8 = null;
        while (args_it.next()) |a| {
            if (std.mem.eql(u8, a, "-o") or std.mem.eql(u8, a, "--output")) {
                out_path = args_it.next();
            } else if (in_path == null) {
                in_path = a;
            }
        }
        const in = in_path orelse {
            std.debug.print("missing input\n", .{});
            std.process.exit(2);
        };
        const in_owned = try allocator.dupe(u8, in);
        defer allocator.free(in_owned);
        const out_owned = if (out_path) |o|
            try allocator.dupe(u8, o)
        else if (std.mem.endsWith(u8, in_owned, ".bav"))
            try allocator.dupe(u8, in_owned[0 .. in_owned.len - 4])
        else
            try std.fmt.allocPrint(allocator, "{s}.out", .{in_owned});
        defer allocator.free(out_owned);

        const frame = try std.Io.Dir.cwd().readFileAlloc(io, in_owned, allocator, .unlimited);
        defer allocator.free(frame);
        const data = try bav.decompress(allocator, frame);
        defer allocator.free(data);
        {
            const file = try std.Io.Dir.cwd().createFile(io, out_owned, .{});
            defer file.close(io);
            try file.writeStreamingAll(io, data);
        }
        var buf: [512]u8 = undefined;
        const msg = try std.fmt.bufPrint(&buf, "decompressed {d} -> {d} bytes ({s})\n", .{ frame.len, data.len, out_owned });
        try std.Io.File.stdout().writeStreamingAll(io, msg);
        return;
    }

    std.debug.print("unknown command\n", .{});
    std.process.exit(2);
}
