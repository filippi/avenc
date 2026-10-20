import { decompressSync, strFromU8 } from "https://cdn.jsdelivr.net/npm/fflate@0.8.2/esm/browser.js";

const AVENC_PREFIXES = ["AVENCv2|", "AVENCv1|"];
const DEFAULT_SCAN_BYTES = 8 * 1024 * 1024;
const DEFAULT_RANGE_CHUNK = 512 * 1024;
const DEFAULT_MAX_PAYLOAD_CHARS = 16 * 1024 * 1024;
const DEFAULT_MP4BOX_PROBE_TIMEOUT_MS = 600;

function bytesToText(bytes) {
  try {
    return strFromU8(bytes);
  } catch {
    return new TextDecoder("utf-8", { fatal: false }).decode(bytes);
  }
}

function b64ToBytes(base64) {
  const normalized = String(base64 || "").replace(/[\n\r\t ]+/g, "");
  const binary = atob(normalized);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

function inflateMaybe(bytes) {
  try {
    return decompressSync(bytes);
  } catch {
    return bytes;
  }
}

function isBase64Byte(code) {
  return (
    (code >= 65 && code <= 90) ||
    (code >= 97 && code <= 122) ||
    (code >= 48 && code <= 57) ||
    code === 43 || code === 47 || code === 61 || code === 45 || code === 95 || code === 46
  );
}

function readU32LE(bytes, offset) {
  if (offset < 0 || offset + 4 > bytes.length) {
    throw new Error(`readU32LE out of range at offset ${offset}`);
  }
  return (
    (bytes[offset]) |
    (bytes[offset + 1] << 8) |
    (bytes[offset + 2] << 16) |
    (bytes[offset + 3] << 24)
  ) >>> 0;
}

function readU64LE(bytes, offset) {
  const lo = readU32LE(bytes, offset);
  const hi = readU32LE(bytes, offset + 4);
  return (BigInt(hi) << 32n) | BigInt(lo);
}

function readI32LE(bytes, offset) {
  const u = readU32LE(bytes, offset);
  return (u & 0x80000000) ? (u - 0x100000000) : u;
}

function readF64LE(bytes, offset) {
  if (offset < 0 || offset + 8 > bytes.length) {
    throw new Error(`readF64LE out of range at offset ${offset}`);
  }
  const dv = new DataView(bytes.buffer, bytes.byteOffset + offset, 8);
  return dv.getFloat64(0, true);
}

function findBytes(haystack, needle) {
  if (!needle.length || haystack.length < needle.length) return -1;
  outer: for (let i = 0; i <= haystack.length - needle.length; i += 1) {
    for (let j = 0; j < needle.length; j += 1) {
      if (haystack[i + j] !== needle[j]) continue outer;
    }
    return i;
  }
  return -1;
}

async function fetchHeadMeta(url, signal) {
  const r = await fetch(url, { method: "HEAD", cache: "no-cache", signal });
  const len = Number(r.headers.get("content-length") || 0);
  return {
    ok: r.ok,
    size: Number.isFinite(len) && len > 0 ? len : null
  };
}

// One retained range per inspection: reuse the marker scan for its payload.
// Refuse servers that ignore Range before buffering a potentially huge MP4.
function createRangeReader(signal) {
  let cached = null;
  let bytesRead = 0;
  const read = async (url, start, end, allowPartial = false) => {
    if (cached && start >= cached.responseStart && start <= cached.end && (end <= cached.end || allowPartial)) {
      const last = Math.min(end, cached.end);
      return {
        bytes: cached.bytes.subarray(start - cached.responseStart, last - cached.responseStart + 1),
        responseStart: start,
        isFull: false
      };
    }
    const r = await fetch(url, {
      headers: { Range: `bytes=${start}-${end}` }, cache: "no-cache", signal
    });
    const match = (r.headers.get("content-range") || "").match(/^bytes (\d+)-(\d+)\/(\d+|\*)$/i);
    const length = Number(r.headers.get("content-length"));
    const boundedFull = r.status === 200 && start === 0 && length > 0 && length <= end + 1;
    if (!(r.status === 206 && match && Number(match[1]) === start && Number(match[2]) <= end) && !boundedFull) {
      await r.body?.cancel();
      throw new Error(`Server must support HTTP byte ranges for metadata inspection (HTTP ${r.status})`);
    }
    // Stream into a fixed-size buffer, including when Content-Length is incorrect.
    const buffer = new Uint8Array(end - start + 1);
    const reader = r.body.getReader();
    let count = 0;
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (count + value.length > buffer.length) throw new Error("Range response exceeds requested size");
        buffer.set(value, count);
        count += value.length;
      }
    } finally {
      await reader.cancel();
    }
    if (match && count !== Number(match[2]) - start + 1) throw new Error("Truncated range response");
    bytesRead += count;
    cached = { bytes: buffer.subarray(0, count), responseStart: start, end: start + count - 1 };
    return { ...cached, isFull: boundedFull };
  };
  read.bytesRead = () => bytesRead;
  return read;
}

