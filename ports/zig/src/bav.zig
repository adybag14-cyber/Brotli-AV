//! BAV1 research codec — pure Zig port (adybag14-cyber/zig only, no system zlib).
const std = @import("std");
const flate = std.compress.flate;
const Writer = std.Io.Writer;
const Reader = std.Io.Reader;

pub const MAGIC = "BAV1";
pub const FORMAT_VERSION: u8 = 1;
pub const HEADER_SIZE: usize = 19;

pub const M_STORE: u8 = 0;
pub const M_DEFLATE: u8 = 1;
pub const M_TRANSPOSE: u8 = 4;
pub const M_PREFILTER: u8 = 6;
pub const M_BWT: u8 = 7;

pub const F_MTF: u8 = 1;
pub const F_RLE0: u8 = 2;
pub const F_MTF_RLE0: u8 = 3;
pub const BWT_F_MTF: u8 = 1;
pub const BWT_F_RLE0: u8 = 2;
const BWT_MAX: usize = 120_000;

pub const Method = enum { auto, store, deflate, research };

pub fn crc32(data: []const u8) u32 {
    return std.hash.crc.@"CRC-32/ISO-HDLC".hash(data);
}

fn packHeader(method: u8, orig: u64, crc: u32, out: *[HEADER_SIZE]u8) void {
    @memcpy(out[0..4], MAGIC);
    out[4] = FORMAT_VERSION;
    out[5] = method;
    out[6] = 0;
    std.mem.writeInt(u64, out[7..15], orig, .little);
    std.mem.writeInt(u32, out[15..19], crc, .little);
}

fn zlibEnc(allocator: std.mem.Allocator, data: []const u8) ![]u8 {
    // Output capacity: worst-case store expansion + zlib header/footer headroom
    const cap = data.len + data.len / 8 + 64 + 1024;
    const out_buf = try allocator.alloc(u8, cap);
    defer allocator.free(out_buf);
    var out_w: Writer = .fixed(out_buf);
    var window: [flate.max_window_len * 2]u8 = undefined;
    var comp = try flate.Compress.init(&out_w, &window, .zlib, .level_9);
    try comp.writer.writeAll(data);
    try comp.finish();
    const written = out_w.buffered();
    return try allocator.dupe(u8, written);
}

fn zlibDec(allocator: std.mem.Allocator, data: []const u8) ![]u8 {
    var in_reader: Reader = .fixed(data);
    var window: [flate.max_window_len]u8 = undefined;
    var decomp = flate.Decompress.init(&in_reader, .zlib, &window);
    var list: std.ArrayList(u8) = .empty;
    errdefer list.deinit(allocator);
    var tmp: [8192]u8 = undefined;
    while (true) {
        const n = try decomp.reader.readSliceShort(&tmp);
        if (n == 0) break;
        try list.appendSlice(allocator, tmp[0..n]);
    }
    return try list.toOwnedSlice(allocator);
}

pub fn mtfEncode(allocator: std.mem.Allocator, data: []const u8) ![]u8 {
    var table: [256]u8 = undefined;
    var pos: [256]u8 = undefined;
    for (0..256) |i| {
        table[i] = @intCast(i);
        pos[i] = @intCast(i);
    }
    const out = try allocator.alloc(u8, data.len);
    for (data, 0..) |b, i| {
        const r = pos[b];
        out[i] = r;
        if (r != 0) {
            var j: u8 = r;
            while (j > 0) : (j -= 1) {
                const s = table[j - 1];
                table[j] = s;
                pos[s] = j;
            }
            table[0] = b;
            pos[b] = 0;
        }
    }
    return out;
}

pub fn mtfDecode(allocator: std.mem.Allocator, data: []const u8) ![]u8 {
    var table: [256]u8 = undefined;
    for (0..256) |i| table[i] = @intCast(i);
    const out = try allocator.alloc(u8, data.len);
    for (data, 0..) |r, i| {
        const b = table[r];
        out[i] = b;
        if (r != 0) {
            var j: u8 = r;
            while (j > 0) : (j -= 1) table[j] = table[j - 1];
            table[0] = b;
        }
    }
    return out;
}

