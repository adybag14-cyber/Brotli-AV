//! BAV research codec — full parity port of `src/bav/codec.py` (gen2).
//!
//! Wire: BAV v2 header (default) magic `BAV1`, version 2, method, flags,
//! orig u32 LE, CRC32 LE; v1 (orig u64) still decodes.
//!
//! Auto mode tries: store / deflate-9 / lzma-extreme(+delta) / zstd-22 / brotli-11,
//! transpose, xform, prefilters, BWT(+MTF/RLE0/sub), multi-block, parts, token.
//! Independent candidate families are evaluated in parallel (rayon).

use brotli::enc::BrotliCompress;
use brotli::enc::BrotliEncoderParams;
use flate2::read::ZlibDecoder;
use flate2::write::ZlibEncoder;
use flate2::Compression;
use rayon::prelude::*;
use std::collections::HashMap;
use std::io::{Cursor, Read, Write};
use xz2::stream::{Action, Check, Filters, LzmaOptions, Status, Stream};
use xz2::write::XzEncoder;

pub const MAGIC: &[u8; 4] = b"BAV1";
pub const FORMAT_VERSION: u8 = 2;
pub const FORMAT_VERSION_LEGACY: u8 = 1;

pub const M_STORE: u8 = 0;
pub const M_DEFLATE: u8 = 1;
pub const M_LZMA: u8 = 2;
pub const M_ZSTD: u8 = 3;
pub const M_TRANSPOSE: u8 = 4;
pub const M_BROTLI: u8 = 5;
pub const M_PREFILTER: u8 = 6;
pub const M_BWT: u8 = 7;
pub const M_BLOCKS: u8 = 8;
pub const M_TOKEN: u8 = 9;
pub const M_XFORM: u8 = 10;
pub const M_PARTS: u8 = 11;

pub const F_MTF: u8 = 1;
pub const F_RLE0: u8 = 2;
pub const F_MTF_RLE0: u8 = 3;
pub const F_SUB1: u8 = 4;
pub const F_SUB4: u8 = 5;
pub const F_XOR1: u8 = 6;

pub const BWT_F_MTF: u8 = 1;
pub const BWT_F_RLE0: u8 = 2;
pub const BWT_F_SUB1: u8 = 4;

pub const X_NONE: u8 = 0;
pub const X_SUB1: u8 = 1;
pub const X_SUB4: u8 = 2;
pub const X_XOR1: u8 = 3;

const HEADER_V1_SIZE: usize = 19; // 4+1+1+1+8+4
const HEADER_V2_SIZE: usize = 15; // 4+1+1+1+4+4
const BWT_MAX_BYTES: usize = 120_000;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Method {
    Auto,
    Store,
    Deflate,
    Lzma,
    Zstd,
    Brotli,
    Research,
}

impl Method {
    pub fn parse(s: &str) -> Result<Self, String> {
        match s.to_ascii_lowercase().as_str() {
            "auto" => Ok(Self::Auto),
            "store" => Ok(Self::Store),
            "deflate" => Ok(Self::Deflate),
            "lzma" => Ok(Self::Lzma),
            "zstd" => Ok(Self::Zstd),
            "brotli" => Ok(Self::Brotli),
            "research" => Ok(Self::Research),
            other => Err(format!("unknown method: {other}")),
        }
    }
}

pub fn crc32(data: &[u8]) -> u32 {
    // Match Python zlib.crc32 (ISO-HDLC), not the inverse of some other CRCs.
    crc32fast::hash(data)
}

fn pack_header_v2(method: u8, orig: u32, crc: u32) -> [u8; HEADER_V2_SIZE] {
    let mut h = [0u8; HEADER_V2_SIZE];
    h[0..4].copy_from_slice(MAGIC);
    h[4] = FORMAT_VERSION;
    h[5] = method;
    h[6] = 0;
    h[7..11].copy_from_slice(&orig.to_le_bytes());
    h[11..15].copy_from_slice(&crc.to_le_bytes());
    h
}

fn pack_header_v1(method: u8, orig: u64, crc: u32) -> [u8; HEADER_V1_SIZE] {
    let mut h = [0u8; HEADER_V1_SIZE];
    h[0..4].copy_from_slice(MAGIC);
    h[4] = FORMAT_VERSION_LEGACY;
    h[5] = method;
    h[6] = 0;
    h[7..15].copy_from_slice(&orig.to_le_bytes());
    h[15..19].copy_from_slice(&crc.to_le_bytes());
    h
}

// --- backends ---

fn store_enc(data: &[u8]) -> Vec<u8> {
    data.to_vec()
}

fn store_dec(data: &[u8]) -> Vec<u8> {
    data.to_vec()
}

fn deflate_enc(data: &[u8]) -> Result<Vec<u8>, String> {
    let mut enc = ZlibEncoder::new(Vec::new(), Compression::best());
    enc.write_all(data).map_err(|e| e.to_string())?;
    enc.finish().map_err(|e| e.to_string())
}

