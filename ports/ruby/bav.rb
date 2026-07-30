#!/usr/bin/env ruby
# frozen_string_literal: true
# BAV1 research codec — Ruby port of src/bav/codec.py

require "zlib"
require "stringio"

module Bav
  MAGIC = "BAV1"
  FORMAT_VERSION = 1
  HEADER_SIZE = 19
  M_STORE = 0
  M_DEFLATE = 1
  M_TRANSPOSE = 4
  M_PREFILTER = 6
  M_BWT = 7
  F_MTF = 1
  F_RLE0 = 2
  F_MTF_RLE0 = 3
  BWT_F_MTF = 1
  BWT_F_RLE0 = 2
  BWT_MAX = 120_000

  module_function

  def crc32(data)
    Zlib.crc32(data) & 0xFFFFFFFF
  end

  def pack_header(method, orig, crc)
    [MAGIC, FORMAT_VERSION, method, 0, orig, crc].pack("a4CCCQ<L<")
  end

  def mtf_encode(data)
    table = (0..255).to_a
    pos = (0..255).to_a
    out = String.new(encoding: Encoding::BINARY)
    data.each_byte do |b|
      r = pos[b]
      out << r.chr
      if r != 0
        r.downto(1) do |j|
          s = table[j - 1]
          table[j] = s
          pos[s] = j
        end
        table[0] = b
        pos[b] = 0
      end
    end
    out
  end

  def mtf_decode(data)
    table = (0..255).to_a
    out = String.new(encoding: Encoding::BINARY)
    data.each_byte do |r|
      b = table[r]
      out << b.chr
      if r != 0
        r.downto(1) { |j| table[j] = table[j - 1] }
        table[0] = b
      end
    end
    out
  end

  def rle0_encode(data)
    out = String.new(encoding: Encoding::BINARY)
    i = 0
    bytes = data.bytes
    n = bytes.length
    while i < n
      if bytes[i] == 0
        j = i
        j += 1 while j < n && bytes[j] == 0 && (j - i) < 255
        out << 0.chr << (j - i).chr
        i = j
      else
        out << bytes[i].chr
        i += 1
      end
    end
    out
  end

  def rle0_decode(data)
    out = String.new(encoding: Encoding::BINARY)
    bytes = data.bytes
    i = 0
    while i < bytes.length
      if bytes[i] == 0
        raise "truncated RLE0" if i + 1 >= bytes.length
        out << ("\x00" * bytes[i + 1])
        i += 2
      else
        out << bytes[i].chr
        i += 1
      end
    end
    out
  end

  def transpose(data, width)
    return data.dup if width <= 1 || data.bytesize < width * 2
    n = data.bytesize - (data.bytesize % width)
    return data.dup if n == 0
    body = data.byteslice(0, n)
    tail = data.byteslice(n, data.bytesize - n) || ""
    rows = n / width
    out = Array.new(n)
    width.times do |col|
      base = col * rows
      rows.times { |row| out[base + row] = body.getbyte(row * width + col) }
    end
    out.pack("C*") + tail
  end

  def untranspose(data, width)
    return data.dup if width <= 1 || data.bytesize < width * 2
    n = data.bytesize - (data.bytesize % width)
    body = data.byteslice(0, n)
    tail = data.byteslice(n, data.bytesize - n) || ""
    rows = n / width
    out = Array.new(n)
    width.times do |col|
      base = col * rows
      rows.times { |row| out[row * width + col] = body.getbyte(base + row) }
    end
    out.pack("C*") + tail
  end

  def bwt_encode(data)
    n = data.bytesize
    return ["", 0] if n == 0
    s = data + data
    sa = (0...n).sort_by { |i| s.byteslice(i, n) }
    last = sa.map { |i| data.getbyte((i - 1) % n) }.pack("C*")
    primary = sa.index(0)
    [last, primary]
  end

  def bwt_decode(last, primary)
    n = last.bytesize
    return "" if n == 0
    raise "BWT primary OOR" if primary < 0 || primary >= n
    order = (0...n).sort_by { |i| [last.getbyte(i), i] }
    out = String.new(encoding: Encoding::BINARY)
    p = primary
    n.times do
      p = order[p]
      out << last.getbyte(p).chr
    end
    out
  end

  def deflate_enc(data)
    Zlib::Deflate.deflate(data, 9)
  end

  def deflate_dec(data)
    Zlib::Inflate.inflate(data)
  end

  def best_backend(data)
    store = data
    defl = deflate_enc(data)
    if defl.bytesize < store.bytesize
      [M_DEFLATE, defl]
    else
      [M_STORE, store]
    end
  end

  def backend_dec(mid, payload)
    case mid
    when M_STORE then payload
    when M_DEFLATE then deflate_dec(payload)
    else raise "unknown backend #{mid}"
    end
  end

  def compress(data, method: :auto)
    data = data.b
    crc = crc32(data)
    orig = data.bytesize
    cands = []

    case method.to_sym
    when :store
      cands << [M_STORE, data]
    when :deflate
      cands << [M_DEFLATE, deflate_enc(data)]
    else
      cands << [M_STORE, data]
      cands << [M_DEFLATE, deflate_enc(data)]
      if data.bytesize >= 32
        [F_MTF, F_RLE0, F_MTF_RLE0].each do |fid|
          filtered = case fid
                     when F_MTF then mtf_encode(data)
                     when F_RLE0 then rle0_encode(data)
                     else rle0_encode(mtf_encode(data))
                     end
          next if filtered.bytesize > data.bytesize * 2 + 64
          mid, payload = best_backend(filtered)
          cands << [M_PREFILTER, [fid, mid].pack("CC") + payload]
        end
      end
      [2, 4, 8, 16].each do |w|
        next if data.bytesize < w * 4
        t = transpose(data, w)
        mid, payload = best_backend(t)
        cands << [M_TRANSPOSE, [w, mid].pack("CC") + payload]
      end
      if data.bytesize >= 64 && data.bytesize <= BWT_MAX
        last, primary = bwt_encode(data)
        mtf = mtf_encode(last)
        mid, payload = best_backend(mtf)
        cands << [M_BWT, [BWT_F_MTF].pack("C") + [primary].pack("L<") + [mid].pack("C") + payload]
      end
    end

    cands.sort_by! { |mid, p| [p.bytesize, mid] }
    mid, payload = cands.first
    pack_header(mid, orig, crc) + payload
  end

  def decompress(frame)
    frame = frame.b
    raise "frame too short" if frame.bytesize < HEADER_SIZE
    magic, ver, mid, _flags, orig, crc = frame.byteslice(0, HEADER_SIZE).unpack("a4CCCQ<L<")
    raise "bad magic" unless magic == MAGIC
    raise "bad version" unless ver == FORMAT_VERSION
    payload = frame.byteslice(HEADER_SIZE, frame.bytesize - HEADER_SIZE) || ""

    data = case mid
           when M_TRANSPOSE
             width, backend = payload.byteslice(0, 2).unpack("CC")
             transformed = backend_dec(backend, payload.byteslice(2..-1) || "")
             untranspose(transformed, width)
           when M_PREFILTER
             fid, backend = payload.byteslice(0, 2).unpack("CC")
             filtered = backend_dec(backend, payload.byteslice(2..-1) || "")
             case fid
             when F_MTF then mtf_decode(filtered)
             when F_RLE0 then rle0_decode(filtered)
             when F_MTF_RLE0 then mtf_decode(rle0_decode(filtered))
             else raise "unknown filter"
             end
           when M_BWT
             flags = payload.getbyte(0)
             primary = payload.byteslice(1, 4).unpack1("L<")
             backend = payload.getbyte(5)
             transformed = backend_dec(backend, payload.byteslice(6..-1) || "")
             transformed = rle0_decode(transformed) if (flags & BWT_F_RLE0) != 0
             transformed = mtf_decode(transformed) if (flags & BWT_F_MTF) != 0
             bwt_decode(transformed, primary)
           else
             backend_dec(mid, payload)
           end

    raise "size mismatch" unless data.bytesize == orig
    raise "CRC mismatch" unless crc32(data) == crc
    data
  end
