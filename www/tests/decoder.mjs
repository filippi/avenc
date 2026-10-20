// node www/tests/decoder.mjs [path/to/mp4box.all.js path/to/fixture.mp4]
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
const source = (await readFile(new URL('../js/avenc_model.js', import.meta.url), 'utf8'))
  .replace(/^import .*?;\n/, 'import { unzipSync as decompressSync } from "node:zlib"; const strFromU8 = b => new TextDecoder().decode(b);\n');
const { AvencModel } = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);
let live = 0, peak = 0, opened = 0, closed = 0, inputCount = 0, maxQueue = 0;
let injectError = false, omitOutput = false, earlyDequeue = false;
let outstanding = 0, peakOutstanding = 0;
class FakeFrame {
  constructor(timestamp) { this.timestamp = timestamp; this.duration = 1000; this.codedWidth = 64; this.codedHeight = 64; this.closed = false; live++; peak = Math.max(peak, live); }
  clone() { assert.equal(this.closed, false); return new FakeFrame(this.timestamp); }
  close() { if (!this.closed) { this.closed = true; live--; } }
}
class FakeDecoder extends EventTarget {
  constructor(callbacks) { super(); this.callbacks = callbacks; this.state = 'unconfigured'; this.decodeQueueSize = 0; this.pending = []; opened++; }
  configure() { this.state = 'configured'; }
  decode(chunk) {
    assert.equal(this.state, 'configured');
    inputCount++; this.decodeQueueSize++; maxQueue = Math.max(maxQueue, this.decodeQueueSize);
    outstanding++; peakOutstanding = Math.max(peakOutstanding, outstanding);
    if (earlyDequeue) {
      this.decodeQueueSize--;
      queueMicrotask(() => this.dispatchEvent(new Event('dequeue')));
    }
    this.pending.push(new Promise(resolve => setTimeout(() => {
      if (this.state !== 'closed') {
        if (!earlyDequeue) this.decodeQueueSize--;
        if (injectError) { injectError = false; this.callbacks.error(new Error('injected failure')); }
        else if (!omitOutput) this.callbacks.output(new FakeFrame(chunk.timestamp));
        this.dispatchEvent(new Event('dequeue'));
      }
      outstanding--;
      resolve();
    }, earlyDequeue ? 5 : 0)));
  }
  async flush() { await Promise.all(this.pending); }
  close() { assert.notEqual(this.state, 'closed', 'decoder closed twice'); this.state = 'closed'; closed++; }
}
globalThis.VideoDecoder = FakeDecoder;
globalThis.EncodedVideoChunk = class { constructor(config) { Object.assign(this, config); } };
globalThis.window = { VideoDecoder: FakeDecoder };
function setup(count = 10000) {
  const model = new AvencModel([]);
  model._emitStatus = () => {};
  model.arrayBuffer = new ArrayBuffer(count);
  const samples = Array.from({ length: count }, (_, i) => ({ number: i, cts: i, dts: i, timescale: 1000, duration: 1, offset: i, size: 1, is_sync: i % 12 === 0 }));
  model.videoTracks = [{ id: 1, codec: 'av01.0.08M.08', video: { width: 64, height: 64 } }, { id: 2, codec: 'av01.0.08M.08', video: { width: 64, height: 64 } }];
  model._mp4File = { getTrackById: () => ({ samples }) };
  model.groups = [0, 1].map(id => ({ id, stream_index: id, frame_count: count, options: { bits: 8 } }));
  return model;
}
const model = setup();
await model.selectGroup(0);
assert.equal(opened, 0, 'opening a group must not decode');
assert.equal(model.getCurrentFrameCount(), 10000);
let frame = await model.getFrameVisual(9997);
assert.equal(frame.timestampUs, 9997000);
assert.ok(inputCount <= 12, 'late seek should start at preceding keyframe');
const inputs = inputCount;
await model.getFrameVisual(9997);
assert.equal(inputCount, inputs, 'cache hit must not decode');
const first = frame.frame;
await model.getFrameVisual(1);
await model.getFrameVisual(13);
assert.equal(first.closed, true, 'eviction must close frame');
assert.equal(model.decodedFrames.size, 2);
assert.equal(live, 2);
const sameA = model.getFrameVisual(30), sameB = model.getFrameVisual(30);
assert.equal((await sameA).frame, (await sameB).frame, 'same-frame requests should coalesce');
const obsolete = model.getFrameVisual(45);
const obsoleteCheck = assert.rejects(obsolete, { name: 'AbortError' });
const latest = model.getFrameVisual(60);
await obsoleteCheck; await latest;
assert.equal(live, 2);
const switching = model.getFrameVisual(79);
const switchCheck = assert.rejects(switching, { name: 'AbortError' });
await model.selectGroup(1); await switchCheck;
assert.equal(live, 0, 'group change releases cached and pending frames');
injectError = true;
await assert.rejects(model.getFrameVisual(5), /injected failure/);
assert.equal(live, 0, 'error releases all partial output');
omitOutput = true;
await assert.rejects(model.getFrameVisual(5), /did not output/);
omitOutput = false;
assert.equal(opened, closed);
// Presentation order can differ from decode order (B-frames).
model._samples[0].cts = 0; model._samples[1].cts = 3; model._samples[2].cts = 1; model._samples[3].cts = 2;
model.decodedStreamIndex = -1;
await model.selectGroup(1);
assert.equal((await model.getFrameVisual(1)).timestampUs, 1000);
const leaving = model.getFrameVisual(100);
const leavingCheck = assert.rejects(leaving, { name: 'AbortError' });
model.close(); await leavingCheck;
assert.equal(live, 0);
assert.equal(opened, closed);
assert.ok(maxQueue <= 4);
assert.ok(peak <= 4, `peak live output frames ${peak}`);
console.log(`Lifecycle checks passed: ${opened} decoders opened/closed; queue peak ${maxQueue}; live output peak ${peak}, final ${live}.`);
// Native decoders can dequeue before delivering output: bound that backlog too.
await new Promise(resolve => setTimeout(resolve, 10));
earlyDequeue = true; peakOutstanding = 0;
const backpressure = setup(200);
for (const sample of backpressure._mp4File.getTrackById(1).samples) sample.is_sync = sample.number === 0;
await backpressure.selectGroup(0);
await backpressure.getFrameVisual(190);
assert.ok(peakOutstanding <= 16, `native input backlog ${peakOutstanding}`);
backpressure.close(); earlyDequeue = false;
// Concurrent hover copies share work; eviction must not close their owned clone.
const hover = setup(); await hover.selectGroup(0);
let finishCopy, copies = 0;
hover._copyPlanes = frame => {
  copies++;
  return new Promise(resolve => { finishCopy = () => { assert.equal(frame.closed, false); resolve({ bits: 8 }); }; });
};
const planeA = hover._ensurePlaneData(0), planeB = hover._ensurePlaneData(0);
while (!finishCopy) await new Promise(resolve => setTimeout(resolve, 1));
assert.equal(copies, 1);
await hover.getFrameVisual(1); await hover.getFrameVisual(2);
finishCopy();
assert.equal(await planeA, await planeB);
hover.close(); assert.equal(live, 0); assert.equal(opened, closed);
console.log('Native backlog bound and concurrent pixel-copy lifetime checks passed.');


