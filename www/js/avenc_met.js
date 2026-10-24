import { zip } from 'https://cdn.jsdelivr.net/npm/fflate@0.8.2/esm/browser.js';
import { accessExamples } from './avenc_access.js';
import { fieldArchive } from './avenc_npz.js';
import { AvencRangeModel } from './avenc_range_model.js';
import { buildInventory, pairWind, gridIndex, displayConfiguration, formatReading, compatibleGeometry } from './avenc_met_core.js';
const $=id=>document.getElementById(id),query=new URLSearchParams(location.search);
let datasetCatalogue=null,exportBusy=false;
let catalogue=null,catalogueError='';
const cataloguePromise=fetch(new URL('../data/ifs_variables.json',import.meta.url)).then(async r=>{if(!r.ok)throw new Error('Variable catalogue unavailable');const c=await r.json();if(c.schema!=='ecflix.variables.v1')throw new Error('Invalid variable catalogue');return c;}).catch(e=>{catalogueError=e.message;return null;});
const model=new AvencRangeModel(),map=L.map('map',{zoomControl:false,worldCopyJump:false,minZoom:2,maxZoom:10}).setView([46,5],4);
L.control.zoom({position:'topright'}).addTo(map);const scaleControl=L.control.scale({position:'bottomleft',imperial:false}).addTo(map);
map.createPane('weather');map.getPane('weather').style.zIndex=420;
let base,labels;
function basemap(kind){
  if(base)map.removeLayer(base);if(labels)map.removeLayer(labels);base=labels=null;
  if(kind==='none'||model.geometry?.native)return;
  const attribution='© <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors';
  map.getPane('tilePane').classList.toggle('dark-base',kind==='dark');
  base=L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{attribution,maxZoom:19}).addTo(map);
}
// Wait for grid classification before requesting geographic background tiles.
const PALETTES={signed:[[45,70,133],[63,138,180],[165,210,222],[232,239,232],[245,190,128],[217,109,76],[153,45,77]],wind:[[0,46,81],[28,113,154],[42,169,171],[122,202,151],[224,208,107],[246,147,78],[210,66,100],[126,56,142]],temperature:[[71,60,129],[52,121,175],[85,188,185],[183,218,151],[248,216,112],[242,139,83],[191,60,74]],rain:[[31,68,94],[46,119,159],[56,191,181],[140,205,107],[245,215,87],[234,124,71],[172,55,110]],cloud:[[28,52,73],[67,94,113],[117,141,155],[180,197,204],[238,245,246]]};
const FIELDS={wind:{title:'Wind',description:'Wind speed & direction',unit:'m/s',range:[0,40],ticks:[0,10,20,30,40],palette:'wind',factor:1,offset:0},temperature:{title:'Temperature',description:'Air temperature',unit:'°C',range:[-30,40],ticks:[-30,-10,10,30,40],palette:'temperature',factor:1,offset:-273.15},rain:{title:'Precipitation',description:'Total accumulated precipitation',unit:'mm',range:[0,100],ticks:[0,1,10,50,100],palette:'rain',factor:1000,offset:0,log:true},cloud:{title:'Cloud cover',description:'Total cloud cover',unit:'%',range:[0,100],ticks:[0,25,50,75,100],palette:'cloud',factor:100,offset:0}};
let views=[],supported=new Map(),frames=[],index=0,field='wind',custom=null,activeConfig=FIELDS.wind,current=null,controller=null,loadController=null,sequence=0,playing=false,timer=null,point=null,loading=false,noticeTimer=null;
function tell(text,error=false){$('messageText').textContent=text;$('message').classList.toggle('error',error);$('message').classList.remove('ready');}
function bytes(n){return n>=1e6?(n/1e6).toFixed(2)+' MB':(n/1024).toFixed(1)+' KB';}
function network(t){$('network').textContent=`${bytes(t.bytes)} transferred · HTTP Range`;}
function date(ms){return new Date(ms).toLocaleString('en-GB',{day:'2-digit',month:'short',hour:'2-digit',minute:'2-digit',timeZone:'UTC',hour12:false})+' UTC';}
function palette(config){const stops=PALETTES[config.palette]||PALETTES.wind;return Array.from({length:256},(_,i)=>{const x=i/255*(stops.length-1),a=Math.floor(x),b=Math.min(a+1,stops.length-1),t=x-a;return stops[a].map((v,j)=>Math.round(v*(1-t)+stops[b][j]*t));});}
function normalized(v,c){return Math.max(0,Math.min(1,c.log?Math.log1p(Math.max(0,v))/Math.log1p(c.range[1]):(v-c.range[0])/(c.range[1]-c.range[0])));}
function updateLegend(){
  const c=activeConfig,canvas=$('legend'),ctx=canvas.getContext('2d'),colors=palette(c),image=ctx.createImageData(canvas.width,canvas.height);
  for(let y=0;y<canvas.height;y++)for(let x=0;x<canvas.width;x++){const k=(y*canvas.width+x)*4,rgb=colors[Math.round((1-y/(canvas.height-1))*255)];image.data.set([...rgb,255],k);}ctx.putImageData(image,0,0);
  $('legendTitle').textContent=c.title==='Wind'?'Wind speed':c.title;$('legendUnit').textContent=c.unit;
  $('legendTicks').replaceChildren(...(c.ticks||[c.range[0],(c.range[0]+c.range[1])/2,c.range[1]]).map(v=>{const el=document.createElement('span');el.textContent=Number(v.toPrecision(3));el.style.top=(100*(1-normalized(v,c)))+'%';el.style.transform='translateY(-50%)';return el;}));
}
const Weather=L.Layer.extend({
  onAdd(m){this._map=m;this.canvas=L.DomUtil.create('canvas','met-canvas');this.flow=L.DomUtil.create('canvas','met-canvas');m.getPane('weather').append(this.canvas,this.flow);this.render=this.render.bind(this);m.on('moveend resize zoomend',this.render);m.on('movestart',this.hide,this);m.on('moveend',this.show,this);this.render();this.last=0;this.animate=this.animate.bind(this);this.raf=requestAnimationFrame(this.animate);},
  hide(){this.flow.style.opacity='0';this.canvas.style.opacity='0';},show(){this.flow.style.opacity='1';this.canvas.style.opacity='1';},
  onRemove(m){cancelAnimationFrame(this.raf);m.off('moveend resize zoomend',this.render);m.off('movestart',this.hide,this);m.off('moveend',this.show,this);this.canvas.remove();this.flow.remove();},
  render(){
    const sz=map.getSize();this.width=Math.ceil(sz.x/2);this.height=Math.ceil(sz.y/2);
    const resized=this.canvas.width!==this.width||this.canvas.height!==this.height;
    for(const c of [this.canvas,this.flow]){if(resized){c.width=this.width;c.height=this.height;}c.style.width=sz.x+'px';c.style.height=sz.y+'px';L.DomUtil.setPosition(c,map.containerPointToLayerPoint([0,0]));}
    if(!current){this.canvas.getContext('2d').clearRect(0,0,this.width,this.height);this.flow.getContext('2d').clearRect(0,0,this.width,this.height);return;}
    const ctx=this.canvas.getContext('2d'),im=ctx.createImageData(this.width,this.height),geo=model.geometry,colors=palette(activeConfig);
    this.cols=new Int32Array(this.width);this.rows=new Int32Array(this.height);this.lats=new Float32Array(this.height);
    for(let x=0;x<this.width;x++){const ll=map.containerPointToLatLng([x*2,0]);const k=gridIndex(geo,geo.lat0,ll.lng);this.cols[x]=k<0?-1:k%geo.nx;}
    for(let y=0;y<this.height;y++){const ll=map.containerPointToLatLng([0,y*2]);const k=gridIndex(geo,ll.lat,geo.lon0);this.rows[y]=k<0?-1:Math.floor(k/geo.nx);this.lats[y]=ll.lat;}
    for(let y=0;y<this.height;y++)for(let x=0;x<this.width;x++){
      const i=this.cols[x],j=this.rows[y];if(i<0||j<0)continue;const k=j*geo.nx+i;
      let v=current.v?Math.hypot(current.u[k],current.v[k]):current.u[k];v=v*activeConfig.factor+activeConfig.offset;if(!Number.isFinite(v))continue;
      const rgb=colors[Math.round(normalized(v,activeConfig)*255)],off=(y*this.width+x)*4;
      im.data.set(rgb,off);im.data[off+3]=(field==='rain'&&v<.05)?0:Math.round(Number($('opacity').value)*2.55);
    }
    ctx.putImageData(im,0,0);if(resized||!this.particles)this.particles=Array.from({length:Math.min(750,Math.floor(this.width*this.height/500))},()=>({x:Math.random()*this.width,y:Math.random()*this.height,age:Math.random()*90}));if(!current.v)this.flow.getContext('2d').clearRect(0,0,this.width,this.height);this.drawArrows();updateNativeAxes();
  },
  vector(x,y){const i=this.cols?.[Math.round(x)],j=this.rows?.[Math.round(y)];if(i==null||j==null||i<0||j<0||!current?.v)return null;const k=j*model.geometry.nx+i;const u=current.u[k],v=current.v[k];return Number.isFinite(u)&&Number.isFinite(v)?[u,v,this.lats[Math.round(y)]]:null;},
  drawArrows(){
    const ctx=this.canvas.getContext('2d');if(model.geometry?.native||!$('arrows').checked||!current?.v)return;
    ctx.strokeStyle='#f3fbffe0';ctx.lineWidth=.65;
    for(let y=36;y<this.height;y+=29)for(let x=25;x<this.width;x+=29){const v=this.vector(x,y);if(!v)continue;const speed=Math.hypot(v[0],v[1]);if(speed<.5)continue;
      const a=Math.atan2(-v[1],v[0]),len=Math.min(12,5+speed*.16),dx=Math.cos(a)*len,dy=Math.sin(a)*len;
      ctx.beginPath();ctx.moveTo(x-dx/2,y-dy/2);ctx.lineTo(x+dx/2,y+dy/2);for(const t of [-.55,.55]){ctx.moveTo(x+dx/2,y+dy/2);ctx.lineTo(x+dx/2-Math.cos(a+t)*3,y+dy/2-Math.sin(a+t)*3);}ctx.stroke();
    }
  },
  animate(now){
    this.raf=requestAnimationFrame(this.animate);
    if(now-this.last<32)return;const dt=Math.min(.06,(now-this.last)/1000);this.last=now;
    const ctx=this.flow.getContext('2d');
    if(model.geometry?.native||!current?.v||!$('flow').checked||matchMedia('(prefers-reduced-motion: reduce)').matches){ctx.clearRect(0,0,this.width,this.height);return;}
    ctx.globalCompositeOperation='destination-in';ctx.fillStyle='rgba(0,0,0,.90)';ctx.fillRect(0,0,this.width,this.height);ctx.globalCompositeOperation='source-over';ctx.strokeStyle='#edfaffad';ctx.lineWidth=.7;ctx.beginPath();
    const scale=256*2**map.getZoom()/40075016*16000/2;
    for(const p of this.particles||[]){const v=this.vector(p.x,p.y);if(!v||p.age++>100||p.x<0||p.y<0||p.x>=this.width||p.y>=this.height){p.x=Math.random()*this.width;p.y=Math.random()*this.height;p.age=0;continue;}
      const stretch=Math.max(.18,Math.cos(v[2]*Math.PI/180)),dx=v[0]*scale*dt/stretch,dy=-v[1]*scale*dt/stretch;
      ctx.moveTo(p.x,p.y);p.x+=dx;p.y+=dy;ctx.lineTo(p.x,p.y);
    }ctx.stroke();
  }
});
const weather=new Weather().addTo(map);
let viewGeometryKey='';
function fitNative(){
  const g=model.geometry;if(!g?.native)return;
  map.fitBounds([[.5,-.5],[-g.ny+.5,g.nx-.5]],{animate:false,paddingTopLeft:[matchMedia('(max-width:760px)').matches?40:145,225],paddingBottomRight:[105,135]});
}
function configureGeometry(g){
  const key=JSON.stringify([g.native,g.nx,g.ny,g.lat0,g.lon0,g.dx,g.dy]);if(key===viewGeometryKey)return;viewGeometryKey=key;
  current=null;point=null;$('probe').hidden=true;if(marker){map.removeLayer(marker);marker=null;}
  basemap('none');model.geometry=g;map.options.zoomSnap=g.native?0:1;map.options.zoomDelta=g.native ? 0.5 : 1;map.options.crs=g.native?L.CRS.Simple:L.CRS.EPSG3857;map.setMinZoom(g.native?-8:2);
  document.body.classList.toggle('native-grid',g.native);$('nativeNotice').hidden=!g.native;$('nativeAxes').hidden=!g.native;
  for(const id of ['basemap','flow','arrows']){$(id).disabled=g.native;$(id).title=g.native?'Unavailable in native-grid view':'';}
  $('home').title=$('world').title=g.native?'Fit native grid':'Reset map';
  $('home').setAttribute('aria-label',g.native?'Fit native grid':'Return to Europe');$('world').setAttribute('aria-label',g.native?'Fit native grid':'Show the world');
  scaleControl.remove();
  if(g.native){
    map.setView([0,0],0,{reset:true});fitNative();
    if(query.has('grid_row')&&query.has('grid_column')){const row=Number(query.get('grid_row')),col=Number(query.get('grid_column')),zoom=Number(query.get('zoom'));if(Number.isFinite(row)&&Number.isFinite(col)&&Number.isFinite(zoom))map.setView([-row,col],Math.max(-8,Math.min(10,zoom)),{reset:true});}
  }else{map.setView([46,5],4,{reset:true});scaleControl.addTo(map);basemap($('basemap').value);}
  $('resolution').textContent=g.native?`NATIVE GRID · ${g.nx} × ${g.ny}`:`${Math.abs(g.dx).toFixed(2)}° GRID · ${g.nx} × ${g.ny}`;
  updateNativeAxes();
}
function updateNativeAxes(){
  const g=model.geometry;if(!g?.native)return;
  for(const [id,horizontal,n] of [['columnAxis',true,g.nx],['rowAxis',false,g.ny]]){
    const axis=$(id),rect=axis.getBoundingClientRect(),labels=[];
    for(let j=0;j<=5;j++){
      const ll=map.containerPointToLatLng(horizontal?[rect.left+rect.width*j/5,0]:[0,rect.top+rect.height*j/5]);
      const value=Math.round(horizontal?ll.lng:-ll.lat);if(value<0||value>=n||labels.some(e=>e.textContent===String(value)))continue;
      const pixel=map.latLngToContainerPoint(horizontal?[0,value]:[-value,0]);
      const span=document.createElement('span');span.textContent=value;span.style[horizontal?'left':'top']=((horizontal?pixel.x-rect.left:pixel.y-rect.top)/(horizontal?rect.width:rect.height)*100)+'%';labels.push(span);
    }axis.replaceChildren(...labels);
  }
}
function find(name,level){return views.find(v=>v.name===name&&(level===undefined||v.level===String(level)));}
function currentViews(){
  if(custom)return [custom];
  const lev=$('level').value;
  if(field==='wind')return lev==='surface'?[find('10u',10),find('10v',10)]:[find('u',lev),find('v',lev)];
  if(field==='temperature')return [lev==='surface'?find('2t',2):find('t',lev)];
  return [find(field==='rain'?'tp':'tcc')];
}
function updateFrames(keepTime){
  const vs=currentViews();frames=field==='wind'&&!custom?pairWind(vs[0],vs[1]):vs[0]?.frames||[];
  if(!frames.length){tell('No matching fields at this level and valid time.',true);current=null;weather.render();return false;}
  const desired=keepTime??frames[0].ms+Number(query.get('lead')||120)*3600000;
  index=frames.reduce((best,f,i)=>Math.abs(f.ms-desired)<Math.abs(frames[best].ms-desired)?i:best,0);
  $('time').max=frames.length-1;$('time').value=index;$('time').disabled=false;
  $('timeTicks').replaceChildren(...Array.from({length:6},(_,n)=>{
    const i=Math.round(n*(frames.length-1)/5),el=document.createElement('span');
    el.textContent=new Date(frames[i].ms).toLocaleDateString('en-GB',{day:'2-digit',month:'short',timeZone:'UTC'});
    el.style.left=(100*i/Math.max(1,frames.length-1))+'%';el.style.transform=n===0?'none':n===5?'translateX(-100%)':'translateX(-50%)';return el;
  }));
  return true;
}
function configForView(v){return displayConfiguration(v);}

