import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
const text=await readFile(new URL('../js/avenc_met_core.js',import.meta.url),'utf8');
const {decodeValue,makeGeometry,gridIndex,buildInventory,pairWind,gribAxes,lumaStorageCode}=await import('data:text/javascript;base64,'+Buffer.from(text).toString('base64'));
const geo=makeGeometry([90,0,-90],[0,90,180,270],4,3);
assert.equal(gridIndex(geo,0,-90),7);assert.equal(gridIndex(geo,0,270),7);assert.equal(gridIndex(geo,90,360),0);
assert.equal(gridIndex(geo,-90,-180),10);
const ascending=makeGeometry([-90,0,90],[0,90,180,270],4,3);assert.equal(gridIndex(ascending,90,-90),11);
assert.throws(()=>makeGeometry([90,1,-90],[0,90,180,270],4,3));
for(const bits of [8,10,12,16]){
 const max=2**bits-1;
 assert.equal(decodeValue(0,bits,{min:0,max:10},{},false),0);
 assert.equal(decodeValue(max,bits,{min:0,max:10},{},false),10);
 assert.equal(decodeValue(123,bits,{min:0,max:0},{},false),0);
 assert(Number.isNaN(decodeValue(0,bits,{min:0,max:10},{},true)));
 assert(Number.isNaN(decodeValue(5,bits,{min:0,max:10},{bitmap_payload:'x'},true,true)));
 assert.equal(decodeValue(1,bits,{min:0,max:10},{bitmap_payload:'x'},true),0);
 assert.equal(decodeValue(max,bits,{min:0,max:10},{bitmap_payload:'x'},true),10);
 assert.equal(decodeValue(max,bits,{min:0,max:1},{apply_asinh:true,transform_scale:2},false),2*Math.sinh(1));
 assert(Math.abs(decodeValue(max,bits,{min:0,max:1},{apply_shifted_log:true,transform_scale:99,transform_origin:-5,transform_span:10},false)-5)<1e-12);
}
const group=(id,name,level,ts)=>({id,short_name:name,type_of_level:'isobaricInhPa',level,options:{},records:ts.map((t,i)=>({shortName:name,typeOfLevel:'isobaricInhPa',level,frame_index:i,timestamp:t}))});
const a=buildInventory([group(0,'u','500',['2026-10-19T00:00:00Z','2026-10-19T03:00:00Z']),group(1,'v','500',['2026-10-19T03:00:00Z']),group(2,'v','850',['2026-10-19T00:00:00Z'])]);
assert.equal(pairWind(a[0],a[1]).length,1);assert.equal(pairWind(a[0],a[1])[0].frame,1);assert.equal(pairWind(a[0],a[2]).length,0);
console.log('PASS: coordinates/wrapping/orientation, 8/10/12/16-bit transforms, masks/constants and exact U/V valid-time + level pairing.');

assert.equal(lumaStorageCode(551,10,16),35298);
assert.equal(lumaStorageCode(512,10,16),32800);
assert.equal(lumaStorageCode(4095,12,16),65535);
// Synthetic GRIB template 3.0 with wrapped 180 -> 179.75 longitude endpoints.
const grib=new Uint8Array(88),dv=new DataView(grib.buffer);grib[7]=2;dv.setUint32(16,72);grib[20]=3;
dv.setUint32(46,1440);dv.setUint32(50,721);dv.setUint32(62,90000000);dv.setUint32(66,180000000);dv.setUint32(79,250000);dv.setUint32(83,250000);
const axes=gribAxes(Buffer.from(grib).toString('base64'));assert.equal(axes.lon[1439],539.75);assert.equal(axes.lat[720],-90);
const wrapped=makeGeometry(axes.lat,axes.lon,1440,721);assert.equal(gridIndex(wrapped,46,5),176*1440+740);
console.log('PASS: gray16 storage alignment and dateline-wrapped GRIB geometry.');