fn deflate_dec(data: &[u8]) -> Result<Vec<u8>, String> {
    let mut out = Vec::new();
    ZlibDecoder::new(data)
        .read_to_end(&mut out)
        .map_err(|e| e.to_string())?;
    Ok(out)
}

/// LZMA2/XZ at max preset. Delta distance search is covered by SUB prefilters
/// + this backend (xz2 crate does not expose FILTER_DELTA on Filters).
fn lzma_enc(data: &[u8]) -> Result<Vec<u8>, String> {
    // Python: preset=9|PRESET_EXTREME. xz2 preset is 0..=9.
    let mut best = xz_compress_preset(data, 9)?;
    // Also try lzma2-only stream at preset 9 via filter API (sometimes smaller framing).
    if let Ok(cand) = xz_compress_lzma2_only(data, 9) {
        if cand.len() < best.len() {
            best = cand;
        }
    }
    Ok(best)
}

fn xz_compress_preset(data: &[u8], preset: u32) -> Result<Vec<u8>, String> {
    let mut enc = XzEncoder::new(Vec::new(), preset);
    enc.write_all(data).map_err(|e| e.to_string())?;
    enc.finish().map_err(|e| e.to_string())
}

fn xz_compress_lzma2_only(data: &[u8], preset: u32) -> Result<Vec<u8>, String> {
    let mut filters = Filters::new();
    let opt = LzmaOptions::new_preset(preset).map_err(|e| format!("lzma preset: {e}"))?;
    filters.lzma2(&opt);
    let mut stream =
        Stream::new_stream_encoder(&filters, Check::Crc64).map_err(|e| format!("xz: {e}"))?;
    let mut out = Vec::new();
    let mut in_pos = 0usize;
    let mut out_buf = vec![0u8; 1 << 16];
    loop {
        let finish = in_pos >= data.len();
        let action = if finish {
            Action::Finish
        } else {
            Action::Run
        };
        let before_in = stream.total_in();
        let before_out = stream.total_out();
        let status = stream
            .process(&data[in_pos..], &mut out_buf, action)
            .map_err(|e| format!("xz: {e}"))?;
        let read = (stream.total_in() - before_in) as usize;
        let written = (stream.total_out() - before_out) as usize;
        in_pos += read;
        out.extend_from_slice(&out_buf[..written]);
        if matches!(status, Status::StreamEnd) {
            break;
        }
        if finish && written == 0 {
            out_buf.resize(out_buf.len() * 2, 0);
        }
    }
    Ok(out)
}

fn lzma_dec(data: &[u8]) -> Result<Vec<u8>, String> {
    let mut dec = xz2::read::XzDecoder::new(data);
    let mut out = Vec::new();
    dec.read_to_end(&mut out).map_err(|e| e.to_string())?;
    Ok(out)
}

fn zstd_enc(data: &[u8]) -> Result<Vec<u8>, String> {
    zstd::encode_all(data, 22).map_err(|e| e.to_string())
}

fn zstd_dec(data: &[u8]) -> Result<Vec<u8>, String> {
    zstd::decode_all(data).map_err(|e| e.to_string())
}

fn brotli_enc(data: &[u8]) -> Result<Vec<u8>, String> {
    let mut best = brotli_once(data, 11, 0)?;
    // mode 0 generic, 1 text, 2 font — try text/font like Python
    for mode in [1u32, 2] {
        if let Ok(cand) = brotli_once(data, 11, mode) {
            if cand.len() < best.len() {
                best = cand;
            }
        }
    }
    Ok(best)
}

fn brotli_once(data: &[u8], quality: u32, mode: u32) -> Result<Vec<u8>, String> {
    let mut params = BrotliEncoderParams::default();
    params.quality = quality as i32;
    params.mode = match mode {
        1 => brotli::enc::backward_references::BrotliEncoderMode::BROTLI_MODE_TEXT,
        2 => brotli::enc::backward_references::BrotliEncoderMode::BROTLI_MODE_FONT,
        _ => brotli::enc::backward_references::BrotliEncoderMode::BROTLI_MODE_GENERIC,
    };
    let mut input = Cursor::new(data);
    let mut output = Vec::new();
    BrotliCompress(&mut input, &mut output, &params).map_err(|e| e.to_string())?;
    Ok(output)
}

fn brotli_dec(data: &[u8]) -> Result<Vec<u8>, String> {
    let mut input = Cursor::new(data);
    let mut output = Vec::new();
    brotli::BrotliDecompress(&mut input, &mut output).map_err(|e| format!("brotli dec: {e}"))?;
    Ok(output)
}

fn backend_enc(mid: u8, data: &[u8]) -> Result<Vec<u8>, String> {
    match mid {
        M_STORE => Ok(store_enc(data)),
        M_DEFLATE => deflate_enc(data),
        M_LZMA => lzma_enc(data),
        M_ZSTD => zstd_enc(data),
        M_BROTLI => brotli_enc(data),
        _ => Err(format!("encode backend {mid} not linked")),
    }
}

