// Run: node www/tests/metadata.mjs [optional MP4 fixtures...]
// Substitute Node's zlib for the browser CDN dependency; exercise the real reader/parser.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { deflateSync } from 'node:zlib';
const source = (await readFile(new URL('../js/avenc_metadata.js', import.meta.url), 'utf8'))
  .replace(/^import .*?;\n/, 'import { unzipSync as decompressSync } from "node:zlib"; const strFromU8 = b => new TextDecoder().decode(b);\n');
const { inspectAvencDataset } = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);
globalThis.window = {}; // Full mode can operate without the optional MP4Box probe.
let gets;
function serve(bytes, { ignoreRange = false, unknownSize = false } = {}) {
  gets = [];
  globalThis.fetch = async (_url, options) => {
    if (options.signal?.aborted) throw new DOMException('Aborted', 'AbortError');
    if (options.method === 'HEAD') return new Response(null, { headers: unknownSize ? {} : { 'content-length': bytes.length } });
    const [, startText, endText] = options.headers.Range.match(/bytes=(\d+)-(\d+)/);
    const start = Number(startText), end = Math.min(Number(endText), bytes.length - 1);
    gets.push([start, end]);
    if (ignoreRange) return new Response(bytes, { headers: { 'content-length': bytes.length } });
    return new Response(bytes.subarray(start, end + 1), { status: 206, headers: { 'content-range': `bytes ${start}-${end}/${bytes.length}` } });
  };
}
const payload = Buffer.from('AVENCv1|' + deflateSync(Buffer.from(JSON.stringify({ nx: 32, ny: 64, stream_count: 2, record_count: 8 }))).toString('base64') + '\0');
const front = Buffer.alloc(2 * 1024 * 1024);
payload.copy(front, 32);
serve(front);
let result = await inspectAvencDataset('fixture');
assert.equal(result.ok, true);
assert.equal(result.records, 8);
assert.equal(result.bytes_fetched, 65536);
assert.equal(gets.length, 1, 'payload must reuse the initial range');
const tail = Buffer.alloc(front.length);
payload.copy(tail, tail.length - payload.length - 100);
serve(tail);
assert.equal((await inspectAvencDataset('tail')).ok, true);
assert.equal(gets.length, 2);
const middle = Buffer.alloc(front.length);
payload.copy(middle, 200000);
serve(middle);
assert.equal((await inspectAvencDataset('fallback')).ok, true, 'larger fallback scan must extend a cached short range');
serve(front, { ignoreRange: true });
assert.match((await inspectAvencDataset('ignored')).error, /byte ranges/);
serve(payload, { ignoreRange: true });
assert.equal((await inspectAvencDataset('small-full')).ok, true);
serve(front, { unknownSize: true });
assert.equal((await inspectAvencDataset('no-head-size')).ok, true);
serve(front);
const controller = new AbortController(); controller.abort();
await assert.rejects(inspectAvencDataset('abort', { signal: controller.signal }), { name: 'AbortError' });
serve(front);
assert.match((await inspectAvencDataset('limit', { maxPayloadChars: 8 })).error, /too large/);
// A payload crossing the first cached range must resume at the actual consumed offset.
const longJson = JSON.stringify({ nx: 32, ny: 64, record_count: 3, padding: Array.from({ length: 30000 }, (_, i) => Math.sin(i).toString()).join('') });
const longPayload = Buffer.from('AVENCv1|' + deflateSync(Buffer.from(longJson)).toString('base64') + '\0');
const crossing = Buffer.concat([Buffer.alloc(65000), longPayload, Buffer.alloc(16)]);
serve(crossing);
assert.equal((await inspectAvencDataset('crossing')).records, 3);
assert.equal(gets[1][0], 65536);
for (const fixture of process.argv.slice(2)) {
  const bytes = await readFile(fixture);
  serve(bytes);
  const fast = await inspectAvencDataset(fixture);
  serve(bytes);
  const full = await inspectAvencDataset(fixture, { fast: false });
  assert.equal(fast.ok, true, JSON.stringify(fast));
  assert.deepEqual(fast.stream_summaries, full.stream_summaries);
  assert.deepEqual(fast.grid, full.grid);
  assert.equal(fast.records, full.records);
  console.log(`${fixture}: ${fast.marker_prefix} ${fast.records} records; fast ${fast.bytes_fetched} bytes, full ${full.bytes_fetched} bytes`);
}
console.log('Metadata range, fallback, cancellation, limit and fixture checks passed.');