function setHeadings(){
  if(!custom&&['wind','temperature'].includes(field)){
    const surface=field==='wind'?find('10u',10)&&find('10v',10):find('2t',2);
    const surfaceOption=[...$('level').options].find(o=>o.value==='surface');if(surfaceOption)surfaceOption.disabled=!surface;
    if($('level').value==='surface'&&!surface){const preferred=[...$('level').options].find(o=>o.value==='850')||[...$('level').options].find(o=>o.value!=='surface');if(preferred)$('level').value=preferred.value;}
  }
  activeConfig=custom?configForView(custom):field==='wind'?{...FIELDS.wind}:configForView(currentViews()[0]);
  if(!custom)activeConfig.description=FIELDS[field].description;
  if(activeConfig.unit==='°C'&&((custom?.kind==='isobaricInhPa')||(!custom&&$('level').value!=='surface'))){activeConfig.range=[-85,35];activeConfig.ticks=[-80,-40,0,35];}
  $('fieldTitle').textContent=activeConfig.title;
  const level=$('level').value==='surface'?'Near surface':$('level').value+' hPa';
  $('fieldDescription').textContent=custom?`${custom.name} · ${custom.kind} ${custom.level}${custom.member?' · member '+custom.member:''}`:activeConfig.description+' · '+(field==='rain'?'since initialization':field==='cloud'?'entire atmosphere':level);
  for(const b of document.querySelectorAll('[data-field]'))b.classList.toggle('active',!custom&&b.dataset.field===field);
  $('mobileParameter').value=custom?'field:'+custom.key:field;
  if(custom?.kind==='isobaricInhPa'){$('level').value=custom.level;}
  $('level').disabled=custom?custom.kind!=='isobaricInhPa':!['wind','temperature'].includes(field);
  for(const b of document.querySelectorAll('[data-variable]'))b.classList.toggle('active',Boolean(custom)&&b.dataset.variable===custom.name);updateLegend();
}
async function render(){
  if(!frames.length)return;const ticket=++sequence;controller?.abort();controller=new AbortController();const signal=controller.signal;
  const f=frames[index],viewKey=custom?.key||field+'|'+$('level').value;
  loading=true;$('exportField').disabled=true;clearTimeout(noticeTimer);
  // Keep the previous frame and its timestamp visible during same-layer playback.
  // A new layer is cleared so an old field cannot appear under its new legend.
  if(current?.viewKey!==viewKey){current=null;weather.render();$('probe').hidden=true;}
  noticeTimer=setTimeout(()=>{if(ticket===sequence)tell(`Reading ${activeConfig.title.toLowerCase()} · ${date(f.ms)}…`);},350);
  try{
    const geometry=model.geometryForGroup(f.group);
    if(f.v&&!compatibleGeometry(geometry,model.geometryForGroup(f.v.group)))throw new Error('U and V do not share the same grid. Select a scalar field.');
    const u=await model.getGrid(f.group,f.frame,signal);const v=f.v?await model.getGrid(f.v.group,f.v.frame,signal):null;
    if(ticket!==sequence)return;
    configureGeometry(geometry);current={u,v,ms:f.ms,viewKey,record:f.record};
    loading=false;clearTimeout(noticeTimer);$('time').value=index;
    if(activeConfig.auto){let min=Infinity,max=-Infinity;for(const x of u)if(Number.isFinite(x)){const v=x*activeConfig.factor+activeConfig.offset;min=Math.min(min,v);max=Math.max(max,v);}if(!Number.isFinite(min))throw new Error('This field has no finite data.');if(activeConfig.symmetric){const m=Math.max(Math.abs(min),Math.abs(max),1e-12);activeConfig.range=[-m,m];}else activeConfig.range=[min,max>min?max:min+1];updateLegend();}
    $('validTime').textContent=date(f.ms);
    const r=f.record,dd=String(r.dataDate),hh=String(r.dataTime||'0').padStart(4,'0');
    const init=Date.parse(`${dd.slice(0,4)}-${dd.slice(4,6)}-${dd.slice(6,8)}T${hh.slice(0,2)}:${hh.slice(2,4)}:00Z`);
    $('lead').textContent=Number.isFinite(init)?`+${Math.round((f.ms-init)/3600000)} h`:'';
    $('runInfo').textContent=Number.isFinite(init)?'Initialized '+date(init):'IFS operational forecast';
    if(field==='rain'&&!custom)$('runInfo').textContent+=' · accumulation '+r.stepRange+' h';
    $('exportField').disabled=exportBusy;weather.render();if(point)probe(point);$('message').classList.add('ready');
    // One next-time compressed GOP only. No bulk prefetch or whole-file download.
    const next=frames[index+1];if(next)setTimeout(async()=>{try{signal.throwIfAborted();await model.packets(next.group,next.frame,signal);if(next.v)await model.packets(next.v.group,next.v.frame,signal);}catch{}},150);
    if(playing)timer=setTimeout(()=>step(1),1000);
  }catch(e){if(ticket!==sequence)return;loading=false;clearTimeout(noticeTimer);if(e.name==='AbortError')return;tell(e.message,true);stop();}
}
function stop(){playing=false;clearTimeout(timer);$('play').textContent='▶';$('play').setAttribute('aria-label','Play forecast');}
function step(delta){clearTimeout(timer);index=(index+delta+frames.length)%frames.length;render();}
function changeLayer(next){closeMobilePanel();stop();const t=frames[index]?.ms;field=next;custom=null;$('allFields').value='';setHeadings();if(updateFrames(t))render();}
let marker=null;
function probe(ll){
  if(!current)return;point=ll;const k=gridIndex(model.geometry,ll.lat,ll.lng);const raw=k>=0?current.u[k]:NaN,other=k>=0?current.v?.[k]:NaN;
  let value=current.v?Math.hypot(raw,other):raw;value=value*activeConfig.factor+activeConfig.offset;
  $('probeCoords').textContent=model.geometry.native?(k<0?'Outside native grid':`Column ${k%model.geometry.nx} · row ${Math.floor(k/model.geometry.nx)}`):`${ll.lat.toFixed(2)}° · ${(((ll.lng+180)%360+360)%360-180).toFixed(2)}°`;
  const number=document.createElement('span'),unit=document.createElement('span');number.textContent=formatReading(value,activeConfig);unit.className='reading-unit';unit.textContent=Number.isFinite(value)?activeConfig.unit:'';$('probeValue').replaceChildren(number,unit);
  const direction=current.v&&!model.geometry.native?((Math.atan2(-raw,-other)*180/Math.PI+360)%360):null;
  $('probeDetail').textContent=current.v&&!model.geometry.native&&Number.isFinite(value)?`From ${direction.toFixed(0)}° · U ${raw.toFixed(2)}, V ${other.toFixed(2)} m/s`:(model.geometry.native?'Stored raster cell · ':'Nearest native grid point · ')+date(current.ms);
  $('probe').hidden=false;if(marker)map.removeLayer(marker);marker=L.circleMarker(ll,{radius:5,weight:2,color:'white',fillColor:'#142c3b',fillOpacity:1}).addTo(map);
}
async function open(){
  stop();++sequence;clearTimeout(noticeTimer);loading=true;$('exportField').disabled=true;controller?.abort();loadController?.abort();loadController=new AbortController();const signal=loadController.signal;
  frames=[];current=null;weather.render();for(const id of ['level','allFields','mobileParameter'])$(id).disabled=true;document.querySelectorAll('.field').forEach(b=>b.disabled=true);tell('Reading the forecast index and grid…');$('connect').disabled=true;$('datasetSelect').disabled=true;$('time').disabled=true;
  try{
    await model.queue;signal.throwIfAborted();await model.open(new URL($('dataset').value,location.href).href,signal,network);catalogue=await cataloguePromise;signal.throwIfAborted();views=buildInventory(model.groups,catalogue||{});
    const ids=[...new Set(views.flatMap(v=>v.frames.map(f=>f.group)))];supported=new Map(await Promise.all(ids.map(async g=>[g.id,await model.supported(g)])));
    signal.throwIfAborted();
    const usable=v=>v&&v.frames.every(f=>supported.get(f.group.id));
    const windLevels=views.filter(v=>v.name==='u'&&usable(v)&&usable(find('v',v.level))).map(v=>v.level);
    const tempLevels=views.filter(v=>v.name==='t'&&usable(v)).map(v=>v.level);
    const levels=[...new Set([...windLevels,...tempLevels,...views.filter(v=>v.kind==='isobaricInhPa'&&usable(v)).map(v=>v.level)])].sort((a,b)=>Number(b)-Number(a));
    $('level').replaceChildren(...['surface',...levels].map(v=>{const o=document.createElement('option');o.value=v;o.textContent=v==='surface'?'Surface / 10 m':v+' hPa';return o;}));
    $('allFields').replaceChildren(new Option('Choose a variable…',''),...views.sort((a,b)=>a.name.localeCompare(b.name)||Number(b.level)-Number(a.level)).map(v=>{const o=new Option(`${v.variable?.title||v.name} (${v.name}) · ${v.level} · ${v.variable?.display.unit||'unit unknown'}${v.member?' · member '+v.member:''}${usable(v)?'':' · codec unsupported'}`,v.key);o.disabled=!usable(v);return o;}));$('allFields').disabled=false;
    const quick=document.createElement('optgroup');quick.label='Weather layers';
    for(const [value,label] of [['wind','Wind'],['temperature','Temperature'],['rain','Precipitation'],['cloud','Cloud cover']])quick.append(new Option(label,value));
    const all=document.createElement('optgroup');all.label='All variables & levels';
    for(const o of [...$('allFields').options].slice(1)){const copy=o.cloneNode(true);copy.value='field:'+o.value;all.append(copy);}
    $('mobileParameter').replaceChildren(quick,all);$('mobileParameter').disabled=false;
    for(const b of document.querySelectorAll('[data-field]')){const name={temperature:'2t',rain:'tp',cloud:'tcc'}[b.dataset.field];b.disabled=b.dataset.field==='wind'?!((usable(find('10u',10))&&usable(find('10v',10)))||windLevels.length):b.dataset.field==='temperature'?!(usable(find(name))||tempLevels.length):!usable(find(name));b.title=b.disabled?'This dataset/browser does not expose a supported codec for this layer.':'';}
    for(const o of $('mobileParameter').options)if(!o.value.startsWith('field:'))o.disabled=document.querySelector(`[data-field=${o.value}]`).disabled;
    document.querySelectorAll('[data-variable]').forEach(b=>b.remove());
    for(const [names,icon,label] of [[['r','q'],'◌','Humidity'],[['sp','msl'],'◴','Pressure'],[['gh','z'],'≋','Height'],[['vo'],'⟳','Vorticity']]){
      const name=names.find(n=>views.some(v=>v.name===n&&usable(v)));if(!name)continue;
      const b=document.createElement('button');b.className='field';b.dataset.variable=name;b.title=label;b.setAttribute('aria-label',label);
      const i=document.createElement('span');i.textContent=icon;const t=document.createElement('small');t.textContent=label;b.append(i,t);
      b.onclick=()=>{const candidates=views.filter(v=>v.name===name&&usable(v));const v=candidates.find(v=>v.level===$('level').value)||candidates.find(v=>v.level==='850')||candidates[0];$('allFields').value=v.key;$('allFields').onchange();};
      document.querySelector('.field-buttons').append(b);
    }
    configureGeometry(model.geometry);
    $('datasetInfo').textContent=`${bytes(model.fileSizeBytes)} · ${model.videoTracks.length} streams · ${views.length} field/level combinations${catalogueError?' · '+catalogueError:''}`;
    const selected=query.get('field');custom=views.find(v=>v.key===selected&&usable(v))||null;
    field=FIELDS[query.get('layer')]?query.get('layer'):'wind';
    if(document.querySelector(`[data-field=${field}]`).disabled)field='wind';
    if(!custom&&document.querySelector('[data-field=wind]').disabled)custom=views.find(usable);
    if([...$('level').options].some(o=>o.value===query.get('level')))$('level').value=query.get('level');
    const lat=Number(query.get('lat')),lon=Number(query.get('lon')),zoom=Number(query.get('zoom'));
    if(!model.geometry.native&&query.has('lat')&&query.has('lon')&&Number.isFinite(lat)&&Number.isFinite(lon))map.setView([Math.max(-80,Math.min(80,lat)),lon],Math.max(2,Math.min(10,zoom||4)));
    document.querySelector('.tools-link').href='./viewer.html?dataset='+encodeURIComponent($('dataset').value);
    if(!custom&&document.querySelector('[data-field=wind]').disabled)throw new Error('No browser-decodable field found. Use AV1/HEVC profiles with supported bit depths, or the native AVENC tools.');
    if(custom)$('allFields').value=custom.key;
    setHeadings();if(updateFrames(query.get('time')?Number(query.get('time')):undefined))await render();
  }catch(e){if(e.name!=='AbortError'){loading=false;tell(e.message,true);}}finally{if(!signal.aborted){$('connect').disabled=false;$('datasetSelect').disabled=false;syncDatasetSelection();}}
}
for(const b of document.querySelectorAll('[data-field]'))b.onclick=()=>changeLayer(b.dataset.field);
$('mobileParameter').onchange=()=>{const v=$('mobileParameter').value;if(v.startsWith('field:')){$('allFields').value=v.slice(6);$('allFields').onchange();}else changeLayer(v);};
$('level').onchange=()=>{if(custom){const v=views.find(v=>v.name===custom.name&&v.kind===custom.kind&&v.level===$('level').value&&v.member===custom.member);if(!v){$('level').value=custom.level;return;}custom=v;$('allFields').value=v.key;$('mobileParameter').value='field:'+v.key;}closeMobilePanel();const t=frames[index]?.ms;stop();setHeadings();if(updateFrames(t))render();};
$('allFields').onchange=()=>{if(!$('allFields').value)return;closeMobilePanel();stop();const t=frames[index]?.ms;custom=views.find(v=>v.key===$('allFields').value);setHeadings();if(updateFrames(t))render();};
$('time').oninput=()=>{stop();index=Number($('time').value);render();};$('previous').onclick=()=>{stop();step(-1);};$('next').onclick=()=>{stop();step(1);};
$('play').onclick=()=>{if(playing)stop();else if(frames.length){playing=true;$('play').textContent='Ⅱ';$('play').setAttribute('aria-label','Pause forecast');step(1);}};
$('opacity').oninput=()=>weather.render();$('arrows').onchange=()=>weather.render();$('flow').onchange=()=>weather.render();$('basemap').onchange=()=>basemap($('basemap').value);
$('home').onclick=()=>model.geometry?.native?fitNative():map.setView([46,5],4);$('world').onclick=()=>model.geometry?.native?fitNative():map.setView([20,0],2);
$('closeProbe').onclick=()=>{point=null;$('probe').hidden=true;if(marker)map.removeLayer(marker);marker=null;};map.on('click',e=>probe(e.latlng));
$('connect').onclick=open;$('dataset').onkeydown=e=>{if(e.key==='Enter')open();};
$('share').onclick=async()=>{const u=new URL(location.href);u.searchParams.set('layer',field);u.searchParams.set('level',$('level').value);if(model.geometry.native){u.searchParams.delete('lat');u.searchParams.delete('lon');u.searchParams.set('grid_row',(-map.getCenter().lat).toFixed(3));u.searchParams.set('grid_column',map.getCenter().lng.toFixed(3));}else{u.searchParams.delete('grid_row');u.searchParams.delete('grid_column');u.searchParams.set('lat',map.getCenter().lat.toFixed(4));u.searchParams.set('lon',map.getCenter().lng.toFixed(4));}u.searchParams.set('zoom',map.getZoom());u.searchParams.delete('field');u.searchParams.set('dataset',$('dataset').value);if(frames[index])u.searchParams.set('time',frames[index].ms);if(custom)u.searchParams.set('field',custom.key);try{await navigator.clipboard.writeText(u.href);tell('View link copied');setTimeout(()=>$('message').classList.add('ready'),1800);}catch{tell('Copy this URL: '+u.href);}};
document.addEventListener('keydown',e=>{if(/INPUT|SELECT|TEXTAREA|BUTTON/.test(e.target.tagName))return;if(e.key==='ArrowRight'){e.preventDefault();stop();step(1);}if(e.key==='ArrowLeft'){e.preventDefault();stop();step(-1);}if(e.code==='Space'){e.preventDefault();$('play').click();}});
document.addEventListener('visibilitychange',()=>{if(document.hidden)stop();});
function closeMobilePanel(){if(matchMedia('(max-width:760px)').matches){$('layersPanel').classList.remove('open');$('panelToggle').setAttribute('aria-expanded','false');}closeOptions();}
$('about').onclick=()=>$('aboutDialog').showModal();$('closeAbout').onclick=()=>$('aboutDialog').close();
$('panelToggle').onclick=()=>{const open=$('layersPanel').classList.toggle('open');$('panelToggle').setAttribute('aria-expanded',String(open));if(!open)closeOptions();};
$('collapseRail').onclick=()=>{const collapsed=$('layersPanel').classList.toggle('collapsed');$('collapseRail').textContent=collapsed?'☰':'‹';$('collapseRail').setAttribute('aria-expanded',String(!collapsed));$('collapseRail').setAttribute('aria-label',collapsed?'Show weather controls':'Collapse weather controls');if(collapsed)closeOptions();};
function closeOptions(){$('railOptions').hidden=true;$('moreFields').setAttribute('aria-expanded','false');}
$('moreFields').onclick=()=>{const visible=$('railOptions').hidden;$('railOptions').hidden=!visible;$('moreFields').setAttribute('aria-expanded',String(visible));};$('closeOptions').onclick=closeOptions;
map.on('click',closeMobilePanel);
$('datasetSelect').onchange=()=>{if(!$('datasetSelect').value)return;$('dataset').value=$('datasetSelect').value;closeMobilePanel();open();};
function syncDatasetSelection(){
  const url=$('dataset').value;
  if(![...$('datasetSelect').options].some(o=>o.value===url))$('datasetSelect').append(new Option('Custom · '+url.split('/').pop(),url));
  $('datasetSelect').value=url;
}
async function start(){
  try{const response=await fetch('./datasets/dataset.json');if(!response.ok)throw new Error('Dataset list unavailable');const c=await response.json();if(c.schema!=='ecflix.datasets.v1'||!Array.isArray(c.datasets))throw new Error('Invalid dataset list');datasetCatalogue=c;
    $('datasetSelect').replaceChildren(...c.datasets.map(d=>new Option(d.label||d.url,d.url)));
  }catch(e){console.warn(e.message);$('datasetSelect').replaceChildren();}
  $('dataset').value=query.get('dataset')||datasetCatalogue?.default||'./dataset/20261021.browser_svt10.mp4';syncDatasetSelection();await open();
}
function downloadName(){const name=(custom?.name||field).replace(/[^a-z0-9_-]/gi,'_');return `ecflix_${name}_${new Date(current.ms).toISOString().replace(/[-:]/g,'').slice(0,13)}.npz`;}
function pythonExample(){const file=current?downloadName():'ecflix_field.npz';return `import json
import numpy as np
import matplotlib.pyplot as plt

with np.load("${file}", allow_pickle=False) as d:
    field = d["field"]  # Float32, displayed units
    meta = json.loads(d["metadata_json"].tobytes())
    native = meta["geometry_mode"] == "native"
    x = d["column"] if native else d["longitude"]
    y = d["row"] if native else d["latitude"]

plt.pcolormesh(x, y, field, shading="nearest")
if native:
    plt.gca().invert_yaxis()  # stored row 0 at the top
plt.colorbar(label=meta["units"])
plt.title(meta["variable"] + " · " + meta["valid_time"])
plt.xlabel("Grid column" if native else "Longitude")
plt.ylabel("Grid row" if native else "Latitude")
plt.show()
# Wind archives also contain native u and v (m/s).
# Preview values are highly compressed, not reference data.`;}
function showDownload(){
  if(!current||loading)return;stop();
  const f=frames[index],selected=currentViews(),parts=[f,...(f.v?[f.v]:[])];
  const variables=parts.map((p,i)=>({name:selected[i].name,stream:p.group.stream_index,frame:p.frame}));
  const wholeViews=views.filter(v=>selected.some(p=>p.name===v.name&&p.rawCode===v.rawCode&&(p.name!=='unknown'||p.rawCode||v===p)));
  const wholeGroups=[...new Set(wholeViews.flatMap(v=>v.frames.map(f=>f.group)))];
  const examples=accessExamples({url:new URL($('dataset').value,location.href).href,variables,wholeGroups,unit:activeConfig.unit,factor:activeConfig.factor,offset:activeConfig.offset,native:model.geometry.native});
  $('cliExample').textContent=examples.cli;$('xarrayExample').textContent=examples.python;$('pythonExample').textContent=pythonExample();
  $('downloadContext').textContent=activeConfig.title+' · '+(custom?custom.kind+' '+custom.level:$('level').selectedOptions[0].textContent)+' · '+date(current.ms);
  $('exportStatus').textContent='';$('pythonDownload').disabled=exportBusy;$('pythonDialog').showModal();
}
$('exportField').onclick=showDownload;$('closePython').onclick=()=>$('pythonDialog').close();
for(const [button,code,label] of [['copyCli','cliExample','Command'],['copyXarray','xarrayExample','Python example'],['copyPython','pythonExample','NumPy example']])$(button).onclick=async()=>{try{await navigator.clipboard.writeText($(code).textContent);$('exportStatus').textContent=label+' copied.';}catch{$('exportStatus').textContent='Select and copy the example above.';}};
async function exportCurrent(){
  if(!current||loading||exportBusy)return;exportBusy=true;stop();const snapshot=current,config={...activeConfig},geometry={...model.geometry},name=downloadName();
  const metadata={variable:custom?.name||(snapshot.v?'wind_speed':field),title:config.title,level:custom?.level||$('level').value,type_of_level:custom?.kind||($('level').value==='surface'?'surface':'isobaricInhPa'),valid_time:new Date(snapshot.ms).toISOString(),source_url:new URL($('dataset').value,location.href).href,grib_record:snapshot.record};
  $('exportField').disabled=true;$('pythonDownload').disabled=true;$('exportStatus').textContent='Preparing compressed Float32 archive…';
  try{
    await new Promise(r=>setTimeout(r,20));
    const files=fieldArchive({u:snapshot.u,v:snapshot.v,geometry,config,metadata});
    const archive=await new Promise((resolve,reject)=>zip(files,{level:6},(error,data)=>error?reject(error):resolve(data)));
    const url=URL.createObjectURL(new Blob([archive],{type:'application/zip'})),a=document.createElement('a');a.href=url;a.download=name;document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(url),60000);
    $('exportStatus').textContent='Downloaded '+name+' · '+bytes(archive.length);tell('NPZ downloaded · '+bytes(archive.length));setTimeout(()=>$('message').classList.add('ready'),2200);
  }catch(e){$('exportStatus').textContent=e.message;tell('Export failed: '+e.message,true);}finally{exportBusy=false;$('exportField').disabled=!current||loading;$('pythonDownload').disabled=!current||loading||exportBusy;}
}
$('pythonDownload').onclick=exportCurrent;
updateLegend();
// Read-only diagnostics for reproducible local checks; no synthetic weather.
window.avencMet={model,map,get state(){return {field,custom:custom?.key,index,frames:frames.length,ready:Boolean(current)&&!loading,loading,playing,validTime:current?.ms,native:Boolean(model.geometry?.native)};}};
start();