fn backend_dec(mid: u8, data: &[u8]) -> Result<Vec<u8>, String> {
    match mid {
        M_STORE => Ok(store_dec(data)),
        M_DEFLATE => deflate_dec(data),
        M_LZMA => lzma_dec(data),
        M_ZSTD => zstd_dec(data),
        M_BROTLI => brotli_dec(data),
        _ => Err(format!("decode backend {mid} not linked")),
    }
}

fn try_backends(data: &[u8]) -> Vec<(u8, Vec<u8>)> {
    let mids = [M_STORE, M_DEFLATE, M_LZMA, M_ZSTD, M_BROTLI];
    mids.par_iter()
        .filter_map(|&mid| backend_enc(mid, data).ok().map(|p| (mid, p)))
        .collect()
}

fn best_backend(data: &[u8]) -> (u8, Vec<u8>) {
    let mut c = try_backends(data);
    if c.is_empty() {
        return (M_STORE, data.to_vec());
    }
    c.sort_by(|a, b| a.1.len().cmp(&b.1.len()).then(a.0.cmp(&b.0)));
    c.into_iter().next().unwrap()
}

// --- transforms ---

pub fn transpose(data: &[u8], width: usize) -> Vec<u8> {
    if width <= 1 || data.len() < width * 2 {
        return data.to_vec();
    }
    let n = data.len() - (data.len() % width);
    if n == 0 {
        return data.to_vec();
    }
    let body = &data[..n];
    let tail = &data[n..];
    let rows = n / width;
    let mut out = vec![0u8; n];
    for col in 0..width {
        let base = col * rows;
        for row in 0..rows {
            out[base + row] = body[row * width + col];
        }
    }
    out.extend_from_slice(tail);
    out
}

pub fn untranspose(data: &[u8], width: usize) -> Vec<u8> {
    if width <= 1 || data.len() < width * 2 {
        return data.to_vec();
    }
    let n = data.len() - (data.len() % width);
    if n == 0 {
        return data.to_vec();
    }
    let body = &data[..n];
    let tail = &data[n..];
    let rows = n / width;
    let mut out = vec![0u8; n];
    for col in 0..width {
        let base = col * rows;
        for row in 0..rows {
            out[row * width + col] = body[base + row];
        }
    }
    out.extend_from_slice(tail);
    out
}

pub fn mtf_encode(data: &[u8]) -> Vec<u8> {
    let mut table: Vec<u8> = (0..=255).collect();
    let mut pos: Vec<u8> = (0..=255).collect();
    let mut out = vec![0u8; data.len()];
    for (i, &b) in data.iter().enumerate() {
        let r = pos[b as usize];
        out[i] = r;
        if r != 0 {
            for j in (1..=r).rev() {
                let s = table[(j - 1) as usize];
                table[j as usize] = s;
                pos[s as usize] = j;
            }
            table[0] = b;
            pos[b as usize] = 0;
        }
    }
    out
}

pub fn mtf_decode(data: &[u8]) -> Vec<u8> {
    let mut table: Vec<u8> = (0..=255).collect();
    let mut out = vec![0u8; data.len()];
    for (i, &r) in data.iter().enumerate() {
        let b = table[r as usize];
        out[i] = b;
        if r != 0 {
            for j in (1..=r).rev() {
                table[j as usize] = table[(j - 1) as usize];
            }
            table[0] = b;
        }
    }
    out
}

pub fn rle0_encode(data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(data.len());
    let mut i = 0;
    while i < data.len() {
        if data[i] == 0 {
            let mut j = i;
            while j < data.len() && data[j] == 0 && (j - i) < 255 {
                j += 1;
            }
            out.push(0);
            out.push((j - i) as u8);
            i = j;
        } else {
            out.push(data[i]);
            i += 1;
        }
    }
    out
}

pub fn rle0_decode(data: &[u8]) -> Result<Vec<u8>, String> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < data.len() {
        let b = data[i];
        if b == 0 {
            if i + 1 >= data.len() {
                return Err("truncated RLE0".into());
            }
            let count = data[i + 1] as usize;
            out.extend(std::iter::repeat(0u8).take(count));
            i += 2;
        } else {
            out.push(b);
            i += 1;
        }
    }
    Ok(out)
}

fn sub_delta(data: &[u8], dist: usize) -> Vec<u8> {
    if dist < 1 || data.is_empty() {
        return data.to_vec();
    }
    let mut out = data.to_vec();
    for i in (dist..data.len()).rev() {
        out[i] = data[i].wrapping_sub(data[i - dist]);
    }
    out
}

fn sub_delta_inv(data: &[u8], dist: usize) -> Vec<u8> {
    if dist < 1 || data.is_empty() {
        return data.to_vec();
    }
    let mut out = data.to_vec();
    for i in dist..data.len() {
        out[i] = out[i].wrapping_add(out[i - dist]);
    }
    out
}