end

if $PROGRAM_NAME == __FILE__
  cmd = ARGV.shift
  case cmd
  when "version"
    puts "bav-ruby 0.1.0"
  when "compress"
    method = "auto"
    out = nil
    input = nil
    while (a = ARGV.shift)
      if a == "-o" || a == "--output"
        out = ARGV.shift
      elsif a == "-m" || a == "--method"
        method = ARGV.shift
      else
        input = a
      end
    end
    abort "missing input" unless input
    data = File.binread(input)
    frame = Bav.compress(data, method: method.to_sym)
    out ||= "#{input}.bav"
    File.binwrite(out, frame)
    puts "compressed #{data.bytesize} -> #{frame.bytesize} bytes (#{out})"
  when "decompress"
    out = nil
    input = nil
    while (a = ARGV.shift)
      if a == "-o" || a == "--output"
        out = ARGV.shift
      else
        input = a
      end
    end
    abort "missing input" unless input
    frame = File.binread(input)
    data = Bav.decompress(frame)
    out ||= input.end_with?(".bav") ? input[0...-4] : "#{input}.out"
    File.binwrite(out, data)
    puts "decompressed #{frame.bytesize} -> #{data.bytesize} bytes (#{out})"
  else
    warn "usage: bav.rb compress|decompress|version ..."
    exit 2
  end
end