function codecFamily(codec) {
  const c = String(codec || "").toLowerCase();
  if (c.startsWith("av01") || c.startsWith("av1")) return "av1";
  if (c.startsWith("vp09") || c.startsWith("vp9")) return "vp9";
  if (c.startsWith("hvc1") || c.startsWith("hev1") || c.startsWith("hevc") || c.startsWith("h265")) return "hevc";
  if (c.startsWith("avc1") || c.startsWith("avc3") || c.startsWith("avc") || c.startsWith("h264")) return "avc";
  return c || "unknown";
}

async function probeMp4VideoTracks(url, fileSize, scanBytes, fetchRange) {
  if (!(window.MP4Box && window.MP4Box.createFile)) return [];
  try {
    const probeEnd = fileSize ? Math.min(fileSize - 1, Math.max(0, scanBytes - 1)) : Math.max(0, scanBytes - 1);
    const headChunk = await fetchRange(url, 0, probeEnd);
    const src = headChunk.bytes;
    if (!src || src.length === 0) return [];

    const localBuffer = src.buffer.slice(src.byteOffset, src.byteOffset + src.byteLength);
    return await new Promise((resolve) => {
      let settled = false;
      const done = (tracks) => {
        if (settled) return;
        settled = true;
        resolve(Array.isArray(tracks) ? tracks : []);
      };

      try {
        const file = window.MP4Box.createFile();
        file.onError = () => done([]);
        file.onReady = (info) => {
          const tracks = (info?.videoTracks || [])
            .slice()
            .sort((a, b) => Number(a.id) - Number(b.id))
            .map((t, idx) => ({
              stream_index: idx,
              track_id: Number(t.id ?? idx),
              codec: String(t.codec || ""),
              codec_family: codecFamily(t.codec),
              width: Number(t?.video?.width || 0),
              height: Number(t?.video?.height || 0)
            }));
          done(tracks);
        };
        localBuffer.fileStart = 0;
        file.appendBuffer(localBuffer);
        file.flush();
      } catch {
        done([]);
      }

      setTimeout(() => done([]), DEFAULT_MP4BOX_PROBE_TIMEOUT_MS);
    });
  } catch {
    return [];
  }
}

async function locateMarker(url, fileSize, scanBytes, fetchRange) {
  const enc = new TextEncoder();
  const markers = AVENC_PREFIXES.map((p) => ({ prefix: p, bytes: enc.encode(p) }));

  const windows = [];
  if (!fileSize || fileSize <= scanBytes) {
    windows.push({ start: 0, end: Math.max(0, (fileSize || scanBytes) - 1) });
  } else {
    windows.push({ start: 0, end: scanBytes - 1 });
    windows.push({ start: fileSize - scanBytes, end: fileSize - 1 });
  }

  for (const w of windows) {
    const chunkResp = await fetchRange(url, w.start, w.end);
    const chunk = chunkResp.bytes;
    for (const m of markers) {
      let idx = findBytes(chunk, m.bytes);
      while (idx >= 0) {
        // If the next char is available in this fetched buffer, require it to look like base64.
        const nextPos = idx + m.bytes.length;
        if (nextPos < chunk.length && !isBase64Byte(chunk[nextPos])) {
          const sub = chunk.subarray(nextPos);
          const nextIdxRel = findBytes(sub, m.bytes);
          idx = nextIdxRel >= 0 ? (nextPos + nextIdxRel) : -1;
          continue;
        }
        const absoluteMarkerStart = chunkResp.isFull ? idx : (chunkResp.responseStart + idx);
        return {
          prefix: m.prefix,
          absoluteMarkerStart,
          markerLength: m.bytes.length,
          fileSize
        };
      }
    }
  }
  throw new Error("Unable to find AVENC metadata marker in scanned MP4 ranges");
}