if (process.argv[2] && process.argv[3]) {
  const MP4Box = createRequire(import.meta.url)(process.argv[2]);
  window.MP4Box = MP4Box;
  const bytes = await readFile(process.argv[3]);
  const real = new AvencModel([]); real._emitStatus = () => {};
  real.arrayBuffer = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
  const info = await real._parseMp4Info(real.arrayBuffer);
  real.videoTracks = info.videoTracks.slice().sort((a, b) => a.id - b.id);
  real.metadata = real._extractAvencMetadata(new Uint8Array(real.arrayBuffer));
  real.recon = real._extractReconPayload(real.metadata);
  real.groups = real.metadata.groups;
  real._attachRecordsFallback();
  await real.selectGroup(real.groups[0].id);
  const n = real.getCurrentFrameCount();
  for (const index of [0, Math.floor(n / 2), n - 1]) {
    const sample = real._displaySamples[index];
    assert.ok(sample.offset + sample.size <= real.arrayBuffer.byteLength);
    assert.equal((await real.getFrameVisual(index)).timestampUs, Math.round(sample.cts * 1e6 / sample.timescale));
  }
  console.log(`Real MP4 index/metadata checked: ${real.groups.length} groups, ${n} frames in first group. Video decoding mocked.`);
  real.close();
  assert.equal(live, 0); assert.equal(opened, closed);
}
