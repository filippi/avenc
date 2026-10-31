import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
const text=await readFile(new URL('../js/avenc_met_core.js',import.meta.url),'utf8');
const {decodeValue,makeGeometry,gridIndex,buildInventory,pairWind,gribAxes,lumaStorageCode,streamGeometry,compatibleGeometry}=await import('data:text/javascript;base64,'+Buffer.from(text).toString('base64'));
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

const rect={options:{crop_width:4,crop_height:3}};
const native=streamGeometry(rect);assert.equal(native.native,true);assert.equal(gridIndex(native,0,0),0);assert.equal(gridIndex(native,-2,3),11);assert.equal(gridIndex(native,1,0),-1);assert.equal(native.cyclic,false);
const regular={options:{crop_width:1440,crop_height:721,template_b64:Buffer.from(grib).toString('base64')}};assert.equal(streamGeometry(regular).native,false);
const projected=grib.slice();new DataView(projected.buffer).setUint16(28,30);assert.equal(streamGeometry({options:{crop_width:1440,crop_height:721,template_b64:Buffer.from(projected).toString('base64')}}).native,true);
const alternating=grib.slice();alternating[87]=16;assert.equal(streamGeometry({options:{crop_width:1440,crop_height:721,template_b64:Buffer.from(alternating).toString('base64')}}).native,true);
assert(compatibleGeometry(native,{...native}));assert(!compatibleGeometry(native,{...native,nx:5}));assert(!compatibleGeometry(native,{...native,native:false}));assert.throws(()=>streamGeometry({options:{crop_width:0,crop_height:3}}));
console.log('PASS: unsupported/missing grid and scan fallback, truthful native row/column orientation, invalid dimensions and geometry compatibility.');

// GRIB1 regular lat/lon: honour signed 24-bit millidegrees and scan directions.
function grib1({nx=3600,ny=1801,lat=90,lon=0,dx=.1,dy=-.1,scan=0,type=0,flags=128}={}){
 const b=new Uint8Array(68),d=new DataView(b.buffer),p=36;
 b.set(Buffer.from('GRIB'));b[7]=1;b[10]=28;b[15]=128;b[p+2]=32;b[p+4]=255;b[p+5]=type;
 const coordinate=(o,v)=>{const n=Math.round(Math.abs(v)*1000)|(v<0?0x800000:0);b[p+o]=n>>>16;b[p+o+1]=n>>>8;b[p+o+2]=n;};
 d.setUint16(p+6,nx);d.setUint16(p+8,ny);coordinate(10,lat);coordinate(13,lon);b[p+16]=flags;
 coordinate(17,lat+(ny-1)*dy);coordinate(20,((lon+(nx-1)*dx)%360+360)%360);
 d.setUint16(p+23,Math.round(Math.abs(dx)*1000));d.setUint16(p+25,Math.round(Math.abs(dy)*1000));b[p+27]=scan;return b;
}
const encoded=b=>Buffer.from(b).toString('base64');
const landAxes=gribAxes(encoded(grib1())),land=streamGeometry({options:{crop_width:3600,crop_height:1801,template_b64:encoded(grib1())}});
assert.equal(land.native,false);assert.equal(land.cyclic,true);assert.equal(landAxes.lat[1800],-90);assert(Math.abs(landAxes.lon[3599]-359.9)<1e-9);assert(Math.abs(land.dx-.1)<1e-12);assert.equal(gridIndex(land,46,5),440*3600+50);
const g1wrapped=gribAxes(encoded(grib1({nx:8,ny:5,lat:90,lon:180,dx:45,dy:-45})));assert.equal(g1wrapped.lon.at(-1),495);assert.equal(gridIndex(makeGeometry(g1wrapped.lat,g1wrapped.lon,8,5),0,0),2*8+4);
const reversed=gribAxes(encoded(grib1({nx:8,ny:5,lat:-90,lon:-45,dx:-45,dy:45,scan:192})));assert.equal(reversed.lat.at(-1),90);assert.equal(reversed.lon.at(-1),-360);
for(const bytes of [grib1({type:10}),grib1({nx:65535}),grib1({scan:32}),grib1({scan:16}),grib1({flags:0}),grib1().slice(0,67)])assert.equal(streamGeometry({options:{crop_width:3600,crop_height:1801,template_b64:encoded(bytes)}}).native,true);
const noGDS=grib1();noGDS[15]=0;assert.equal(gribAxes(encoded(noGDS)),null);
const inconsistent=grib1();inconsistent[36+19]=0;assert.equal(gribAxes(encoded(inconsistent)),null);
assert.equal(streamGeometry({options:{crop_width:4,crop_height:3,template_b64:encoded(grib1())}}).native,true);
console.log('PASS: GRIB1 ERA5-Land geometry, signed coordinates, both scan directions, dateline wrapping, unsupported/truncated grids and dimension checks.');