async function readBase64PayloadFrom(url, markerInfo, chunkSize, maxChars, fetchRange) {
  const parts = [];
  let partBuf = [];
  let count = 0;
  let pos = markerInfo.absoluteMarkerStart + markerInfo.markerLength;
  const size = markerInfo.fileSize;

  const flush = () => {
    if (!partBuf.length) return;
    parts.push(String.fromCharCode(...partBuf));
    partBuf = [];
  };

  while (!size || pos < size) {
    const end = size ? Math.min(size - 1, pos + chunkSize - 1) : (pos + chunkSize - 1);
    const chunkResp = await fetchRange(url, pos, end, true);
    const chunk = chunkResp.bytes;
    if (!chunk.length) break;

    // Fallback for servers that ignore Range and return whole file with HTTP 200.
    let startIdx = 0;
    if (chunkResp.isFull && pos > 0) {
      if (pos >= chunk.length) break;
      startIdx = pos;
    }

    let stop = false;
    for (let i = startIdx; i < chunk.length; i += 1) {
      const c = chunk[i];
      if (isBase64Byte(c)) {
        partBuf.push(c);
        count += 1;
        if (partBuf.length >= 8192) flush();
        if (count > maxChars) {
          throw new Error(`AVENC metadata payload too large (> ${maxChars} chars)`);
        }
      } else {
        stop = true;
        break;
      }
    }

    if (stop) break;
    if (chunkResp.isFull) break;
    pos = chunkResp.responseStart + chunk.length;
  }

  flush();
  const payloadB64 = parts.join("");
  if (!payloadB64) throw new Error("Found AVENC marker but empty payload");
  return payloadB64;
}

