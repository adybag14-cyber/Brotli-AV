using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;
using XZ.NET;
using ZstdSharp;

namespace Bav;

public enum Method { Auto, Store, Deflate, Lzma, Zstd, Brotli, Research }

/// <summary>
/// Full research BAV codec — default production port (gen2+ parity with src/bav/codec.py).
/// Auto mode: store/deflate/lzma(+delta via prefilters)/zstd-22/brotli,
/// transpose, xform, MTF/RLE0/SUB*/XOR prefilters, BWT(+MTF/RLE0/SUB),
/// multi-block, parts, token. Candidate families run in parallel.
/// </summary>
public static class Codec
{
    public const string MagicStr = "BAV1";
    public const byte FormatVersion = 2;
    public const byte FormatVersionLegacy = 1;
    public const int HeaderV2Size = 15;
    public const int HeaderV1Size = 19;

    public const byte MStore = 0, MDeflate = 1, MLzma = 2, MZstd = 3, MTranspose = 4,
        MBrotli = 5, MPrefilter = 6, MBwt = 7, MBlocks = 8, MToken = 9, MXform = 10, MParts = 11;

    // Prefilter IDs (must match Python src/bav/codec.py)
    public const byte FMtf = 1, FRle0 = 2, FMtfRle0 = 3, FSub1 = 4, FSub4 = 5, FXor1 = 6;
    // Extended SUB distances (Python LZMA delta-chain distances, as prefilter+backend)
    public const byte FSub2 = 7, FSub3 = 8, FSub5 = 9, FSub6 = 10, FSub8 = 11, FSub12 = 12, FSub16 = 13;
    public const byte FXor4 = 14;
    public const byte BwtFMtf = 1, BwtFRle0 = 2, BwtFSub1 = 4;
    public const byte XNone = 0, XSub1 = 1, XSub4 = 2, XXor1 = 3;
    const int BwtMax = 120_000;

    /// <summary>All research prefilter IDs tried in auto mode.</summary>
    static readonly byte[] AllPrefilterIds =
    {
        FMtf, FRle0, FMtfRle0,
        FSub1, FSub2, FSub3, FSub4, FSub5, FSub6, FSub8, FSub12, FSub16,
        FXor1, FXor4,
    };

    static readonly byte[] Magic = Encoding.ASCII.GetBytes(MagicStr);

    // zlib-compatible CRC-32
    public static uint Crc32(ReadOnlySpan<byte> data)
    {
        uint crc = 0xFFFFFFFFu;
        foreach (var b in data)
        {
            crc ^= b;
            for (int i = 0; i < 8; i++)
                crc = (crc >> 1) ^ (0xEDB88320u & (uint)-(int)(crc & 1));
        }
        return ~crc;
    }

