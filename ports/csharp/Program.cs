using Bav;

static int Usage()
{
    Console.Error.WriteLine(
        """
        BAV research compressor — MAIN port (C# full research)
        usage:
          bav-csharp compress <in> [-o out] [-m auto|store|deflate|lzma|zstd|brotli|research]
          bav-csharp decompress <in> [-o out]
          bav-csharp version

        Default method is auto (all backends + full research paths).
        """);
    return 2;
}

if (args.Length == 0) return Usage();
var cmd = args[0].ToLowerInvariant();
if (cmd is "version" or "--version" or "-v")
{
    Console.WriteLine("bav-csharp 0.3.0 (main port, full research)");
    return 0;
}

if (cmd == "compress")
{
    if (args.Length < 2) return Usage();
    string input = args[1], output = input + ".bav", method = "auto";
    for (int i = 2; i < args.Length; i++)
    {
        if ((args[i] is "-o" or "--output") && i + 1 < args.Length) output = args[++i];
        else if ((args[i] is "-m" or "--method") && i + 1 < args.Length) method = args[++i];
    }
    var data = File.ReadAllBytes(input);
    var m = MethodParse.Parse(method);
    var frame = Codec.Compress(data, m);
    File.WriteAllBytes(output, frame);
    Console.WriteLine($"compressed {data.Length} -> {frame.Length} bytes ({output})");
    return 0;
}

if (cmd == "decompress")
{
    if (args.Length < 2) return Usage();
    string input = args[1];
    string output = input.EndsWith(".bav", StringComparison.OrdinalIgnoreCase)
        ? input[..^4]
        : input + ".out";
    for (int i = 2; i < args.Length; i++)
        if ((args[i] is "-o" or "--output") && i + 1 < args.Length) output = args[++i];
    var frame = File.ReadAllBytes(input);
    var data = Codec.Decompress(frame);
    File.WriteAllBytes(output, data);
    Console.WriteLine($"decompressed {frame.Length} -> {data.Length} bytes ({output})");
    return 0;
}

return Usage();

static class MethodParse
{
    public static Method Parse(string s) => s.ToLowerInvariant() switch
    {
        "auto" => Method.Auto,
        "store" => Method.Store,
        "deflate" => Method.Deflate,
        "lzma" => Method.Lzma,
        "zstd" => Method.Zstd,
        "brotli" => Method.Brotli,
        "research" => Method.Research,
        _ => throw new ArgumentException($"unknown method: {s}"),
    };
}