fn xor_delta(data: &[u8], dist: usize) -> Vec<u8> {
    if dist < 1 || data.is_empty() {
        return data.to_vec();
    }
    let mut out = data.to_vec();
    for i in (dist..data.len()).rev() {
        out[i] = data[i] ^ data[i - dist];
    }
    out
}

fn xor_delta_inv(data: &[u8], dist: usize) -> Vec<u8> {
    if dist < 1 || data.is_empty() {
        return data.to_vec();
    }
    let mut out = data.to_vec();
    for i in dist..data.len() {
        out[i] ^= out[i - dist];
    }
    out
}

fn apply_filter(data: &[u8], fid: u8) -> Result<Vec<u8>, String> {
    Ok(match fid {
        F_MTF => mtf_encode(data),
        F_RLE0 => rle0_encode(data),
        F_MTF_RLE0 => rle0_encode(&mtf_encode(data)),
        F_SUB1 => sub_delta(data, 1),
        F_SUB4 => sub_delta(data, 4),
        F_XOR1 => xor_delta(data, 1),
        _ => return Err(format!("unknown filter {fid}")),
    })
}

fn undo_filter(data: &[u8], fid: u8) -> Result<Vec<u8>, String> {
    Ok(match fid {
        F_MTF => mtf_decode(data),
        F_RLE0 => rle0_decode(data)?,
        F_MTF_RLE0 => mtf_decode(&rle0_decode(data)?),
        F_SUB1 => sub_delta_inv(data, 1),
        F_SUB4 => sub_delta_inv(data, 4),
        F_XOR1 => xor_delta_inv(data, 1),
        _ => return Err(format!("unknown filter {fid}")),
    })
}

fn apply_xform(data: &[u8], xid: u8) -> Result<Vec<u8>, String> {
    Ok(match xid {
        X_NONE => data.to_vec(),
        X_SUB1 => sub_delta(data, 1),
        X_SUB4 => sub_delta(data, 4),
        X_XOR1 => xor_delta(data, 1),
        _ => return Err(format!("unknown xform {xid}")),
    })
}

fn undo_xform(data: &[u8], xid: u8) -> Result<Vec<u8>, String> {
    Ok(match xid {
        X_NONE => data.to_vec(),
        X_SUB1 => sub_delta_inv(data, 1),
        X_SUB4 => sub_delta_inv(data, 4),
        X_XOR1 => xor_delta_inv(data, 1),
        _ => return Err(format!("unknown xform {xid}")),
    })
}

// --- BWT ---

pub fn bwt_encode(data: &[u8]) -> (Vec<u8>, u32) {
    let n = data.len();
    if n == 0 {
        return (Vec::new(), 0);
    }
    let mut s = Vec::with_capacity(n * 2);
    s.extend_from_slice(data);
    s.extend_from_slice(data);
    let mut sa: Vec<usize> = (0..n).collect();
    sa.sort_by(|&i, &j| s[i..i + n].cmp(&s[j..j + n]));
    let last: Vec<u8> = sa
        .iter()
        .map(|&i| data[(i + n - 1) % n])
        .collect();
    let primary = sa.iter().position(|&x| x == 0).unwrap() as u32;
    (last, primary)
}

pub fn bwt_decode(last: &[u8], primary: u32) -> Result<Vec<u8>, String> {
    let n = last.len();
    if n == 0 {
        return Ok(Vec::new());
    }
    if primary as usize >= n {
        return Err("BWT primary index out of range".into());
    }
    let mut order: Vec<usize> = (0..n).collect();
    order.sort_by(|&i, &j| last[i].cmp(&last[j]).then(i.cmp(&j)));
    let mut out = vec![0u8; n];
    let mut p = primary as usize;
    for i in 0..n {
        p = order[p];
        out[i] = last[p];
    }
    Ok(out)
}

// --- token ---

fn is_word_byte(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_'
}

fn token_split(data: &[u8]) -> Vec<Vec<u8>> {
    let mut tokens = Vec::new();
    let mut i = 0;
    while i < data.len() {
        if is_word_byte(data[i]) {
            let mut j = i + 1;
            while j < data.len() && is_word_byte(data[j]) {
                j += 1;
            }
            tokens.push(data[i..j].to_vec());
            i = j;
        } else {
            tokens.push(vec![data[i]]);
            i += 1;
        }
    }
    tokens
}

fn varint_encode(values: &[u32]) -> Vec<u8> {
    let mut out = Vec::new();
    for &orig in values {
        let mut v = orig;
        while v >= 0x80 {
            out.push(((v as u8) & 0x7f) | 0x80);
            v >>= 7;
        }
        out.push(v as u8);
    }
    out
}