function parseV2BinarySummary(payloadB64, fileSize) {
  const compressed = b64ToBytes(payloadB64);
  const packed = inflateMaybe(compressed);
  if (packed.length < 36) throw new Error("AVENCv2 payload too short");
  const expect = [65, 86, 50, 77, 69, 84, 65, 0]; // AV2META\0
  for (let i = 0; i < expect.length; i += 1) {
    if (packed[i] !== expect[i]) throw new Error("Invalid AVENCv2 metadata magic");
  }
  const fmtVersion = readU32LE(packed, 8);
  if (fmtVersion !== 2) throw new Error(`Unsupported AVENCv2 fmt version: ${fmtVersion}`);

  let off = 12;
  const _flags = readU32LE(packed, off); off += 4;
  const nx = readU32LE(packed, off); off += 4;
  const ny = readU32LE(packed, off); off += 4;
  const streamCount = readU32LE(packed, off); off += 4;
  const recordCount = readU32LE(packed, off); off += 4;
  const _reserved = readU32LE(packed, off); off += 4;

  const readU32 = () => {
    const v = readU32LE(packed, off);
    off += 4;
    return v;
  };
  const readI32 = () => {
    const v = readI32LE(packed, off);
    off += 4;
    return v;
  };
  const readU64 = () => {
    const v = readU64LE(packed, off);
    off += 8;
    return v;
  };
  const readF64 = () => {
    const v = readF64LE(packed, off);
    off += 8;
    return v;
  };
  const skipStr = () => {
    const n = readU32();
    if (off + n > packed.length) throw new Error("Truncated AVENCv2 string");
    off += n;
    return n;
  };
  const readStr = () => {
    const n = readU32();
    if (off + n > packed.length) throw new Error("Truncated AVENCv2 string");
    const s = bytesToText(packed.subarray(off, off + n));
    off += n;
    return s;
  };

  const streams = [];
  const byStream = new Map();
  for (let i = 0; i < streamCount; i += 1) {
    const s = {
      stream_index: readI32(),
      frame_count: readU32(),
      bits: readU32(),
      storage_shift: readI32(),
      min_value: readF64(),
      max_value: readF64(),
      apply_log2: readU32() !== 0,
      log_offset: readF64(),
      encoded_width: readU32(),
      encoded_height: readU32(),
      crop_width: readU32(),
      crop_height: readU32(),
      records_seen: 0,
      short_name: "",
      type_of_level: "",
      sample_level: "",
      sample_date: "",
      sample_time: "",
      sample_step: ""
    };
    skipStr(); // bitmap_payload
    skipStr(); // template_b64
    streams.push(s);
    byStream.set(s.stream_index, s);
  }

  for (let i = 0; i < recordCount; i += 1) {
    const _order = readU64();
    const streamIndex = readI32();
    const _frameIndex = readU32();
    const _fileIndex = readI32();
    const _msgIndex = readU64();
    const _fmin = readF64();
    const _fmax = readF64();
    const _bitmapPresent = readU32();
    const _bitsPerValue = readI32();
    const _bshift = readI32();

    const target = byStream.get(streamIndex);
    if (target) target.records_seen += 1;

    if (target && !target.short_name) target.short_name = readStr();
    else skipStr();
    if (target && !target.type_of_level) target.type_of_level = readStr();
    else skipStr();
    if (target && !target.sample_level) target.sample_level = readStr();
    else skipStr();
    if (target && !target.sample_date) target.sample_date = readStr();
    else skipStr();
    if (target && !target.sample_time) target.sample_time = readStr();
    else skipStr();
    if (target && !target.sample_step) target.sample_step = readStr();
    else skipStr();

    skipStr(); // timestamp
    skipStr(); // ensemble_number
    skipStr(); // perturbation_number
    skipStr(); // template_b64
  }

  let totalFrames = 0;
  let raw32EquivalentBytes = 0;
  for (const s of streams) {
    totalFrames += Number(s.frame_count || 0);
    const w = Number(s.crop_width || nx || 0);
    const h = Number(s.crop_height || ny || 0);
    raw32EquivalentBytes += Number(s.frame_count || 0) * w * h * 4;
  }

  return {
    ok: true,
    schema: "avenc.pack.v2.binary",
    metadata_version: "AVENCv2(fmt=2)",
    grid: { nx, ny },
    streams: streamCount,
    records: recordCount,
    total_frames: totalFrames,
    metadata_header_compressed_bytes: compressed.length,
    metadata_header_uncompressed_bytes: packed.length,
    metadata_b64_chars: payloadB64.length,
    file_size_bytes: fileSize || null,
    raw32_equivalent_total_bytes: raw32EquivalentBytes,
    compression_ratio_raw32_to_mp4: (fileSize && fileSize > 0) ? (raw32EquivalentBytes / fileSize) : null,
    stream_summaries: streams
  };
}

function parseV1Summary(payloadB64, fileSize) {
  const compressed = b64ToBytes(payloadB64);
  const raw = inflateMaybe(compressed);
  const text = bytesToText(raw);
  const findInt = (re, def = null) => {
    const m = text.match(re);
    return m ? Number(m[1]) : def;
  };
  const nx = findInt(/"nx"\s*:\s*(\d+)/, null);
  const ny = findInt(/"ny"\s*:\s*(\d+)/, null);
  const streams = findInt(/"stream_count"\s*:\s*(\d+)/, null);
  const records = findInt(/"record_count"\s*:\s*(\d+)/, null);
  return {
    ok: true,
    schema: "avenc.pack.v1.json",
    metadata_version: "AVENCv1(json)",
    grid: { nx, ny },
    streams,
    records,
    metadata_header_compressed_bytes: compressed.length,
    metadata_header_uncompressed_bytes: raw.length,
    metadata_b64_chars: payloadB64.length,
    file_size_bytes: fileSize || null,
    note: "V1 summary is minimal to keep metadata scan lightweight"
  };
}