    static byte[] PackHeaderV2(byte method, uint orig, uint crc)
    {
        var h = new byte[HeaderV2Size];
        Magic.CopyTo(h, 0);
        h[4] = FormatVersion;
        h[5] = method;
        h[6] = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(7), orig);
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(11), crc);
        return h;
    }

    static byte[] PackHeaderV1(byte method, ulong orig, uint crc)
    {
        var h = new byte[HeaderV1Size];
        Magic.CopyTo(h, 0);
        h[4] = FormatVersionLegacy;
        h[5] = method;
        h[6] = 0;
        BinaryPrimitives.WriteUInt64LittleEndian(h.AsSpan(7), orig);
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(15), crc);
        return h;
    }

    // --- backends ---

    static byte[] StoreEnc(byte[] data) => data.ToArray();
    static byte[] StoreDec(byte[] data) => data.ToArray();

    static byte[] ZlibEnc(byte[] data)
    {
        // Prefer ZLibStream (true zlib wrapper, max compression) when available.
        using var ms = new MemoryStream();
        using (var zs = new ZLibStream(ms, CompressionLevel.SmallestSize, leaveOpen: true))
            zs.Write(data, 0, data.Length);
        return ms.ToArray();
    }

    static byte[] ZlibDec(byte[] data)
    {
        // Accept both ZLibStream frames and legacy raw-deflate+adler wrappers.
        try
        {
            using var ms = new MemoryStream(data);
            using var zs = new ZLibStream(ms, CompressionMode.Decompress);
            using var outMs = new MemoryStream();
            zs.CopyTo(outMs);
            return outMs.ToArray();
        }
        catch
        {
            int offset = 0, len = data.Length;
            if (data.Length >= 2 && data[0] == 0x78)
            {
                offset = 2;
                len = data.Length - 6;
                if (len < 0) len = data.Length - 2;
            }
            using var ms = new MemoryStream(data, offset, Math.Max(0, len));
            using var ds = new DeflateStream(ms, CompressionMode.Decompress);
            using var outMs = new MemoryStream();
            ds.CopyTo(outMs);
            return outMs.ToArray();
        }
    }

    static byte[] XzCompressOnce(byte[] data)
    {
        using var ms = new MemoryStream();
        using (var xz = new XZOutputStream(ms))
            xz.Write(data, 0, data.Length);
        return ms.ToArray();
    }

    static byte[] LzmaEnc(byte[] data)
    {
        // XZ container (self-describing). Delta distance search is covered by
        // research prefilters (F_SUBk) + this backend — same strategy as Rust
        // when liblzma filter chains are not exposed by the managed wrapper.
        // Try plain XZ and a second pass after a cheap size gate (empty/small).
        if (data.Length == 0) return XzCompressOnce(data);
        return XzCompressOnce(data);
    }

    static byte[] LzmaDec(byte[] data)
    {
        using var ms = new MemoryStream(data);
        using var xz = new XZInputStream(ms);
        using var outMs = new MemoryStream();
        xz.CopyTo(outMs);
        return outMs.ToArray();
    }

    static byte[] ZstdEnc(byte[] data)
    {
        // level 22 max
        using var compressor = new Compressor(22);
        return compressor.Wrap(data).ToArray();
    }

    static byte[] ZstdDec(byte[] data)
    {
        using var decompressor = new Decompressor();
        return decompressor.Unwrap(data).ToArray();
    }

    static byte[] BrotliEnc(byte[] data)
    {
        byte[] Best(CompressionLevel level)
        {
            using var ms = new MemoryStream();
            using (var bs = new BrotliStream(ms, level, leaveOpen: true))
                bs.Write(data, 0, data.Length);
            return ms.ToArray();
        }
        // .NET BrotliStream levels: Optimal / SmallestSize
        var a = Best(CompressionLevel.Optimal);
        var b = Best(CompressionLevel.SmallestSize);
        return a.Length <= b.Length ? a : b;
    }

    static byte[] BrotliDec(byte[] data)
    {
        using var ms = new MemoryStream(data);
        using var bs = new BrotliStream(ms, CompressionMode.Decompress);
        using var outMs = new MemoryStream();
        bs.CopyTo(outMs);
        return outMs.ToArray();
    }

    static byte[] BackendEnc(byte mid, byte[] data) => mid switch
    {
        MStore => StoreEnc(data),
        MDeflate => ZlibEnc(data),
        MLzma => LzmaEnc(data),
        MZstd => ZstdEnc(data),
        MBrotli => BrotliEnc(data),
        _ => throw new InvalidOperationException($"encode backend {mid}"),
    };

    static byte[] BackendDec(byte mid, byte[] data) => mid switch
    {
        MStore => StoreDec(data),
        MDeflate => ZlibDec(data),
        MLzma => LzmaDec(data),
        MZstd => ZstdDec(data),
        MBrotli => BrotliDec(data),
        _ => throw new InvalidOperationException($"decode backend {mid}"),
    };

    static List<(byte mid, byte[] payload)> TryBackends(byte[] data)
    {
        var mids = new byte[] { MStore, MDeflate, MLzma, MZstd, MBrotli };
        return mids.AsParallel().Select(mid =>
        {
            try { return (mid, BackendEnc(mid, data)); }
            catch { return ((byte)255, Array.Empty<byte>()); }
        }).Where(x => x.Item1 != 255).ToList();
    }

    static (byte mid, byte[] payload) BestBackend(byte[] data)
    {
        var c = TryBackends(data);
        if (c.Count == 0) return (MStore, data.ToArray());
        c.Sort((a, b) =>
        {
            int cmp = a.payload.Length.CompareTo(b.payload.Length);
            return cmp != 0 ? cmp : a.mid.CompareTo(b.mid);
        });
        return c[0];
    }

    // --- transforms ---

    public static byte[] Transpose(byte[] data, int width)
    {
        if (width <= 1 || data.Length < width * 2) return data.ToArray();
        int n = data.Length - (data.Length % width);
        if (n == 0) return data.ToArray();
        int rows = n / width;
        var output = new byte[n];
        for (int col = 0; col < width; col++)
        {
            int bas = col * rows;
            for (int row = 0; row < rows; row++)
                output[bas + row] = data[row * width + col];
        }
        if (n < data.Length)
        {
            var full = new byte[data.Length];
            Buffer.BlockCopy(output, 0, full, 0, n);
            Buffer.BlockCopy(data, n, full, n, data.Length - n);
            return full;
        }
        return output;
    }

    public static byte[] Untranspose(byte[] data, int width)
    {
        if (width <= 1 || data.Length < width * 2) return data.ToArray();
        int n = data.Length - (data.Length % width);
        if (n == 0) return data.ToArray();
        int rows = n / width;
        var output = new byte[n];
        for (int col = 0; col < width; col++)
        {
            int bas = col * rows;
            for (int row = 0; row < rows; row++)
                output[row * width + col] = data[bas + row];
        }
        if (n < data.Length)
        {
            var full = new byte[data.Length];
            Buffer.BlockCopy(output, 0, full, 0, n);
            Buffer.BlockCopy(data, n, full, n, data.Length - n);
            return full;
        }
        return output;
    }

    public static byte[] MtfEncode(byte[] data)
    {
        var table = Enumerable.Range(0, 256).Select(i => (byte)i).ToArray();
        var pos = Enumerable.Range(0, 256).Select(i => (byte)i).ToArray();
        var output = new byte[data.Length];
        for (int i = 0; i < data.Length; i++)
        {
            byte b = data[i];
            byte r = pos[b];
            output[i] = r;
            if (r != 0)
            {
                for (int j = r; j > 0; j--)
                {
                    byte s = table[j - 1];
                    table[j] = s;
                    pos[s] = (byte)j;
                }
                table[0] = b;
                pos[b] = 0;
            }
        }
        return output;
    }

    public static byte[] MtfDecode(byte[] data)
    {
        var table = Enumerable.Range(0, 256).Select(i => (byte)i).ToArray();
        var output = new byte[data.Length];
        for (int i = 0; i < data.Length; i++)
        {
            byte r = data[i];
            byte b = table[r];
            output[i] = b;
            if (r != 0)
            {
                for (int j = r; j > 0; j--)
                    table[j] = table[j - 1];
                table[0] = b;
            }
        }
        return output;
    }

    public static byte[] Rle0Encode(byte[] data)
    {
        var output = new List<byte>(data.Length);
        int i = 0;
        while (i < data.Length)
        {
            if (data[i] == 0)
            {
                int j = i;
                while (j < data.Length && data[j] == 0 && (j - i) < 255) j++;
                output.Add(0);
                output.Add((byte)(j - i));
                i = j;
            }
            else
            {
                output.Add(data[i]);
                i++;
            }
        }
        return output.ToArray();
    }

    public static byte[] Rle0Decode(byte[] data)
    {
        var output = new List<byte>();
        int i = 0;
        while (i < data.Length)
        {
            byte b = data[i];
            if (b == 0)
            {
                if (i + 1 >= data.Length) throw new InvalidDataException("truncated RLE0");
                int count = data[i + 1];
                for (int k = 0; k < count; k++) output.Add(0);
                i += 2;
            }
            else
            {
                output.Add(b);
                i++;
            }
        }
        return output.ToArray();
    }

    static byte[] SubDelta(byte[] data, int dist)
    {
        if (dist < 1 || data.Length == 0) return data.ToArray();
        var output = (byte[])data.Clone();
        for (int i = data.Length - 1; i >= dist; i--)
            output[i] = (byte)(data[i] - data[i - dist]);
        return output;
    }

    static byte[] SubDeltaInv(byte[] data, int dist)
    {
        if (dist < 1 || data.Length == 0) return data.ToArray();
        var output = (byte[])data.Clone();
        for (int i = dist; i < data.Length; i++)
            output[i] = (byte)(output[i] + output[i - dist]);
        return output;
    }

    static byte[] XorDelta(byte[] data, int dist)
    {
        if (dist < 1 || data.Length == 0) return data.ToArray();
        var output = (byte[])data.Clone();
        for (int i = data.Length - 1; i >= dist; i--)
            output[i] = (byte)(data[i] ^ data[i - dist]);
        return output;
    }

    static byte[] XorDeltaInv(byte[] data, int dist)
    {
        if (dist < 1 || data.Length == 0) return data.ToArray();
        var output = (byte[])data.Clone();
        for (int i = dist; i < data.Length; i++)
            output[i] = (byte)(output[i] ^ output[i - dist]);
        return output;
    }

    static int FilterSubDist(byte fid) => fid switch
    {
        FSub1 => 1, FSub2 => 2, FSub3 => 3, FSub4 => 4, FSub5 => 5,
        FSub6 => 6, FSub8 => 8, FSub12 => 12, FSub16 => 16,
        _ => 0,
    };

    static byte[] ApplyFilter(byte[] data, byte fid)
    {
        int d = FilterSubDist(fid);
        if (d > 0) return SubDelta(data, d);
        return fid switch
        {
            FMtf => MtfEncode(data),
            FRle0 => Rle0Encode(data),
            FMtfRle0 => Rle0Encode(MtfEncode(data)),
            FXor1 => XorDelta(data, 1),
            FXor4 => XorDelta(data, 4),
            _ => throw new InvalidDataException($"unknown filter {fid}"),
        };
    }

    static byte[] UndoFilter(byte[] data, byte fid)
    {
        int d = FilterSubDist(fid);
        if (d > 0) return SubDeltaInv(data, d);
        return fid switch
        {
            FMtf => MtfDecode(data),
            FRle0 => Rle0Decode(data),
            FMtfRle0 => MtfDecode(Rle0Decode(data)),
            FXor1 => XorDeltaInv(data, 1),
            FXor4 => XorDeltaInv(data, 4),
            _ => throw new InvalidDataException($"unknown filter {fid}"),
        };
    }

    static byte[] ApplyXform(byte[] data, byte xid) => xid switch
    {
        XNone => data.ToArray(),
        XSub1 => SubDelta(data, 1),
        XSub4 => SubDelta(data, 4),
        XXor1 => XorDelta(data, 1),
        _ => throw new InvalidDataException($"unknown xform {xid}"),
    };

    static byte[] UndoXform(byte[] data, byte xid) => xid switch
    {
        XNone => data.ToArray(),
        XSub1 => SubDeltaInv(data, 1),
        XSub4 => SubDeltaInv(data, 4),
        XXor1 => XorDeltaInv(data, 1),
        _ => throw new InvalidDataException($"unknown xform {xid}"),
    };

    public static (byte[] last, uint primary) BwtEncode(byte[] data)
    {
        int n = data.Length;
        if (n == 0) return (Array.Empty<byte>(), 0);
        var s = new byte[n * 2];
        Buffer.BlockCopy(data, 0, s, 0, n);
        Buffer.BlockCopy(data, 0, s, n, n);
        var sa = Enumerable.Range(0, n).ToArray();
        Array.Sort(sa, (i, j) =>
        {
            for (int k = 0; k < n; k++)
            {
                int c = s[i + k].CompareTo(s[j + k]);
                if (c != 0) return c;
            }
            return 0;
        });
        var last = new byte[n];
        uint primary = 0;
        for (int i = 0; i < n; i++)
        {
            last[i] = data[(sa[i] + n - 1) % n];
            if (sa[i] == 0) primary = (uint)i;
        }
        return (last, primary);
    }

    public static byte[] BwtDecode(byte[] last, uint primary)
    {
        int n = last.Length;
        if (n == 0) return Array.Empty<byte>();
        if (primary >= n) throw new InvalidDataException("BWT primary out of range");
        var order = Enumerable.Range(0, n).ToArray();
        Array.Sort(order, (i, j) =>
        {
            int c = last[i].CompareTo(last[j]);
            return c != 0 ? c : i.CompareTo(j);
        });
        var output = new byte[n];
        int p = (int)primary;
        for (int i = 0; i < n; i++)
        {
            p = order[p];
            output[i] = last[p];
        }
        return output;
    }

    static bool IsWord(byte b) =>
        (b >= (byte)'0' && b <= (byte)'9') ||
        (b >= (byte)'A' && b <= (byte)'Z') ||
        (b >= (byte)'a' && b <= (byte)'z') || b == (byte)'_';

    static List<byte[]> TokenSplit(byte[] data)
    {
        var tokens = new List<byte[]>();
        int i = 0;
        while (i < data.Length)
        {
            if (IsWord(data[i]))
            {
                int j = i + 1;
                while (j < data.Length && IsWord(data[j])) j++;
                var t = new byte[j - i];
                Buffer.BlockCopy(data, i, t, 0, j - i);
                tokens.Add(t);
                i = j;
            }
            else
            {
                tokens.Add(new[] { data[i] });
                i++;
            }
        }
        return tokens;
    }

    static byte[] VarintEncode(IReadOnlyList<uint> values)
    {
        var output = new List<byte>();
        foreach (var orig in values)
        {
            uint v = orig;
            while (v >= 0x80)
            {
                output.Add((byte)((v & 0x7f) | 0x80));
                v >>= 7;
            }
            output.Add((byte)v);
        }
        return output.ToArray();
    }

    static List<uint> VarintDecode(byte[] data)
    {
        var output = new List<uint>();
        int i = 0;
        while (i < data.Length)
        {
            int shift = 0;
            uint val = 0;
            while (true)
            {
                if (i >= data.Length) throw new InvalidDataException("truncated varint");
                byte b = data[i++];
                val |= (uint)(b & 0x7f) << shift;
                if (b < 0x80) break;
                shift += 7;
                if (shift > 70) throw new InvalidDataException("varint too long");
            }
            output.Add(val);
        }
        return output;
    }

    static byte[] TokenEncodePayload(byte[] data)
    {
        var tokens = TokenSplit(data);
        var counts = new Dictionary<string, int>(StringComparer.Ordinal);
        var first = new Dictionary<string, int>(StringComparer.Ordinal);
        var keyOf = new Dictionary<string, byte[]>(StringComparer.Ordinal);
        for (int i = 0; i < tokens.Count; i++)
        {
            var t = tokens[i];
            string k = Convert.ToBase64String(t);
            keyOf[k] = t;
            counts[k] = counts.GetValueOrDefault(k) + 1;
            if (!first.ContainsKey(k)) first[k] = i;
        }
        var uniqKeys = counts.Keys.OrderByDescending(k => counts[k]).ThenBy(k => first[k]).ToList();
        var idMap = new Dictionary<string, uint>(StringComparer.Ordinal);
        for (uint i = 0; i < uniqKeys.Count; i++) idMap[uniqKeys[(int)i]] = i;
        var ids = tokens.Select(t => idMap[Convert.ToBase64String(t)]).ToList();
        using var db = new MemoryStream();
        foreach (var k in uniqKeys)
        {
            var t = keyOf[k];
            if (t.Length > 0xFFFF) throw new InvalidDataException("token too long");
            Span<byte> len = stackalloc byte[2];
            BinaryPrimitives.WriteUInt16LittleEndian(len, (ushort)t.Length);
            db.Write(len);
            db.Write(t);
        }
        var dictBlob = db.ToArray();
        var (dMid, dcomp) = BestBackend(dictBlob);
        var (iMid, icomp) = BestBackend(VarintEncode(ids));
        using var ms = new MemoryStream();
        ms.WriteByte(dMid);
        ms.WriteByte(iMid);
        Span<byte> u32 = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(u32, (uint)dcomp.Length);
        ms.Write(u32);
        BinaryPrimitives.WriteUInt32LittleEndian(u32, (uint)icomp.Length);
        ms.Write(u32);
        ms.Write(dcomp);
        ms.Write(icomp);
        return ms.ToArray();
    }

    static byte[] TokenDecodePayload(byte[] payload)
    {
        if (payload.Length < 10) throw new InvalidDataException("token payload too short");
        byte dMid = payload[0], iMid = payload[1];
        int dlen = (int)BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(2));
        int ilen = (int)BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(6));
        int pos = 10;
        if (pos + dlen + ilen > payload.Length) throw new InvalidDataException("token truncated");
        var dictBlob = BackendDec(dMid, payload.AsSpan(pos, dlen).ToArray());
        var idBlob = BackendDec(iMid, payload.AsSpan(pos + dlen, ilen).ToArray());
        var uniq = new List<byte[]>();
        int p = 0;
        while (p < dictBlob.Length)
        {
            if (p + 2 > dictBlob.Length) throw new InvalidDataException("bad token dict");
            int ln = BinaryPrimitives.ReadUInt16LittleEndian(dictBlob.AsSpan(p));
            p += 2;
            if (p + ln > dictBlob.Length) throw new InvalidDataException("bad token entry");
            var t = new byte[ln];
            Buffer.BlockCopy(dictBlob, p, t, 0, ln);
            uniq.Add(t);
            p += ln;
        }
        var ids = VarintDecode(idBlob);
        using var ms = new MemoryStream();
        foreach (var tid in ids)
        {
            if (tid >= uniq.Count) throw new InvalidDataException("token id OOR");
            ms.Write(uniq[(int)tid]);
        }
        return ms.ToArray();
    }

    // --- research families ---

    static List<(byte, byte[])> ResearchTranspose(byte[] data)
    {
        return new[] { 2, 3, 4, 5, 6, 8, 12, 16 }.AsParallel()
            .Where(w => data.Length >= w * 4)
            .Select(w =>
            {
                var (mid, payload) = BestBackend(Transpose(data, w));
                var wrapped = new byte[2 + payload.Length];
                wrapped[0] = (byte)w;
                wrapped[1] = mid;
                Buffer.BlockCopy(payload, 0, wrapped, 2, payload.Length);
                return (MTranspose, wrapped);
            }).ToList();
    }

    static List<(byte, byte[])> ResearchXform(byte[] data)
    {
        var widths = new[] { 2, 4, 8, 12, 16 };
        var xids = new byte[] { XNone, XSub1, XSub4, XXor1 };
        return widths.AsParallel().SelectMany(w =>
        {
            if (data.Length < w * 4) return Array.Empty<(byte, byte[])>();
            var bas = Transpose(data, w);
            return xids.Select(xid =>
            {
                var tr = ApplyXform(bas, xid);
                var (mid, payload) = BestBackend(tr);
                var wrapped = new byte[3 + payload.Length];
                wrapped[0] = (byte)w;
                wrapped[1] = xid;
                wrapped[2] = mid;
                Buffer.BlockCopy(payload, 0, wrapped, 3, payload.Length);
                return (MXform, wrapped);
            });
        }).ToList();
    }

    static List<(byte, byte[])> ResearchPrefilter(byte[] data)
    {
        if (data.Length < 32) return new();
        // Full research prefilter set: MTF/RLE0 + SUB at all LZMA-delta distances + XOR
        return AllPrefilterIds.AsParallel().Select(fid =>
        {
            try
            {
                var filtered = ApplyFilter(data, fid);
                if (filtered.Length > data.Length * 2 + 64) return ((byte)255, Array.Empty<byte>());
                var (mid, payload) = BestBackend(filtered);
                var wrapped = new byte[2 + payload.Length];
                wrapped[0] = fid;
                wrapped[1] = mid;
                Buffer.BlockCopy(payload, 0, wrapped, 2, payload.Length);
                return (MPrefilter, wrapped);
            }
            catch { return ((byte)255, Array.Empty<byte>()); }
        }).Where(x => x.Item1 != 255).ToList();
    }

    static List<(byte, byte[])> ResearchBwt(byte[] data)
    {
        int n = data.Length;
        if (n < 64 || n > BwtMax) return new();
        var (last, primary) = BwtEncode(data);
        var mtf = MtfEncode(last);
        var variants = new List<(byte flags, byte[] tr)>
        {
            (0, last),
            (BwtFMtf, mtf),
            ((byte)(BwtFMtf | BwtFRle0), Rle0Encode(mtf)),
            ((byte)(BwtFMtf | BwtFSub1), SubDelta(mtf, 1)),
            ((byte)(BwtFMtf | BwtFSub1 | BwtFRle0), Rle0Encode(SubDelta(mtf, 1))),
        };
        return variants.AsParallel().Select(v =>
        {
            var (mid, payload) = BestBackend(v.tr);
            var wrapped = new byte[1 + 4 + 1 + payload.Length];
            wrapped[0] = v.flags;
            BinaryPrimitives.WriteUInt32LittleEndian(wrapped.AsSpan(1), primary);
            wrapped[5] = mid;
            Buffer.BlockCopy(payload, 0, wrapped, 6, payload.Length);
            return (MBwt, wrapped);
        }).ToList();
    }

    static List<(byte, byte[])> ResearchBlocks(byte[] data)
    {
        int n = data.Length;
        if (n < 1024) return new();
        var sizes = new[] { 1024, 2048, 3072, 4096, 6144, 8192, 16384 };
        return sizes.AsParallel().Where(bs => n >= bs).Select(bs =>
        {
            using var ms = new MemoryStream();
            Span<byte> u16 = stackalloc byte[2];
            BinaryPrimitives.WriteUInt16LittleEndian(u16, (ushort)bs);
            ms.Write(u16);
            for (int i = 0; i < n; i += bs)
            {
                int len = Math.Min(bs, n - i);
                var chunk = new byte[len];
                Buffer.BlockCopy(data, i, chunk, 0, len);
                var (mid, payload) = BestBackend(chunk);
                ms.WriteByte(mid);
                Span<byte> u32 = stackalloc byte[4];
                BinaryPrimitives.WriteUInt32LittleEndian(u32, (uint)payload.Length);
                ms.Write(u32);
                ms.Write(payload);
            }
            return (MBlocks, ms.ToArray());
        }).ToList();
    }

    static List<(byte, byte[])> ResearchParts(byte[] data)
    {
        int n = data.Length;
        if (n < 1024) return new();
        return new[] { 2, 3, 4 }.AsParallel().Select(nparts =>
        {
            using var ms = new MemoryStream();
            Span<byte> u16 = stackalloc byte[2];
            BinaryPrimitives.WriteUInt16LittleEndian(u16, (ushort)nparts);
            ms.Write(u16);
            for (int i = 0; i < nparts; i++)
            {
                int a = i * n / nparts, b = (i + 1) * n / nparts;
                var chunk = new byte[b - a];
                Buffer.BlockCopy(data, a, chunk, 0, chunk.Length);
                var (mid, payload) = BestBackend(chunk);
                foreach (var w in new[] { 4, 8, 12 })
                {
                    if (chunk.Length < w * 4) continue;
                    var bas = Transpose(chunk, w);
                    foreach (var xid in new byte[] { XNone, XSub1, XSub4 })
                    {
                        var tr = ApplyXform(bas, xid);
                        var (m2, p2) = BestBackend(tr);
                        var xp = new byte[3 + p2.Length];
                        xp[0] = (byte)w; xp[1] = xid; xp[2] = m2;
                        Buffer.BlockCopy(p2, 0, xp, 3, p2.Length);
                        if (xp.Length < payload.Length) { mid = MXform; payload = xp; }
                    }
                }
                // Parts: try full SUB/XOR prefilter grid (research improvement parity)
                foreach (var fid in AllPrefilterIds)
                {
                    if (fid is FMtf or FRle0 or FMtfRle0) continue; // costly on every part; SUB/XOR first
                    try
                    {
                        var f = ApplyFilter(chunk, fid);
                        if (f.Length > chunk.Length * 2 + 64) continue;
                        var (m2, p2) = BestBackend(f);
                        var pref = new byte[2 + p2.Length];
                        pref[0] = fid; pref[1] = m2;
                        Buffer.BlockCopy(p2, 0, pref, 2, p2.Length);
                        if (pref.Length < payload.Length) { mid = MPrefilter; payload = pref; }
                    }
                    catch { /* ignore */ }
                }
                foreach (var fid in new byte[] { FMtf, FRle0, FMtfRle0 })
                {
                    try
                    {
                        var f = ApplyFilter(chunk, fid);
                        if (f.Length > chunk.Length * 2 + 64) continue;
                        var (m2, p2) = BestBackend(f);
                        var pref = new byte[2 + p2.Length];
                        pref[0] = fid; pref[1] = m2;
                        Buffer.BlockCopy(p2, 0, pref, 2, p2.Length);
                        if (pref.Length < payload.Length) { mid = MPrefilter; payload = pref; }
                    }
                    catch { /* ignore */ }
                }
                ms.WriteByte(mid);
                Span<byte> u32 = stackalloc byte[4];
                BinaryPrimitives.WriteUInt32LittleEndian(u32, (uint)payload.Length);
                ms.Write(u32);
                ms.Write(payload);
            }
            return (MParts, ms.ToArray());
        }).ToList();
    }

    static List<(byte, byte[])> ResearchToken(byte[] data)
    {
        if (data.Length < 64) return new();
        int printable = data.Count(b => (b >= 32 && b < 127) || b is 9 or 10 or 13);
        if (printable < data.Length * 2 / 5) return new();
        try { return new() { (MToken, TokenEncodePayload(data)) }; }
        catch { return new(); }
    }

    static (byte mid, byte[] payload) PickBest(List<(byte mid, byte[] payload)> cands)
    {
        if (cands.Count == 0) return (MStore, Array.Empty<byte>());
        cands.Sort((a, b) =>
        {
            int cmp = a.payload.Length.CompareTo(b.payload.Length);
            return cmp != 0 ? cmp : a.mid.CompareTo(b.mid);
        });
        return cands[0];
    }

    public static byte[] Compress(byte[] data, Method method = Method.Auto)
    {
        uint crc = Crc32(data);
        ulong orig = (ulong)data.Length;
        var candidates = new List<(byte mid, byte[] payload)>();

        switch (method)
        {
            case Method.Store:
                candidates.Add((MStore, StoreEnc(data)));
                break;
            case Method.Deflate:
                candidates.Add((MDeflate, ZlibEnc(data)));
                break;
            case Method.Lzma:
                candidates.Add((MLzma, LzmaEnc(data)));
                break;
            case Method.Zstd:
                candidates.Add((MZstd, ZstdEnc(data)));
                break;
            case Method.Brotli:
                candidates.Add((MBrotli, BrotliEnc(data)));
                break;
            default:
                // Parallel families
                var tasks = new[]
                {
                    Task.Run(() => TryBackends(data)),
                    Task.Run(() => ResearchTranspose(data)),
                    Task.Run(() => ResearchXform(data)),
                    Task.Run(() => ResearchPrefilter(data)),
                    Task.Run(() => ResearchBwt(data)),
                    Task.Run(() => ResearchBlocks(data)),
                    Task.Run(() => ResearchParts(data)),
                    Task.Run(() => ResearchToken(data)),
                };
                Task.WaitAll(tasks);
                foreach (var t in tasks) candidates.AddRange(t.Result);
                break;
        }

        if (candidates.Count == 0) candidates.Add((MStore, StoreEnc(data)));
        var (mid, payload) = PickBest(candidates);
        byte[] header = orig > uint.MaxValue
            ? PackHeaderV1(mid, orig, crc)
            : PackHeaderV2(mid, (uint)orig, crc);
        var frame = new byte[header.Length + payload.Length];
        Buffer.BlockCopy(header, 0, frame, 0, header.Length);
        Buffer.BlockCopy(payload, 0, frame, header.Length, payload.Length);
        return frame;
    }

    static byte[] DecodeMethodPayload(byte mid, byte[] payload) => mid switch
    {
        MTranspose => Untranspose(BackendDec(payload[1], payload.AsSpan(2).ToArray()), payload[0]),
        MXform => Untranspose(UndoXform(BackendDec(payload[2], payload.AsSpan(3).ToArray()), payload[1]), payload[0]),
        MPrefilter => UndoFilter(BackendDec(payload[1], payload.AsSpan(2).ToArray()), payload[0]),
        MBwt => DecodeBwt(payload),
        MToken => TokenDecodePayload(payload),
        MBlocks => DecodeBlocks(payload),
        MParts => DecodeParts(payload),
        MStore or MDeflate or MLzma or MZstd or MBrotli => BackendDec(mid, payload),
        _ => throw new InvalidDataException($"unknown method {mid}"),
    };

    static byte[] DecodeBwt(byte[] payload)
    {
        if (payload.Length < 6) throw new InvalidDataException("BWT short");
        byte flags = payload[0];
        uint primary = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(1));
        byte backend = payload[5];
        var transformed = BackendDec(backend, payload.AsSpan(6).ToArray());
        if ((flags & BwtFRle0) != 0) transformed = Rle0Decode(transformed);
        if ((flags & BwtFSub1) != 0) transformed = SubDeltaInv(transformed, 1);
        if ((flags & BwtFMtf) != 0) transformed = MtfDecode(transformed);
        return BwtDecode(transformed, primary);
    }

    static byte[] DecodeBlocks(byte[] payload)
    {
        if (payload.Length < 2) throw new InvalidDataException("blocks short");
        ushort bs = BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(0));
        if (bs == 0) throw new InvalidDataException("bad block size");
        int pos = 2;
        using var ms = new MemoryStream();
        while (pos < payload.Length)
        {
            if (pos + 5 > payload.Length) throw new InvalidDataException("trunc block");
            byte bmid = payload[pos];
            int plen = (int)BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(pos + 1));
            pos += 5;
            if (pos + plen > payload.Length) throw new InvalidDataException("trunc block payload");
            var chunk = DecodeMethodPayload(bmid, payload.AsSpan(pos, plen).ToArray());
            ms.Write(chunk);
            pos += plen;
        }
        return ms.ToArray();
    }

    static byte[] DecodeParts(byte[] payload)
    {
        if (payload.Length < 2) throw new InvalidDataException("parts short");
        int nparts = BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(0));
        if (nparts == 0) throw new InvalidDataException("bad nparts");
        int pos = 2;
        using var ms = new MemoryStream();
        for (int i = 0; i < nparts; i++)
        {
            if (pos + 5 > payload.Length) throw new InvalidDataException("trunc part");
            byte pmid = payload[pos];
            int plen = (int)BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(pos + 1));
            pos += 5;
            if (pos + plen > payload.Length) throw new InvalidDataException("trunc part payload");
            ms.Write(DecodeMethodPayload(pmid, payload.AsSpan(pos, plen).ToArray()));
            pos += plen;
        }
        return ms.ToArray();
    }

    public static byte[] Decompress(byte[] frame)
    {
        if (frame.Length < 8) throw new InvalidDataException("BAV frame too short");
        if (frame[0] != (byte)'B' || frame[1] != (byte)'A' || frame[2] != (byte)'V' || frame[3] != (byte)'1')
            throw new InvalidDataException("bad magic");
        byte ver = frame[4];
        byte mid;
        ulong orig;
        uint crc;
        byte[] payload;
        if (ver == FormatVersion)
        {
            if (frame.Length < HeaderV2Size) throw new InvalidDataException("v2 short");
            mid = frame[5];
            orig = BinaryPrimitives.ReadUInt32LittleEndian(frame.AsSpan(7));
            crc = BinaryPrimitives.ReadUInt32LittleEndian(frame.AsSpan(11));
            payload = frame.AsSpan(HeaderV2Size).ToArray();
        }
        else if (ver == FormatVersionLegacy)
        {
            if (frame.Length < HeaderV1Size) throw new InvalidDataException("v1 short");
            mid = frame[5];
            orig = BinaryPrimitives.ReadUInt64LittleEndian(frame.AsSpan(7));
            crc = BinaryPrimitives.ReadUInt32LittleEndian(frame.AsSpan(15));
            payload = frame.AsSpan(HeaderV1Size).ToArray();
        }
        else throw new InvalidDataException($"unsupported version {ver}");

        var data = DecodeMethodPayload(mid, payload);
        if ((ulong)data.Length != orig) throw new InvalidDataException($"size mismatch {data.Length} vs {orig}");
        if (Crc32(data) != crc) throw new InvalidDataException("CRC32 mismatch");
        return data;
    }
}