fn varint_decode(data: &[u8]) -> Result<Vec<u32>, String> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < data.len() {
        let mut shift = 0u32;
        let mut val = 0u32;
        loop {
            if i >= data.len() {
                return Err("truncated varint".into());
            }
            let b = data[i];
            i += 1;
            val |= ((b & 0x7f) as u32) << shift;
            if b < 0x80 {
                break;
            }
            shift += 7;
            if shift > 70 {
                return Err("varint too long".into());
            }
        }
        out.push(val);
    }
    Ok(out)
}

fn token_encode_payload(data: &[u8]) -> Result<Vec<u8>, String> {
    let tokens = token_split(data);
    let mut counts: HashMap<Vec<u8>, usize> = HashMap::new();
    let mut first: HashMap<Vec<u8>, usize> = HashMap::new();
    for (i, t) in tokens.iter().enumerate() {
        *counts.entry(t.clone()).or_insert(0) += 1;
        first.entry(t.clone()).or_insert(i);
    }
    let mut uniq: Vec<Vec<u8>> = counts.keys().cloned().collect();
    uniq.sort_by(|a, b| {
        let ca = counts[a];
        let cb = counts[b];
        cb.cmp(&ca)
            .then(first[a].cmp(&first[b]))
    });
    let id_map: HashMap<Vec<u8>, u32> = uniq
        .iter()
        .enumerate()
        .map(|(i, t)| (t.clone(), i as u32))
        .collect();
    let ids: Vec<u32> = tokens.iter().map(|t| id_map[t]).collect();
    let mut dict_blob = Vec::new();
    for t in &uniq {
        if t.len() > 0xffff {
            return Err("token too long".into());
        }
        dict_blob.extend_from_slice(&(t.len() as u16).to_le_bytes());
        dict_blob.extend_from_slice(t);
    }
    let (d_mid, dcomp) = best_backend(&dict_blob);
    let (i_mid, icomp) = best_backend(&varint_encode(&ids));
    let mut out = vec![d_mid, i_mid];
    out.extend_from_slice(&(dcomp.len() as u32).to_le_bytes());
    out.extend_from_slice(&(icomp.len() as u32).to_le_bytes());
    out.extend_from_slice(&dcomp);
    out.extend_from_slice(&icomp);
    Ok(out)
}

fn token_decode_payload(payload: &[u8]) -> Result<Vec<u8>, String> {
    if payload.len() < 10 {
        return Err("token payload too short".into());
    }
    let d_mid = payload[0];
    let i_mid = payload[1];
    let dlen = u32::from_le_bytes(payload[2..6].try_into().unwrap()) as usize;
    let ilen = u32::from_le_bytes(payload[6..10].try_into().unwrap()) as usize;
    let pos = 10;
    if pos + dlen + ilen > payload.len() {
        return Err("token payload truncated".into());
    }
    let dict_blob = backend_dec(d_mid, &payload[pos..pos + dlen])?;
    let id_blob = backend_dec(i_mid, &payload[pos + dlen..pos + dlen + ilen])?;
    let mut uniq = Vec::new();
    let mut p = 0;
    while p < dict_blob.len() {
        if p + 2 > dict_blob.len() {
            return Err("bad token dict".into());
        }
        let ln = u16::from_le_bytes(dict_blob[p..p + 2].try_into().unwrap()) as usize;
        p += 2;
        if p + ln > dict_blob.len() {
            return Err("bad token dict entry".into());
        }
        uniq.push(dict_blob[p..p + ln].to_vec());
        p += ln;
    }
    let ids = varint_decode(&id_blob)?;
    let mut out = Vec::new();
    for tid in ids {
        let t = uniq
            .get(tid as usize)
            .ok_or_else(|| "token id out of range".to_string())?;
        out.extend_from_slice(t);
    }
    Ok(out)
}

// --- research candidates ---

fn research_transpose(data: &[u8]) -> Vec<(u8, Vec<u8>)> {
    [2usize, 3, 4, 5, 6, 8, 12, 16]
        .par_iter()
        .filter_map(|&width| {
            if data.len() < width * 4 {
                return None;
            }
            let transformed = transpose(data, width);
            let (mid, payload) = best_backend(&transformed);
            let mut wrapped = vec![width as u8, mid];
            wrapped.extend_from_slice(&payload);
            Some((M_TRANSPOSE, wrapped))
        })
        .collect()
}

fn research_xform(data: &[u8]) -> Vec<(u8, Vec<u8>)> {
    let widths = [2usize, 4, 8, 12, 16];
    let xids = [X_NONE, X_SUB1, X_SUB4, X_XOR1];
    widths
        .par_iter()
        .flat_map(|&width| {
            xids.par_iter().filter_map(move |&xid| {
                if data.len() < width * 4 {
                    return None;
                }
                let base = transpose(data, width);
                let transformed = apply_xform(&base, xid).ok()?;
                let (mid, payload) = best_backend(&transformed);
                let mut wrapped = vec![width as u8, xid, mid];
                wrapped.extend_from_slice(&payload);
                Some((M_XFORM, wrapped))
            })
        })
        .collect()
}