function summarizeError(err) {
  const msg = String(err?.message || err || "Unknown error");
  return { ok: false, error: msg };
}

export function formatBytes(bytes) {
  const n = Number(bytes || 0);
  if (!Number.isFinite(n) || n <= 0) return "0 B";
  const units = ["B", "KB", "MB", "GB", "TB"];
  let v = n;
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i += 1;
  }
  return `${v.toFixed(v >= 100 ? 0 : v >= 10 ? 1 : 2)} ${units[i]}`;
}

export async function inspectAvencDataset(url, options = {}) {
  const t0 = performance.now();
  const scanBytes = Number(options.scanBytes || DEFAULT_SCAN_BYTES);
  const rangeChunk = Number(options.rangeChunk || DEFAULT_RANGE_CHUNK);
  const maxPayloadChars = Number(options.maxPayloadChars || DEFAULT_MAX_PAYLOAD_CHARS);
  try {
    const fast = options.fast !== false;
    const fetchRange = createRangeReader(options.signal);
    const head = await fetchHeadMeta(url, options.signal);
    let markerInfo;
    try {
      markerInfo = await locateMarker(url, head.size, fast ? Math.min(scanBytes, 64 * 1024) : scanBytes, fetchRange);
    } catch (err) {
      if (!fast || !String(err.message).startsWith("Unable to find AVENC")) throw err;
      markerInfo = await locateMarker(url, head.size, scanBytes, fetchRange);
    }
    const payloadB64 = await readBase64PayloadFrom(url, markerInfo, rangeChunk, maxPayloadChars, fetchRange);
    const containerTracks = fast ? [] : await probeMp4VideoTracks(url, head.size, scanBytes, fetchRange);
    let summary;
    if (markerInfo.prefix === "AVENCv2|") {
      summary = parseV2BinarySummary(payloadB64, head.size);
    } else if (markerInfo.prefix === "AVENCv1|") {
      summary = parseV1Summary(payloadB64, head.size);
    } else {
      throw new Error(`Unsupported AVENC marker: ${markerInfo.prefix}`);
    }
    summary.url = url;
    summary.marker_prefix = markerInfo.prefix;
    summary.scan_mode = fast ? "fast-range-head-tail" : "range-head-tail";
    summary.bytes_fetched = fetchRange.bytesRead();
    summary.codec_probe = fast ? "Skipped in fast mode; use full inspection for codec details" : "enabled";
    if (containerTracks.length > 0) {
      summary.container_video_tracks = containerTracks;
      summary.codecs = [...new Set(containerTracks.map((t) => t.codec).filter(Boolean))];
      summary.codec_families = [...new Set(containerTracks.map((t) => t.codec_family).filter(Boolean))];
      if (Array.isArray(summary.stream_summaries)) {
        const byStreamIndex = new Map(containerTracks.map((t) => [Number(t.stream_index), t]));
        summary.stream_summaries = summary.stream_summaries.map((s) => {
          const t = byStreamIndex.get(Number(s.stream_index));
          if (!t) return s;
          return { ...s, codec: t.codec, codec_family: t.codec_family };
        });
      }
    }
    if (Array.isArray(summary.stream_summaries)) {
      summary.bit_depths = [...new Set(summary.stream_summaries
        .map((s) => Number(s.bits))
        .filter((b) => Number.isFinite(b) && b > 0))].sort((a, b) => a - b);
    }
    summary.parse_time_ms = Number((performance.now() - t0).toFixed(3));
    return summary;
  } catch (err) {
    if (options.signal?.aborted) throw err;
    const out = summarizeError(err);
    out.url = url;
    out.parse_time_ms = Number((performance.now() - t0).toFixed(3));
    return out;
  }
}
