// Pure meteorological helpers, shared by the browser and Node tests.
export const RAW_NAMES = {'0/6/192':'tcc','0/1/198':'sf','0/2/62':'ewss','0/2/63':'nsss','0/3/22':'slor','0/19/192':'asn','10/2/1':'sithick','10/3/1':'zos','10/3/14':'sve','10/3/15':'svn'};
export function rawParameter(template) {
  if (!template) return '';
  const b=Uint8Array.from(atob(template),c=>c.charCodeAt(0));
  if(b.length<16 || b[7]!==2) return '';
  const d=new DataView(b.buffer); let p=16;
  while(p+11<b.length){const n=d.getUint32(p);if(n<5||p+n>b.length)break;if(b[p+4]===4)return `${b[6]}/${b[p+9]}/${b[p+10]}`;p+=n;}
  return '';
}
export function rawName(template) {return RAW_NAMES[rawParameter(template)]||'';}
export function resolveVariable(name,raw,catalogue={}) {
  const entries=catalogue.variables||{};
  if(name!=='unknown'&&entries[name])return {name,entry:entries[name]};
  const alias=Object.entries(entries).find(([,v])=>v.aliases?.includes(name));
  if(alias)return {name:alias[0],entry:alias[1]};
  const matches=Object.entries(entries).filter(([,v])=>v.match&&[v.match.discipline,v.match.parameterCategory,v.match.parameterNumber].join('/')===raw);
  // Raw temperature codes, for example, may have different statistical meanings.
  // Resolve only unique matches; never guess units from data values.
  return matches.length===1?{name:matches[0][0],entry:matches[0][1]}:{name:name==='unknown'?(RAW_NAMES[raw]||name):name,entry:null};
}
export function buildInventory(groups,catalogue={}) {
  const views=new Map();
  for(const g of groups){
    const raw=rawParameter(g.options?.template_b64);
    for(const r of g.records||[]){
      const resolved=resolveVariable(r.shortName||g.short_name,raw,catalogue),name=resolved.name;
      const kind=r.typeOfLevel||g.type_of_level,level=String(r.level??g.level??'');
      const member=String(r.perturbationNumber||r.number||'');
      const key=[name,kind,level,member].join('|');
      if(!views.has(key))views.set(key,{key,name,kind,level,member,rawCode:raw,variable:resolved.entry,frames:[]});
      const ms=Date.parse(r.timestamp);
      if(Number.isFinite(ms))views.get(key).frames.push({group:g,frame:Number(r.frame_index),record:r,ms});
    }
  }
  for(const view of views.values())view.frames.sort((a,b)=>a.ms-b.ms||a.frame-b.frame);
  return [...views.values()];
}
export function displayConfiguration(view){
  const v=view?.variable;
  if(!v)return {title:view?.name||'Field',unit:'unit unknown',factor:1,offset:0,range:[0,1],palette:'wind',auto:true,decimals:3};
  return {title:v.title||v.name,description:v.name,sourceUnit:v.units,range:[0,1],...v.display};
}
export function formatReading(value,config){
  if(!Number.isFinite(value))return 'No data';
  const decimals=config.decimals??1,abs=Math.abs(value);
  return abs>=10000||(abs>0&&abs<.001)?value.toExponential(2):value.toFixed(decimals);
}
export function pairWind(u,v){
  if(!u||!v||u.kind!==v.kind||u.level!==v.level||u.member!==v.member)return [];
  const other=new Map(v.frames.map(f=>[f.ms,f]));
  return u.frames.filter(f=>other.has(f.ms)).map(f=>({...f,v:other.get(f.ms)}));
}
export function decodeValue(raw,bits,bounds,opts,hasBitmap,missing=false){
  if(missing||(!opts.bitmap_payload&&hasBitmap&&raw===0))return NaN;
  let q=Number(raw),shift=Number(opts.storage_shift||0);
  if(shift>0)q=Math.floor((q+2**(shift-1))/2**shift);
  const max=2**bits-1;q=Math.min(max,q);
  if(hasBitmap)q=Math.max(0,q-1);
  const norm=q/(hasBitmap?max-1:max);
  let value=bounds.min+norm*(bounds.max-bounds.min);
  if(opts.apply_shifted_log){const a=Number(opts.transform_scale??1);value=Number(opts.transform_origin??0)+Number(opts.transform_span??1)*Math.expm1(value*Math.log1p(a))/a;}
  else if(opts.apply_asinh)value=Number(opts.transform_scale??1)*Math.sinh(value);
  else if(opts.apply_log2)value=2**value-Number(opts.log_offset??1);
  return value;
}
export function makeGeometry(lat,lon,nx,ny){
  if(lat.length!==ny||lon.length!==nx||nx<2||ny<2)throw new Error('This map needs a regular latitude/longitude grid with embedded coordinate axes.');
  const dy=(lat[ny-1]-lat[0])/(ny-1),dx=(lon[nx-1]-lon[0])/(nx-1);
  if(!dy||!dx||lat.some((v,j)=>Math.abs(v-lat[0]-j*dy)>.002)||lon.some((v,i)=>Math.abs(v-lon[0]-i*dx)>.002))throw new Error('Only regular latitude/longitude IFS grids are supported.');
  const cyclic=Math.abs(Math.abs(dx)*nx-360)<.01;
  return {nx,ny,lat0:lat[0],lon0:lon[0],dx,dy,cyclic};
}
export function gridIndex(geo,lat,lon){
  const j=Math.round((lat-geo.lat0)/geo.dy);
  let x=(lon-geo.lon0)/geo.dx;
  if(geo.cyclic)x=((x%geo.nx)+geo.nx)%geo.nx;
  const i=Math.round(x)% (geo.cyclic?geo.nx:Infinity);
  return j<0||j>=geo.ny||i<0||i>=geo.nx?-1:j*geo.nx+i;
}
export function vectorAt(geo,u,v,lat,lon){const k=gridIndex(geo,lat,lon);return k<0?null:{u:u[k],v:v[k],speed:Math.hypot(u[k],v[k])};}
// Read GRIB2 template 3.0 rather than interpolating across a wrapped endpoint.
// A global grid may start at 180 E and finish at 179.75 E: its true dx is +.25.
export function gribAxes(template){
  if(!template)return null;
  const b=Uint8Array.from(atob(template),c=>c.charCodeAt(0)),d=new DataView(b.buffer);
  if(b.length<16||b[7]!==2)return null;
  let p=16;
  while(p+14<=b.length){
    const n=d.getUint32(p);if(n<5||p+n>b.length)break;
    if(b[p+4]===3){
      if(d.getUint16(p+12)!==0||n<72)return null;
      const signed=off=>{const v=d.getUint32(p+off);return (v&0x80000000)?-(v&0x7fffffff):v;};
      const nx=d.getUint32(p+30),ny=d.getUint32(p+34),basic=d.getUint32(p+38),sub=d.getUint32(p+42);
      const unit=basic&&basic!==0xffffffff&&sub&&sub!==0xffffffff?basic/sub:1e-6;
      const scan=b[p+71];if(scan&48)throw new Error('Alternating or column-consecutive GRIB scanning is not supported by this map.');
      const lat0=signed(46)*unit,lon0=signed(50)*unit;
      const dx=d.getUint32(p+63)*unit*((scan&128)?-1:1),dy=d.getUint32(p+67)*unit*((scan&64)?1:-1);
      return {lat:Array.from({length:ny},(_,j)=>lat0+j*dy),lon:Array.from({length:nx},(_,i)=>lon0+i*dx)};
    }p+=n;
  }return null;
}
// Match FFmpeg's full-range 10/12-bit -> gray16 bit replication before
// applying the metadata's storage shift (not an RGB color conversion).
export function lumaStorageCode(raw,nativeBits,storedBits){
  const shift=storedBits-nativeBits;
  if(shift===0)return raw;
  if(shift<0)return Math.floor(raw/2**(-shift));
  return raw*2**shift+Math.floor(raw/2**(nativeBits-shift));
}

// Unknown projections/scanning are still viewable as the stored row-major raster.
// Do not infer geographic coordinates from image dimensions or placeholder axes.
export function streamGeometry(group){
  const nx=Number(group.options.crop_width),ny=Number(group.options.crop_height);
  if(!Number.isSafeInteger(nx)||!Number.isSafeInteger(ny)||nx<1||ny<1)throw new Error('Invalid native grid dimensions');
  try{const axes=gribAxes(group.options.template_b64);if(axes)return {...makeGeometry(axes.lat,axes.lon,nx,ny),native:false};}catch{/* Native raster fallback; no geographic approximation. */}
  return {nx,ny,native:true,lat0:0,lon0:0,dx:1,dy:-1,cyclic:false};
}
export function compatibleGeometry(a,b){return a.native===b.native&&['nx','ny','lat0','lon0','dx','dy'].every(k=>Math.abs(a[k]-b[k])<1e-6);}