pub fn rle0Encode(allocator: std.mem.Allocator, data: []const u8) ![]u8 {
    var list: std.ArrayList(u8) = .empty;
    errdefer list.deinit(allocator);
    var i: usize = 0;
    while (i < data.len) {
        if (data[i] == 0) {
            var j = i;
            while (j < data.len and data[j] == 0 and (j - i) < 255) : (j += 1) {}
            try list.append(allocator, 0);
            try list.append(allocator, @intCast(j - i));
            i = j;
        } else {
            try list.append(allocator, data[i]);
            i += 1;
        }
    }
    return try list.toOwnedSlice(allocator);
}

pub fn rle0Decode(allocator: std.mem.Allocator, data: []const u8) ![]u8 {
    var list: std.ArrayList(u8) = .empty;
    errdefer list.deinit(allocator);
    var i: usize = 0;
    while (i < data.len) {
        if (data[i] == 0) {
            if (i + 1 >= data.len) return error.TruncatedRle0;
            const count = data[i + 1];
            try list.appendNTimes(allocator, 0, count);
            i += 2;
        } else {
            try list.append(allocator, data[i]);
            i += 1;
        }
    }
    return try list.toOwnedSlice(allocator);
}

pub fn transpose(allocator: std.mem.Allocator, data: []const u8, width: usize) ![]u8 {
    if (width <= 1 or data.len < width * 2) return try allocator.dupe(u8, data);
    const n = data.len - (data.len % width);
    if (n == 0) return try allocator.dupe(u8, data);
    const rows = n / width;
    const out = try allocator.alloc(u8, data.len);
    for (0..width) |col| {
        const base = col * rows;
        for (0..rows) |row| out[base + row] = data[row * width + col];
    }
    if (data.len > n) @memcpy(out[n..], data[n..]);
    return out;
}

pub fn untranspose(allocator: std.mem.Allocator, data: []const u8, width: usize) ![]u8 {
    if (width <= 1 or data.len < width * 2) return try allocator.dupe(u8, data);
    const n = data.len - (data.len % width);
    const rows = n / width;
    const out = try allocator.alloc(u8, data.len);
    for (0..width) |col| {
        const base = col * rows;
        for (0..rows) |row| out[row * width + col] = data[base + row];
    }
    if (data.len > n) @memcpy(out[n..], data[n..]);
    return out;
}

pub fn bwtEncode(allocator: std.mem.Allocator, data: []const u8) !struct { last: []u8, primary: u32 } {
    const n = data.len;
    if (n == 0) return .{ .last = try allocator.alloc(u8, 0), .primary = 0 };
    const sa = try allocator.alloc(usize, n);
    defer allocator.free(sa);
    for (0..n) |i| sa[i] = i;
    std.mem.sort(usize, sa, data, struct {
        fn lessThan(d: []const u8, a: usize, b: usize) bool {
            var i: usize = 0;
            while (i < d.len) : (i += 1) {
                const ca = d[(a + i) % d.len];
                const cb = d[(b + i) % d.len];
                if (ca < cb) return true;
                if (ca > cb) return false;
            }
            return false;
        }
    }.lessThan);
    const last = try allocator.alloc(u8, n);
    var primary: u32 = 0;
    for (sa, 0..) |idx, i| {
        last[i] = data[(idx + n - 1) % n];
        if (idx == 0) primary = @intCast(i);
    }
    return .{ .last = last, .primary = primary };
}

pub fn bwtDecode(allocator: std.mem.Allocator, last: []const u8, primary: u32) ![]u8 {
    const n = last.len;
    if (n == 0) return try allocator.alloc(u8, 0);
    if (primary >= n) return error.BwtPrimaryOor;
    const order = try allocator.alloc(usize, n);
    defer allocator.free(order);
    for (0..n) |i| order[i] = i;
    std.mem.sort(usize, order, last, struct {
        fn less(ctx: []const u8, a: usize, b: usize) bool {
            if (ctx[a] < ctx[b]) return true;
            if (ctx[a] > ctx[b]) return false;
            return a < b;
        }
    }.less);
    const out = try allocator.alloc(u8, n);
    var p: usize = primary;
    for (0..n) |i| {
        p = order[p];
        out[i] = last[p];
    }
    return out;
}

fn bestBackend(allocator: std.mem.Allocator, data: []const u8) !struct { mid: u8, payload: []u8 } {
    const store = try allocator.dupe(u8, data);
    const defl = zlibEnc(allocator, data) catch {
        return .{ .mid = M_STORE, .payload = store };
    };
    if (defl.len < store.len) {
        allocator.free(store);
        return .{ .mid = M_DEFLATE, .payload = defl };
    }
    allocator.free(defl);
    return .{ .mid = M_STORE, .payload = store };
}

