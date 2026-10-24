// Run: node www/tests/transforms.mjs [optional native MP4 fixtures...]
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { execFileSync } from 'node:child_process';
const modelSource = (await readFile(new URL('../js/avenc_model.js', import.meta.url), 'utf8'))
  .replace(/^import .*?;\n/, 'import { unzipSync as decompressSync } from "node:zlib"; const strFromU8 = b => new TextDecoder().decode(b);\n')
  + '\nexport { decodeAvencCommentPayload };';
const { decodeAvencCommentPayload, AvencModel } = await import(`data:text/javascript;base64,${Buffer.from(modelSource).toString('base64')}`);
const extractorSource = (await readFile(new URL('../js/avenc_extractor.js', import.meta.url), 'utf8'))
  .replace(/^import .*?;\n/gm, '').split('const extractor = new AvencExtractor();')[0]
  + '\nexport { AvencExtractor };';
const { AvencExtractor } = await import(`data:text/javascript;base64,${Buffer.from(extractorSource).toString('base64')}`);
const summarySource = (await readFile(new URL('../js/avenc_metadata.js', import.meta.url), 'utf8'))
  .replace(/^import .*?;\n/, 'import { unzipSync as decompressSync } from "node:zlib"; const strFromU8 = b => new TextDecoder().decode(b);\n')
  + '\nexport { parseV2BinarySummary };';
const { parseV2BinarySummary }=await import(`data:text/javascript;base64,${Buffer.from(summarySource).toString('base64')}`);
const decode = AvencExtractor.prototype.decodePixelValue;
for (const q of [0, 2047, 4095]) {
  const lo = Math.asinh(-5), hi = Math.asinh(5), encoded = lo + q / 4095 * (hi - lo);
  const actual = decode(q, 12, 12, {min:lo,max:hi}, {apply_asinh:true}, false);
  assert(Math.abs(actual - Math.sinh(encoded)) < 1e-12);
  const log = decode(q, 12, 12, {min:0,max:1}, {apply_log2:true,log_offset:1}, false);
  assert(Math.abs(log - (2 ** (q/4095) - 1)) < 1e-12);
}
for(const q of [0,2047,4095]){
  const y=-2+q/4095*4;
  const scaled=decode(q,12,12,{min:-2,max:2},{apply_asinh:true,transform_scale:1e-6},false);
  assert(Math.abs(scaled-1e-6*Math.sinh(y))<1e-18);
  const shifted=decode(q,12,12,{min:0,max:1},{apply_shifted_log:true,transform_scale:99,transform_origin:-5,transform_span:10},false);
  assert(Math.abs(shifted-(-5+10*Math.expm1(q/4095*Math.log1p(99))/99))<1e-12);
}
// Equal per-frame bounds must survive even when the stream range varies.
const bounds = AvencModel.prototype._getFrameQuantBounds;
for (const constant of [0, 7.5, -2.25]) {
  const context = {currentGroup:{value_min:-1000,value_max:1000},
    _getFrameRecord:()=>({quantization_min:constant,quantization_max:constant})};
  const range = bounds.call(context,0);
  assert.deepEqual(range,{min:constant,max:constant});
  for (const q of [0,1,29039,65535])
    assert.equal(decode(q,16,16,range,{},false),constant);
}
for (const file of process.argv.slice(2)) {
  const probe = JSON.parse(execFileSync('ffprobe', ['-v','error','-show_entries','format_tags=comment','-of','json',file]));
  const comment = probe.format.tags.comment, split = comment.indexOf('|')+1;
  const meta = decodeAvencCommentPayload(comment.slice(0,split),comment.slice(split));
  const options = meta.groups[0].options;
  assert.equal(Boolean(options.apply_asinh), file.includes('asinh'));
  assert.equal(Boolean(options.apply_log2), file.includes('log')&&!file.includes('shifted'));
  assert.equal(Boolean(options.apply_shifted_log),file.includes('shifted'));
  const summary=parseV2BinarySummary(comment.slice(split),0);
  if(file.includes('scaled_asinh')){
    assert.equal(options.transform_scale,1e-6);
    assert.equal(summary.stream_summaries[0].transform_scale,1e-6);
  }
  if(file.includes('shifted')){
    assert.equal(options.transform_scale,99);assert.equal(summary.stream_summaries[0].apply_shifted_log,true);
    assert(options.transform_origin<0);assert(options.transform_span>0);
  }
}
console.log('PASS: signed asinh/log pixel inverses and native metadata in the browser model.');