fn research_prefilter(data: &[u8]) -> Vec<(u8, Vec<u8>)> {
    if data.len() < 32 {
        return Vec::new();
    }
    let fids = [F_MTF, F_RLE0, F_MTF_RLE0, F_SUB1, F_SUB4, F_XOR1];
    fids.par_iter()
        .filter_map(|&fid| {
            let filtered = apply_filter(data, fid).ok()?;
            if filtered.len() > data.len() * 2 + 64 {
                return None;
            }
            let (mid, payload) = best_backend(&filtered);
            let mut wrapped = vec![fid, mid];
            wrapped.extend_from_slice(&payload);
            Some((M_PREFILTER, wrapped))
        })
        .collect()
}

fn research_bwt(data: &[u8]) -> Vec<(u8, Vec<u8>)> {
    let n = data.len();
    if n < 64 || n > BWT_MAX_BYTES {
        return Vec::new();
    }
    let (last, primary) = bwt_encode(data);
    let mtf = mtf_encode(&last);
    let variants: Vec<(u8, Vec<u8>)> = vec![
        (0, last.clone()),
        (BWT_F_MTF, mtf.clone()),
        (BWT_F_MTF | BWT_F_RLE0, rle0_encode(&mtf)),
        (BWT_F_MTF | BWT_F_SUB1, sub_delta(&mtf, 1)),
        (
            BWT_F_MTF | BWT_F_SUB1 | BWT_F_RLE0,
            rle0_encode(&sub_delta(&mtf, 1)),
        ),
    ];
    variants
        .par_iter()
        .map(|(flags, transformed)| {
            let (mid, payload) = best_backend(transformed);
            let mut wrapped = vec![*flags];
            wrapped.extend_from_slice(&primary.to_le_bytes());
            wrapped.push(mid);
            wrapped.extend_from_slice(&payload);
            (M_BWT, wrapped)
        })
        .collect()
}

fn research_blocks(data: &[u8]) -> Vec<(u8, Vec<u8>)> {
    let n = data.len();
    if n < 1024 {
        return Vec::new();
    }
    let sizes = [1024usize, 2048, 3072, 4096, 6144, 8192, 16384];
    sizes
        .par_iter()
        .filter_map(|&bs| {
            if n < bs {
                return None;
            }
            let mut out = Vec::new();
            out.extend_from_slice(&(bs as u16).to_le_bytes());
            let mut i = 0;
            while i < n {
                let end = (i + bs).min(n);
                let chunk = &data[i..end];
                let (mid, payload) = best_backend(chunk);
                if payload.len() > 0xffff_ffff {
                    return None;
                }
                out.push(mid);
                out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
                out.extend_from_slice(&payload);
                i = end;
            }
            Some((M_BLOCKS, out))
        })
        .collect()
}

fn research_parts(data: &[u8]) -> Vec<(u8, Vec<u8>)> {
    let n = data.len();
    if n < 1024 {
        return Vec::new();
    }
    [2usize, 3, 4]
        .par_iter()
        .map(|&nparts| {
            let mut out = Vec::new();
            out.extend_from_slice(&(nparts as u16).to_le_bytes());
            for i in 0..nparts {
                let a = i * n / nparts;
                let b = (i + 1) * n / nparts;
                let chunk = &data[a..b];
                let (mut mid, mut payload) = best_backend(chunk);
                for w in [4usize, 8, 12] {
                    if chunk.len() < w * 4 {
                        continue;
                    }
                    let base = transpose(chunk, w);
                    for xid in [X_NONE, X_SUB1, X_SUB4] {
                        if let Ok(tr) = apply_xform(&base, xid) {
                            let (m2, p2) = best_backend(&tr);
                            let mut xp = vec![w as u8, xid, m2];
                            xp.extend_from_slice(&p2);
                            if xp.len() < payload.len() {
                                mid = M_XFORM;
                                payload = xp;
                            }
                        }
                    }
                }
                for fid in [F_SUB1, F_SUB4] {
                    if let Ok(f) = apply_filter(chunk, fid) {
                        let (m2, p2) = best_backend(&f);
                        let mut pref = vec![fid, m2];
                        pref.extend_from_slice(&p2);
                        if pref.len() < payload.len() {
                            mid = M_PREFILTER;
                            payload = pref;
                        }
                    }
                }
                out.push(mid);
                out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
                out.extend_from_slice(&payload);
            }
            (M_PARTS, out)
        })
        .collect()
}

fn research_token(data: &[u8]) -> Vec<(u8, Vec<u8>)> {
    if data.len() < 64 {
        return Vec::new();
    }
    let printable = data
        .iter()
        .filter(|&&b| (32..127).contains(&b) || matches!(b, 9 | 10 | 13))
        .count();
    if printable < data.len() * 2 / 5 {
        return Vec::new();
    }
    match token_encode_payload(data) {
        Ok(p) => vec![(M_TOKEN, p)],
        Err(_) => Vec::new(),
    }
}