fn backendDec(allocator: std.mem.Allocator, mid: u8, payload: []const u8) ![]u8 {
    return switch (mid) {
        M_STORE => try allocator.dupe(u8, payload),
        M_DEFLATE => try zlibDec(allocator, payload),
        else => error.UnknownBackend,
    };
}

const Cand = struct { mid: u8, payload: []u8 };

pub fn compress(allocator: std.mem.Allocator, data: []const u8, method: Method) ![]u8 {
    const crc = crc32(data);
    const orig: u64 = data.len;
    var cands: std.ArrayList(Cand) = .empty;
    defer {
        for (cands.items) |cnd| allocator.free(cnd.payload);
        cands.deinit(allocator);
    }

    switch (method) {
        .store => try cands.append(allocator, .{ .mid = M_STORE, .payload = try allocator.dupe(u8, data) }),
        .deflate => try cands.append(allocator, .{ .mid = M_DEFLATE, .payload = try zlibEnc(allocator, data) }),
        .auto, .research => {
            try cands.append(allocator, .{ .mid = M_STORE, .payload = try allocator.dupe(u8, data) });
            if (zlibEnc(allocator, data)) |p| {
                try cands.append(allocator, .{ .mid = M_DEFLATE, .payload = p });
            } else |_| {}

            if (data.len >= 32) {
                for ([_]u8{ F_MTF, F_RLE0, F_MTF_RLE0 }) |fid| {
                    const filtered = switch (fid) {
                        F_MTF => try mtfEncode(allocator, data),
                        F_RLE0 => try rle0Encode(allocator, data),
                        else => blk: {
                            const m = try mtfEncode(allocator, data);
                            defer allocator.free(m);
                            break :blk try rle0Encode(allocator, m);
                        },
                    };
                    defer allocator.free(filtered);
                    if (filtered.len > data.len * 2 + 64) continue;
                    const bb = try bestBackend(allocator, filtered);
                    var wrapped = try allocator.alloc(u8, 2 + bb.payload.len);
                    wrapped[0] = fid;
                    wrapped[1] = bb.mid;
                    @memcpy(wrapped[2..], bb.payload);
                    allocator.free(bb.payload);
                    try cands.append(allocator, .{ .mid = M_PREFILTER, .payload = wrapped });
                }
            }
            for ([_]usize{ 2, 4, 8, 16 }) |w| {
                if (data.len < w * 4) continue;
                const t = try transpose(allocator, data, w);
                defer allocator.free(t);
                const bb = try bestBackend(allocator, t);
                var wrapped = try allocator.alloc(u8, 2 + bb.payload.len);
                wrapped[0] = @intCast(w);
                wrapped[1] = bb.mid;
                @memcpy(wrapped[2..], bb.payload);
                allocator.free(bb.payload);
                try cands.append(allocator, .{ .mid = M_TRANSPOSE, .payload = wrapped });
            }
            if (data.len >= 64 and data.len <= BWT_MAX) {
                const bw = try bwtEncode(allocator, data);
                defer allocator.free(bw.last);
                const m = try mtfEncode(allocator, bw.last);
                defer allocator.free(m);
                const bb = try bestBackend(allocator, m);
                var wrapped = try allocator.alloc(u8, 6 + bb.payload.len);
                wrapped[0] = BWT_F_MTF;
                std.mem.writeInt(u32, wrapped[1..5], bw.primary, .little);
                wrapped[5] = bb.mid;
                @memcpy(wrapped[6..], bb.payload);
                allocator.free(bb.payload);
                try cands.append(allocator, .{ .mid = M_BWT, .payload = wrapped });
            }
        },
    }

    if (cands.items.len == 0) {
        try cands.append(allocator, .{ .mid = M_STORE, .payload = try allocator.dupe(u8, data) });
    }
    std.mem.sort(Cand, cands.items, {}, struct {
        fn less(_: void, a: Cand, b: Cand) bool {
            if (a.payload.len != b.payload.len) return a.payload.len < b.payload.len;
            return a.mid < b.mid;
        }
    }.less);

    const best = cands.items[0];
    var frame = try allocator.alloc(u8, HEADER_SIZE + best.payload.len);
    var hdr: [HEADER_SIZE]u8 = undefined;
    packHeader(best.mid, orig, crc, &hdr);
    @memcpy(frame[0..HEADER_SIZE], &hdr);
    @memcpy(frame[HEADER_SIZE..], best.payload);
    return frame;
}

