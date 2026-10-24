// NumPy .npy v1 writer. Only numeric arrays: no pickle/object payloads.
export function npy(array, shape=[array.length]) {
  const types=new Map([[Float32Array,'<f4'],[Float64Array,'<f8'],[Uint8Array,'|u1']]);
  const descr=types.get(array.constructor);
  if(!descr||shape.reduce((a,b)=>a*b,1)!==array.length)throw new Error('Invalid numeric array or shape');
  let header=`{'descr': '${descr}', 'fortran_order': False, 'shape': (${shape.join(', ')}${shape.length===1?',':''}), }`;
  header+=' '.repeat((64-(10+header.length+1)%64)%64)+'\n';
  const out=new Uint8Array(10+header.length+array.byteLength),view=new DataView(out.buffer);
  out.set([0x93,78,85,77,80,89,1,0]);view.setUint16(8,header.length,true);out.set(new TextEncoder().encode(header),10);
  const offset=10+header.length;
  // Explicit little endian to match the declared NumPy dtype on any host.
  if(array instanceof Uint8Array)out.set(array,offset);
  else {const little=new Uint8Array(new Uint16Array([1]).buffer)[0]===1;
    if(little)out.set(new Uint8Array(array.buffer,array.byteOffset,array.byteLength),offset);
    else for(let i=0;i<array.length;i++)array instanceof Float32Array?view.setFloat32(offset+i*4,array[i],true):view.setFloat64(offset+i*8,array[i],true);
  }
  return out;
}
export function fieldArchive({u,v,geometry,config,metadata}){
  const {nx,ny,lat0,lon0,dx,dy}=geometry;
  if(u.length!==nx*ny||(v&&v.length!==u.length))throw new Error('Field/grid shape mismatch');
  const field=new Float32Array(u.length);
  for(let i=0;i<u.length;i++)field[i]=(v?Math.hypot(u[i],v[i]):u[i])*config.factor+config.offset;
  const native=Boolean(geometry.native),dimensions=native?['row','column']:['latitude','longitude'];
  const y=Float64Array.from({length:ny},(_,i)=>native?i:lat0+dy*i);
  const x=Float64Array.from({length:nx},(_,i)=>native?i:((lon0+dx*i+180)%360+360)%360-180);
  const meta={...metadata,shape:[ny,nx],dimensions,geometry_mode:native?'native':'geographic',units:config.unit,source_units:config.sourceUnit||(v?'m/s':null),display_factor:config.factor,display_offset:config.offset,missing:'NaN',reconstructed:true,notice:'Highly compressed visualization data; not for scientific analysis or operational decisions.'};
  if(native)meta.geographic_positioning='unavailable; row/column indices in stored raster order';
  const files={'field.npy':npy(field,[ny,nx]),[dimensions[0]+'.npy']:npy(y),[dimensions[1]+'.npy']:npy(x),'metadata_json.npy':npy(new TextEncoder().encode(JSON.stringify(meta)))};
  if(v){files['u.npy']=npy(u,[ny,nx]);files['v.npy']=npy(v,[ny,nx]);meta.vector_units='m/s';if(native)meta.vector_orientation='unspecified; no geographical direction inferred';files['metadata_json.npy']=npy(new TextEncoder().encode(JSON.stringify(meta)));}
  return files;
}