fn pick_best(mut candidates: Vec<(u8, Vec<u8>)>) -> (u8, Vec<u8>) {
    if candidates.is_empty() {
        return (M_STORE, Vec::new());
    }
    candidates.sort_by(|a, b| a.1.len().cmp(&b.1.len()).then(a.0.cmp(&b.0)));
    candidates.into_iter().next().unwrap()
}

/// Compress `data` into a BAV frame (v2 when size fits in u32).
pub fn compress(data: &[u8], method: Method) -> Result<Vec<u8>, String> {
    let crc = crc32(data);
    let orig = data.len() as u64;

    let candidates: Vec<(u8, Vec<u8>)> = match method {
        Method::Store => vec![(M_STORE, store_enc(data))],
        Method::Deflate => vec![(M_DEFLATE, deflate_enc(data)?)],
        Method::Lzma => vec![(M_LZMA, lzma_enc(data)?)],
        Method::Zstd => vec![(M_ZSTD, zstd_enc(data)?)],
        Method::Brotli => vec![(M_BROTLI, brotli_enc(data)?)],
        Method::Auto | Method::Research => {
            // Independent candidate families in parallel; each family also uses rayon.
            let ((f0, f1), (f2, f3)) = rayon::join(
                || rayon::join(|| try_backends(data), || research_transpose(data)),
                || rayon::join(|| research_xform(data), || research_prefilter(data)),
            );
            let ((f4, f5), (f6, f7)) = rayon::join(
                || rayon::join(|| research_bwt(data), || research_blocks(data)),
                || rayon::join(|| research_parts(data), || research_token(data)),
            );
            let mut all = Vec::new();
            all.extend(f0);
            all.extend(f1);
            all.extend(f2);
            all.extend(f3);
            all.extend(f4);
            all.extend(f5);
            all.extend(f6);
            all.extend(f7);
            all
        }
    };

    let mut candidates = candidates;
    if candidates.is_empty() {
        candidates.push((M_STORE, store_enc(data)));
    }
    let (mid, payload) = pick_best(candidates);

    let mut frame = if orig > u32::MAX as u64 {
        pack_header_v1(mid, orig, crc).to_vec()
    } else {
        pack_header_v2(mid, orig as u32, crc).to_vec()
    };
    frame.extend_from_slice(&payload);
    Ok(frame)
}

fn decode_method_payload(mid: u8, payload: &[u8]) -> Result<Vec<u8>, String> {
    match mid {
        M_TRANSPOSE => {
            if payload.len() < 2 {
                return Err("transpose payload too short".into());
            }
            let width = payload[0] as usize;
            let backend = payload[1];
            let transformed = backend_dec(backend, &payload[2..])?;
            Ok(untranspose(&transformed, width))
        }
        M_XFORM => {
            if payload.len() < 3 {
                return Err("xform payload too short".into());
            }
            let width = payload[0] as usize;
            let xid = payload[1];
            let backend = payload[2];
            let transformed = backend_dec(backend, &payload[3..])?;
            let unx = undo_xform(&transformed, xid)?;
            Ok(untranspose(&unx, width))
        }
        M_PREFILTER => {
            if payload.len() < 2 {
                return Err("prefilter payload too short".into());
            }
            let fid = payload[0];
            let backend = payload[1];
            let filtered = backend_dec(backend, &payload[2..])?;
            undo_filter(&filtered, fid)
        }
        M_BWT => {
            if payload.len() < 6 {
                return Err("BWT payload too short".into());
            }
            let flags = payload[0];
            let primary = u32::from_le_bytes(payload[1..5].try_into().unwrap());
            let backend = payload[5];
            let mut transformed = backend_dec(backend, &payload[6..])?;
            if flags & BWT_F_RLE0 != 0 {
                transformed = rle0_decode(&transformed)?;
            }
            if flags & BWT_F_SUB1 != 0 {
                transformed = sub_delta_inv(&transformed, 1);
            }
            if flags & BWT_F_MTF != 0 {
                transformed = mtf_decode(&transformed);
            }
            bwt_decode(&transformed, primary)
        }
        M_TOKEN => token_decode_payload(payload),
        M_BLOCKS => {
            if payload.len() < 2 {
                return Err("blocks payload too short".into());
            }
            let bs = u16::from_le_bytes(payload[0..2].try_into().unwrap());
            if bs == 0 {
                return Err("invalid block size".into());
            }
            let mut pos = 2usize;
            let mut chunks = Vec::new();
            while pos < payload.len() {
                if pos + 5 > payload.len() {
                    return Err("truncated block header".into());
                }
                let bmid = payload[pos];
                let plen =
                    u32::from_le_bytes(payload[pos + 1..pos + 5].try_into().unwrap()) as usize;
                pos += 5;
                if pos + plen > payload.len() {
                    return Err("truncated block payload".into());
                }
                chunks.push(decode_method_payload(bmid, &payload[pos..pos + plen])?);
                pos += plen;
            }
            Ok(chunks.into_iter().flatten().collect())
        }
        M_PARTS => {
            if payload.len() < 2 {
                return Err("parts payload too short".into());
            }
            let nparts = u16::from_le_bytes(payload[0..2].try_into().unwrap()) as usize;
            if nparts == 0 {
                return Err("invalid nparts".into());
            }
            let mut pos = 2usize;
            let mut chunks = Vec::new();
            for _ in 0..nparts {
                if pos + 5 > payload.len() {
                    return Err("truncated part header".into());
                }
                let pmid = payload[pos];
                let plen =
                    u32::from_le_bytes(payload[pos + 1..pos + 5].try_into().unwrap()) as usize;
                pos += 5;
                if pos + plen > payload.len() {
                    return Err("truncated part payload".into());
                }
                chunks.push(decode_method_payload(pmid, &payload[pos..pos + plen])?);
                pos += plen;
            }
            Ok(chunks.into_iter().flatten().collect())
        }
        M_STORE | M_DEFLATE | M_LZMA | M_ZSTD | M_BROTLI => backend_dec(mid, payload),
        _ => Err(format!("unknown method id {mid}")),
    }
}

