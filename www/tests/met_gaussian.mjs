import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
const load=async name=>import('data:text/javascript;base64,'+Buffer.from(await readFile(new URL('../js/'+name,import.meta.url),'utf8')).toString('base64'));
const {gaussianLatitudes,gribAxes,streamGeometry,gridIndex,latitudeRow,compatibleGeometry}=await load('avenc_met_core.js');
const {fieldArchive}=await load('avenc_npz.js'),{sampleVelocity}=await load('avenc_trajectories.js');
const latitude=gaussianLatitudes(4);
assert(Math.abs(latitude[0]-59.44440828916677)<1e-10);assert(Math.abs(latitude[1]-19.8757191474409)<1e-10);
for(let j=0;j<4;j++)assert(Math.abs(latitude[j]+latitude[3-j])<1e-12);
const full=gaussianLatitudes(2560);assert(Math.abs(full[0]-89.94618771566563)<1e-10);assert.equal(full.length,2560);assert(full.every((v,j)=>!j||v<full[j-1]));
assert.notEqual(full[0]-full[1],full[100]-full[101]);
const first=full[0];full[0]=0;assert.equal(gaussianLatitudes(2560)[0],first,'cached axes are immutable to callers');
assert.throws(()=>gaussianLatitudes(3));assert.throws(()=>gaussianLatitudes(100000));
function template({nx=8,ny=4,lon0=180,scan=0,type=40,N=ny/2}={}){
 const b=new Uint8Array(88),d=new DataView(b.buffer),p=16,lat=gaussianLatitudes(ny);if(scan&64)lat.reverse();
 b.set(Buffer.from('GRIB'));b[7]=2;d.setUint32(p,72);b[p+4]=3;d.setUint16(p+12,type);d.setUint32(p+6,nx*ny);
 d.setUint32(p+30,nx);d.setUint32(p+34,ny);b[p+54]=48;b[p+71]=scan;
 const signed=(offset,value)=>d.setUint32(p+offset,(Math.round(Math.abs(value)*1e6)|(value<0?0x80000000:0))>>>0);
 signed(46,lat[0]);signed(50,lon0);signed(55,lat.at(-1));signed(59,((lon0+(scan&128?-1:1)*(nx-1)*360/nx)%360+360)%360);
 d.setUint32(p+63,Math.round(360/nx*1e6));d.setUint32(p+67,N);return Buffer.from(b).toString('base64');
}
const group=(b,nx=8,ny=4)=>({options:{crop_width:nx,crop_height:ny,template_b64:b}});
const geo=streamGeometry(group(template()));assert.equal(geo.kind,'gaussian');assert.equal(geo.native,false);assert.equal(geo.cyclic,true);assert.equal(geo.lon0,180);assert.equal(geo.dx,45);
assert.equal(gridIndex(geo,latitude[1],0),12);assert.equal(gridIndex(geo,90,180),0);assert.equal(gridIndex(geo,-90,-180),24);assert.equal(gridIndex(geo,91,0),-1);
assert(Math.abs(latitudeRow(geo,(latitude[0]+latitude[1])/2)-.5)<1e-12);
for(const scan of [0,64,128,192]){
 const axes=gribAxes(template({scan})),g=streamGeometry(group(template({scan})));
 assert.equal(g.native,false);assert.equal(Math.sign(g.dx),scan&128?-1:1);assert.equal(Math.sign(g.dy),scan&64?1:-1);
 assert.equal(gridIndex(g,axes.lat[2],axes.lon[3]),2*8+3);
}
const pof=streamGeometry(group(template({nx:5136,ny:2560}),5136,2560));
assert.equal(pof.native,false);assert.equal(pof.gaussianN,1280);assert(Math.abs(pof.dx-360/5136)<1e-14);assert.equal(gridIndex(pof,pof.latitudes[1280],180),1280*5136);
assert.equal(streamGeometry(group(template({scan:16}))).native,true);assert.equal(streamGeometry(group(template({N:3}))).native,true);
assert(compatibleGeometry(geo,streamGeometry(group(template()))));assert(!compatibleGeometry(geo,{...geo,kind:'regular'}));
const fallback=streamGeometry({options:{crop_width:1440,crop_height:721}});assert(fallback.assumed);assert.equal(fallback.lon0,0);assert.equal(gridIndex(fallback,90,0),0);
assert.equal(streamGeometry(group(template({nx:1440,ny:720,type:30}),1440,720)).native,true,'known projection stays native');
assert(streamGeometry(group(template({nx:1440,ny:720,type:30}),1440,720),{assumeGlobal:true}).assumed);
assert.equal(streamGeometry(group(template()),{assumeGlobal:true}).lon0,180,'real metadata wins over fallback');
const u=Float32Array.from({length:32},(_,k)=>latitude[Math.floor(k/8)]),v=new Float32Array(32);
assert(Math.abs(sampleVelocity(geo,u,v,(latitude[0]+latitude[1])/2,180)[0]-(latitude[0]+latitude[1])/2)<1e-5);
const files=fieldArchive({u,v,geometry:geo,config:{unit:'%',factor:1,offset:0},metadata:{variable:'pof'}});
const b=files['latitude.npy'],d=new DataView(b.buffer,b.byteOffset,b.byteLength),offset=10+d.getUint16(8,true);
assert.equal(d.getFloat64(offset,true),latitude[0]);assert.equal(d.getFloat64(offset+8,true),latitude[1]);
console.log('PASS: exact Gaussian roots/N1280 grid, scan directions, 180E wrap, CPU sampling, velocity interpolation, NPZ axes, and labelled global fallback with projected-grid protection.');
