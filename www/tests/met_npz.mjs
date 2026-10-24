import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
const source=await readFile(new URL('../js/avenc_npz.js',import.meta.url),'utf8');
const {npy,fieldArchive}=await import('data:text/javascript;base64,'+Buffer.from(source).toString('base64'));
function unpack(b){const n=new DataView(b.buffer,b.byteOffset,b.byteLength).getUint16(8,true);assert.equal((10+n)%64,0);return {header:new TextDecoder().decode(b.slice(10,10+n)),data:new DataView(b.buffer,b.byteOffset+10+n,b.byteLength-10-n)};}
const u=new Float32Array([3,NaN,-4,0]),v=new Float32Array([4,0,3,0]);
const files=fieldArchive({u,v,geometry:{nx:2,ny:2,lat0:90,lon0:180,dx:180,dy:-180},config:{unit:'m/s',factor:1,offset:0},metadata:{variable:'wind_speed',valid_time:'2026-10-21T00:00:00Z'}});
const field=unpack(files['field.npy']);assert(field.header.includes("'<f4'"));assert(field.header.includes('(2, 2)'));assert.equal(field.data.getFloat32(0,true),5);assert(Number.isNaN(field.data.getFloat32(4,true)));assert.equal(field.data.getFloat32(8,true),5);
const longitude=unpack(files['longitude.npy']);assert.equal(longitude.data.getFloat64(0,true),-180);assert.equal(longitude.data.getFloat64(8,true),0);
const meta=unpack(files['metadata_json.npy']);const data=JSON.parse(new TextDecoder().decode(new Uint8Array(meta.data.buffer,meta.data.byteOffset,meta.data.byteLength)));assert.equal(data.missing,'NaN');assert.equal(data.vector_units,'m/s');assert.equal(data.reconstructed,true);
const scalar=fieldArchive({u:new Float32Array([273.15,0,NaN,280]),geometry:{nx:2,ny:2,lat0:0,lon0:0,dx:1,dy:1},config:{unit:'°C',factor:1,offset:-273.15},metadata:{variable:'t'}});assert(!scalar['v.npy']);assert(Math.abs(unpack(scalar['field.npy']).data.getFloat32(0,true))<1e-4);
assert.throws(()=>npy(u,[1,3]));assert.throws(()=>npy(new Int16Array(2)));
console.log('PASS: NumPy v1 headers/alignment, Float32 arrays, NaNs, wind speed/components, display conversion, coordinates, UTF-8 metadata and shape validation.');

const raster=fieldArchive({u,geometry:{native:true,nx:2,ny:2,lat0:0,lon0:0,dx:1,dy:-1},config:{unit:'m/s',factor:1,offset:0},metadata:{variable:'u'}});
assert(raster['row.npy']&&raster['column.npy']);assert(!raster['latitude.npy']&&!raster['longitude.npy']);assert.equal(unpack(raster['row.npy']).data.getFloat64(8,true),1);
const rm=unpack(raster['metadata_json.npy']);const rmeta=JSON.parse(new TextDecoder().decode(new Uint8Array(rm.data.buffer,rm.data.byteOffset,rm.data.byteLength)));assert.deepEqual(rmeta.dimensions,['row','column']);assert.equal(rmeta.geometry_mode,'native');assert(Number.isNaN(unpack(raster['field.npy']).data.getFloat32(4,true)));
console.log('PASS: native NPZ has row/column indices, no invented geographic coordinates, declared geometry and preserved NaNs.');