/// Decompress a BAV v1 or v2 frame.
pub fn decompress(frame: &[u8]) -> Result<Vec<u8>, String> {
    if frame.len() < 8 {
        return Err("BAV frame too short".into());
    }
    if &frame[0..4] != MAGIC {
        return Err(format!("bad magic: {:?}", &frame[0..4]));
    }
    let ver = frame[4];
    let (mid, orig, crc, payload) = if ver == FORMAT_VERSION {
        if frame.len() < HEADER_V2_SIZE {
            return Err("BAV v2 frame too short".into());
        }
        let mid = frame[5];
        let orig = u32::from_le_bytes(frame[7..11].try_into().unwrap()) as u64;
        let crc = u32::from_le_bytes(frame[11..15].try_into().unwrap());
        (mid, orig, crc, &frame[HEADER_V2_SIZE..])
    } else if ver == FORMAT_VERSION_LEGACY {
        if frame.len() < HEADER_V1_SIZE {
            return Err("BAV v1 frame too short".into());
        }
        let mid = frame[5];
        let orig = u64::from_le_bytes(frame[7..15].try_into().unwrap());
        let crc = u32::from_le_bytes(frame[15..19].try_into().unwrap());
        (mid, orig, crc, &frame[HEADER_V1_SIZE..])
    } else {
        return Err(format!("unsupported BAV version: {ver}"));
    };

    let data = decode_method_payload(mid, payload)?;
    if data.len() as u64 != orig {
        return Err(format!(
            "size mismatch: got {}, expected {orig}",
            data.len()
        ));
    }
    if crc32(&data) != crc {
        return Err("CRC32 mismatch (corrupt or non-lossless frame)".into());
    }
    Ok(data)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;

    fn corpus_dir() -> PathBuf {
        PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("..")
            .join("..")
            .join("corpus")
    }

    #[test]
    fn roundtrip_basic() {
        for s in [
            &b""[..],
            b"a",
            b"hello world",
            b"aaaaaaaaaaaaaaaa",
            &[0u8; 64],
        ] {
            for m in [Method::Store, Method::Deflate, Method::Auto] {
                let frame = compress(s, m).unwrap();
                assert_eq!(&frame[0..4], MAGIC);
                assert_eq!(decompress(&frame).unwrap(), s);
            }
        }
    }

    #[test]
    fn transforms() {
        let data: Vec<u8> = (0..200u8).cycle().take(512).collect();
        assert_eq!(mtf_decode(&mtf_encode(&data)), data);
        assert_eq!(rle0_decode(&rle0_encode(&data)).unwrap(), data);
        assert_eq!(untranspose(&transpose(&data, 4), 4), data);
        let (last, p) = bwt_encode(&data);
        assert_eq!(bwt_decode(&last, p).unwrap(), data);
        assert_eq!(sub_delta_inv(&sub_delta(&data, 1), 1), data);
    }

    #[test]
    fn corpus_roundtrip_auto() {
        let dir = corpus_dir();
        if !dir.is_dir() {
            return;
        }
        for name in [
            "01_plain_text.txt",
            "02_html_js.html",
            "03_binary_records.bin",
            "04_mixed_archive.bin",
            "05_source_code.py",
            "edge_empty.bin",
            "edge_small.txt",
            "edge_tiny.txt",
        ] {
            let data = fs::read(dir.join(name)).unwrap();
            let frame = compress(&data, Method::Auto).unwrap();
            assert_eq!(decompress(&frame).unwrap(), data, "{name}");
        }
    }
}