pub fn decompress(allocator: std.mem.Allocator, frame: []const u8) ![]u8 {
    if (frame.len < HEADER_SIZE) return error.FrameTooShort;
    if (!std.mem.eql(u8, frame[0..4], MAGIC)) return error.BadMagic;
    if (frame[4] != FORMAT_VERSION) return error.BadVersion;
    const mid = frame[5];
    const orig = std.mem.readInt(u64, frame[7..15], .little);
    const crc = std.mem.readInt(u32, frame[15..19], .little);
    const payload = frame[HEADER_SIZE..];

    const data: []u8 = switch (mid) {
        M_TRANSPOSE => blk: {
            if (payload.len < 2) return error.PayloadShort;
            const width = payload[0];
            const transformed = try backendDec(allocator, payload[1], payload[2..]);
            defer allocator.free(transformed);
            break :blk try untranspose(allocator, transformed, width);
        },
        M_PREFILTER => blk: {
            if (payload.len < 2) return error.PayloadShort;
            const fid = payload[0];
            const filtered = try backendDec(allocator, payload[1], payload[2..]);
            defer allocator.free(filtered);
            break :blk switch (fid) {
                F_MTF => try mtfDecode(allocator, filtered),
                F_RLE0 => try rle0Decode(allocator, filtered),
                F_MTF_RLE0 => blk2: {
                    const r = try rle0Decode(allocator, filtered);
                    defer allocator.free(r);
                    break :blk2 try mtfDecode(allocator, r);
                },
                else => return error.UnknownFilter,
            };
        },
        M_BWT => blk: {
            if (payload.len < 6) return error.PayloadShort;
            const flags = payload[0];
            const primary = std.mem.readInt(u32, payload[1..5], .little);
            var transformed = try backendDec(allocator, payload[5], payload[6..]);
            if (flags & BWT_F_RLE0 != 0) {
                const r = try rle0Decode(allocator, transformed);
                allocator.free(transformed);
                transformed = r;
            }
            if (flags & BWT_F_MTF != 0) {
                const m = try mtfDecode(allocator, transformed);
                allocator.free(transformed);
                transformed = m;
            }
            defer allocator.free(transformed);
            break :blk try bwtDecode(allocator, transformed, primary);
        },
        M_STORE, M_DEFLATE => try backendDec(allocator, mid, payload),
        else => return error.UnknownMethod,
    };

    if (data.len != orig) {
        allocator.free(data);
        return error.SizeMismatch;
    }
    if (crc32(data) != crc) {
        allocator.free(data);
        return error.CrcMismatch;
    }
    return data;
}

test "roundtrip store deflate auto" {
    const allocator = std.testing.allocator;
    const samples = [_][]const u8{ "", "a", "hello world", "aaaaaaaaaaaaaaaaaaaaaaaa" };
    for (samples) |s| {
        for ([_]Method{ .store, .deflate, .auto }) |m| {
            const frame = try compress(allocator, s, m);
            defer allocator.free(frame);
            try std.testing.expectEqualSlices(u8, MAGIC, frame[0..4]);
            const back = try decompress(allocator, frame);
            defer allocator.free(back);
            try std.testing.expectEqualSlices(u8, s, back);
        }
    }
}

test "mtf rle0 transpose bwt" {
    const allocator = std.testing.allocator;
    var data: [200]u8 = undefined;
    for (&data, 0..) |*b, i| b.* = @intCast(i % 256);
    const m = try mtfEncode(allocator, &data);
    defer allocator.free(m);
    const md = try mtfDecode(allocator, m);
    defer allocator.free(md);
    try std.testing.expectEqualSlices(u8, &data, md);
    const r = try rle0Encode(allocator, &data);
    defer allocator.free(r);
    const rd = try rle0Decode(allocator, r);
    defer allocator.free(rd);
    try std.testing.expectEqualSlices(u8, &data, rd);
    const t = try transpose(allocator, &data, 4);
    defer allocator.free(t);
    const ut = try untranspose(allocator, t, 4);
    defer allocator.free(ut);
    try std.testing.expectEqualSlices(u8, &data, ut);
    const bw = try bwtEncode(allocator, &data);
    defer allocator.free(bw.last);
    const bd = try bwtDecode(allocator, bw.last, bw.primary);
    defer allocator.free(bd);
    try std.testing.expectEqualSlices(u8, &data, bd);
}
