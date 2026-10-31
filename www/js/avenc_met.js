import { zip } from 'https://cdn.jsdelivr.net/npm/fflate@0.8.2/esm/browser.js';
import { accessExamples } from './avenc_access.js';
import { fieldArchive } from './avenc_npz.js';
import { EmitterSimulation, sampleVelocity, wrapLon } from './avenc_trajectories.js';
import { TracerOverlay } from './avenc_tracer_overlay.js';
import { AvencRangeModel } from './avenc_range_model.js';
import { buildInventory, pairWind, gridIndex, displayConfiguration, formatReading, compatibleGeometry } from './avenc_met_core.js';
const $=id=>document.getElementById(id),query=new URLSearchParams(location.search);
let datasetCatalogue=null,exportBusy=false,globe=null,globeMode=false,viewBusy=false,initialViewApplied=false;
let catalogue=null,catalogueError='';
const cataloguePromise=fetch(new URL('../data/ifs_variables.json',import.meta.url)).then(async r=>{if(!r.ok)throw new Error('Variable catalogue unavailable');const c=await r.json();if(c.schema!=='ecflix.variables.v1')throw new Error('Invalid variable catalogue');return c;}).catch(e=>{catalogueError=e.message;return null;});
const model=new AvencRangeModel(),map=L.map('map',{zoomControl:false,worldCopyJump:false,minZoom:2,maxZoom:10}).setView([46,5],4);
L.control.zoom({position:'topright'}).addTo(map);const scaleControl=L.control.scale({position:'bottomleft',imperial:false}).addTo(map);
map.createPane('weather');map.getPane('weather').style.zIndex=420;
let base,labels;
function basemap(kind){
  if(base)map.removeLayer(base);if(labels)map.removeLayer(labels);base=labels=null;
  if(kind==='none'||model.geometry?.native||globeMode)return;
  const attribution='© <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors';
  map.getPane('tilePane').classList.toggle('dark-base',kind==='dark');
  base=L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{attribution,maxZoom:19}).addTo(map);
}
// Wait for grid classification before requesting geographic background tiles.
const PALETTES={signed:[[45,70,133],[63,138,180],[165,210,222],[232,239,232],[245,190,128],[217,109,76],[153,45,77]],wind:[[0,46,81],[28,113,154],[42,169,171],[122,202,151],[224,208,107],[246,147,78],[210,66,100],[126,56,142]],temperature:[[71,60,129],[52,121,175],[85,188,185],[183,218,151],[248,216,112],[242,139,83],[191,60,74]],rain:[[31,68,94],[46,119,159],[56,191,181],[140,205,107],[245,215,87],[234,124,71],[172,55,110]],cloud:[[28,52,73],[67,94,113],[117,141,155],[180,197,204],[238,245,246]]};
const FIELDS={currents:{title:'Surface currents',description:'Sea surface speed & direction',unit:'m/s',range:[0,3],ticks:[0,.5,1,2,3],palette:'wind',factor:1,offset:0,motionScale:20},wind:{title:'Wind',description:'Wind speed & direction',unit:'m/s',range:[0,40],ticks:[0,10,20,30,40],palette:'wind',factor:1,offset:0},temperature:{title:'Temperature',description:'Air temperature',unit:'°C',range:[-30,40],ticks:[-30,-10,10,30,40],palette:'temperature',factor:1,offset:-273.15},rain:{title:'Precipitation',description:'Total accumulated precipitation',unit:'mm',range:[0,100],ticks:[0,1,10,50,100],palette:'rain',factor:1000,offset:0,log:true},cloud:{title:'Cloud cover',description:'Total cloud cover',unit:'%',range:[0,100],ticks:[0,25,50,75,100],palette:'cloud',factor:100,offset:0}};
let views=[],supported=new Map(),frames=[],index=0,field='wind',custom=null,activeConfig=FIELDS.wind,current=null,controller=null,loadController=null,sequence=0,playing=false,playDirection=1,timer=null,point=null,loading=false,noticeTimer=null;
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
    if(globeMode){updateGlobe();return;}
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
    for(let y=36;y<this.height;y+=29)for(let x=25;x<this.width;x+=29){const v=this.vector(x,y);if(!v)continue;const speed=Math.hypot(v[0],v[1]);if(speed*(activeConfig.motionScale||1)<.5)continue;
      const a=Math.atan2(-v[1],v[0]),len=Math.min(12,5+speed*(activeConfig.motionScale||1)*.16),dx=Math.cos(a)*len,dy=Math.sin(a)*len;
      ctx.beginPath();ctx.moveTo(x-dx/2,y-dy/2);ctx.lineTo(x+dx/2,y+dy/2);for(const t of [-.55,.55]){ctx.moveTo(x+dx/2,y+dy/2);ctx.lineTo(x+dx/2-Math.cos(a+t)*3,y+dy/2-Math.sin(a+t)*3);}ctx.stroke();
    }
  },
  animate(now){
    this.raf=requestAnimationFrame(this.animate);if(globeMode)return;
    if(now-this.last<32)return;const dt=Math.min(.06,(now-this.last)/1000);this.last=now;
    const ctx=this.flow.getContext('2d');
    if(model.geometry?.native||!current?.v||!$('flow').checked||matchMedia('(prefers-reduced-motion: reduce)').matches){ctx.clearRect(0,0,this.width,this.height);return;}
    ctx.globalCompositeOperation='destination-in';ctx.fillStyle='rgba(0,0,0,.90)';ctx.fillRect(0,0,this.width,this.height);ctx.globalCompositeOperation='source-over';ctx.strokeStyle='#edfaffad';ctx.lineWidth=.7;ctx.beginPath();
    const scale=256*2**map.getZoom()/40075016*16000/2*(activeConfig.motionScale||1);
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
  if(g.native&&globeMode)setGlobeMode(false);
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
  updateNativeAxes();updateViewButton();
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
// ERA5 GRIB1 identifies 10u/10v/2t as surface level 0; IFS GRIB2 uses
// heightAboveGround 10/2. The short names already identify the physical height.
function surfaceField(name,height){return find(name,height)||views.find(v=>v.name===name&&v.kind==='surface'&&v.level==='0');}
function currentViews(){
  if(custom)return [custom];
  const lev=$('level').value;
  if(field==='wind')return lev==='surface'?[surfaceField('10u',10),surfaceField('10v',10)]:[find('u',lev),find('v',lev)];
  if(field==='currents')return [find('sve'),find('svn')];
  if(field==='temperature')return [lev==='surface'?surfaceField('2t',2):find('t',lev)];
  return [find(field==='rain'?'tp':'tcc')];
}
function updateFrames(keepTime){
  const vs=currentViews();frames=['wind','currents'].includes(field)&&!custom?pairWind(vs[0],vs[1]):vs[0]?.frames||[];
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
    const surface=field==='wind'?surfaceField('10u',10)&&surfaceField('10v',10):surfaceField('2t',2);
    const surfaceOption=[...$('level').options].find(o=>o.value==='surface');if(surfaceOption)surfaceOption.disabled=!surface;
    if($('level').value==='surface'&&!surface){const preferred=[...$('level').options].find(o=>o.value==='850')||[...$('level').options].find(o=>o.value!=='surface');if(preferred)$('level').value=preferred.value;}
  }
  activeConfig=custom?configForView(custom):['wind','currents'].includes(field)?{...FIELDS[field]}:configForView(currentViews()[0]);
  if(!custom)activeConfig.description=FIELDS[field].description;
  if(activeConfig.unit==='°C'&&((custom?.kind==='isobaricInhPa')||(!custom&&$('level').value!=='surface'))){activeConfig.range=[-85,35];activeConfig.ticks=[-80,-40,0,35];}
  $('fieldTitle').textContent=activeConfig.title;
  const level=$('level').value==='surface'?'Near surface':$('level').value+' hPa';
  $('fieldDescription').textContent=custom?`${custom.name} · ${custom.kind} ${custom.level}${custom.member?' · member '+custom.member:''}`:activeConfig.description+' · '+(field==='rain'?'since initialization':field==='cloud'?'entire atmosphere':field==='currents'?'sea surface':level);
  for(const b of document.querySelectorAll('[data-field]'))b.classList.toggle('active',!custom&&b.dataset.field===field);
  $('mobileParameter').value=custom?'field:'+custom.key:field;
  if(custom?.kind==='isobaricInhPa'){$('level').value=custom.level;}
  $('level').disabled=custom?custom.kind!=='isobaricInhPa':!['wind','temperature'].includes(field);
  for(const b of document.querySelectorAll('[data-variable]'))b.classList.toggle('active',Boolean(custom)&&b.dataset.variable===custom.name);updateLegend();
}
async function render(){
  if(!frames.length)return;const ticket=++sequence;controller?.abort();controller=new AbortController();const signal=controller.signal;
  const targetIndex=index,frameList=frames,f=frameList[targetIndex],viewKey=custom?.key||field+'|'+$('level').value;
  loading=true;updateViewButton();updateSourceControls();$('exportField').disabled=true;clearTimeout(noticeTimer);
  // Keep the previous frame and its timestamp visible during same-layer playback.
  // A new layer is cleared so an old field cannot appear under its new legend.
  if(current?.viewKey!==viewKey){current=null;weather.render();$('probe').hidden=true;}
  noticeTimer=setTimeout(()=>{if(ticket===sequence)tell(`Reading ${activeConfig.title.toLowerCase()} · ${date(f.ms)}…`);},350);
  try{
    const geometry=model.geometryForGroup(f.group);
    if(f.v&&!compatibleGeometry(geometry,model.geometryForGroup(f.v.group)))throw new Error('U and V do not share the same grid. Select a scalar field.');
    const u=await model.getGrid(f.group,f.frame,signal);const v=f.v?await model.getGrid(f.v.group,f.v.frame,signal):null;
    if(ticket!==sequence)return;
    if(v&&!geometry.native&&emitters.sources.length){
      clearTimeout(noticeTimer);
      // Decode only the intervening paired times required by physical-time paths.
      const pairs=new Map([[targetIndex,{u,v}]]);
      const loadPair=async i=>{
        signal.throwIfAborted();if(pairs.has(i))return pairs.get(i);const f=frameList[i];
        if(!f?.v||!compatibleGeometry(geometry,model.geometryForGroup(f.group))||!compatibleGeometry(geometry,model.geometryForGroup(f.v.group)))throw Error('Trajectory grids do not match.');
        const pair={u:await model.getGrid(f.group,f.frame,signal),v:await model.getGrid(f.v.group,f.v.frame,signal)};
        pairs.set(i,pair);while(pairs.size>2)pairs.delete(pairs.keys().next().value);return pair;
      };
      await emitters.evaluate(targetIndex,{frames:frameList,geometry,loadPair,signal,onProgress:(source,i)=>{const message=`Tracing S${source.id} · ${date(frameList[i].ms)}`;$('sourceStatus').textContent=message;tell(message+'…');}});
      signal.throwIfAborted();if(ticket!==sequence)return;
    }
    configureGeometry(geometry);current={u,v,ms:f.ms,viewKey,record:f.record};
    loading=false;clearTimeout(noticeTimer);$('time').value=index;
    if(activeConfig.auto){let min=Infinity,max=-Infinity;for(const x of u)if(Number.isFinite(x)){const v=x*activeConfig.factor+activeConfig.offset;min=Math.min(min,v);max=Math.max(max,v);}if(!Number.isFinite(min))throw new Error('This field has no finite data.');if(activeConfig.symmetric){const m=Math.max(Math.abs(min),Math.abs(max),1e-12);activeConfig.range=[-m,m];}else activeConfig.range=[min,max>min?max:min+1];updateLegend();}
    $('validTime').textContent=date(f.ms);
    const r=f.record,dd=String(r.dataDate),hh=String(r.dataTime||'0').padStart(4,'0');
    const init=Date.parse(`${dd.slice(0,4)}-${dd.slice(4,6)}-${dd.slice(6,8)}T${hh.slice(0,2)}:${hh.slice(2,4)}:00Z`);
    $('lead').textContent=Number.isFinite(init)?`+${Math.round((f.ms-init)/3600000)} h`:'';
    $('runInfo').textContent=Number.isFinite(init)?'Initialized '+date(init):'IFS operational forecast';
    if(field==='rain'&&!custom)$('runInfo').textContent+=' · accumulation '+r.stepRange+' h';
    $('exportField').disabled=exportBusy;updateViewButton();updateSourceControls();updateSourceList();weather.render();if(point)probe(point);$('message').classList.add('ready');
    // One next-time compressed GOP only. No bulk prefetch or whole-file download.
    const next=frames[index+(playing?playDirection:1)];if(next)setTimeout(async()=>{try{signal.throwIfAborted();await model.packets(next.group,next.frame,signal);if(next.v)await model.packets(next.v.group,next.v.frame,signal);}catch{}},150);
    if(playing)timer=setTimeout(()=>step(playDirection),1000);
  }catch(e){if(ticket!==sequence)return;loading=false;clearTimeout(noticeTimer);if(e.name==='AbortError')return;tell(e.message,true);stop();updateViewButton();updateSourceControls();}
}
function stop(){playing=false;clearTimeout(timer);for(const [id,symbol,label]of [['play','▶','Play forecast'],['playReverse','◀','Play forecast backward']]){$(id).textContent=symbol;$(id).setAttribute('aria-label',label);$(id).setAttribute('aria-pressed','false');}}
function startPlay(direction){
 if(playing&&playDirection===direction){stop();return;}stop();if(!frames.length)return;
 playDirection=direction;playing=true;const button=$(direction<0?'playReverse':'play');button.textContent='Ⅱ';button.setAttribute('aria-label','Pause forecast');button.setAttribute('aria-pressed','true');step(direction);
}
function step(delta){clearTimeout(timer);if(!frames.length)return;const next=index+delta;if(next<0||next>=frames.length){stop();return;}index=next;render();}
function changeLayer(next){closeMobilePanel();stop();resetSources();const t=frames[index]?.ms;field=next;custom=null;$('allFields').value='';setHeadings();if(updateFrames(t))render();}
let marker=null;
function probe(ll){
  if(!current)return;point=ll;const k=gridIndex(model.geometry,ll.lat,ll.lng);const raw=k>=0?current.u[k]:NaN,other=k>=0?current.v?.[k]:NaN;
  let value=current.v?Math.hypot(raw,other):raw;value=value*activeConfig.factor+activeConfig.offset;
  $('probeCoords').textContent=model.geometry.native?(k<0?'Outside native grid':`Column ${k%model.geometry.nx} · row ${Math.floor(k/model.geometry.nx)}`):`${ll.lat.toFixed(2)}° · ${(((ll.lng+180)%360+360)%360-180).toFixed(2)}°`;
  const number=document.createElement('span'),unit=document.createElement('span');number.textContent=formatReading(value,activeConfig);unit.className='reading-unit';unit.textContent=Number.isFinite(value)?activeConfig.unit:'';$('probeValue').replaceChildren(number,unit);
  const direction=current.v&&!model.geometry.native?((Math.atan2(field==='currents'?raw:-raw,field==='currents'?other:-other)*180/Math.PI+360)%360):null;
  $('probeDetail').textContent=current.v&&!model.geometry.native&&Number.isFinite(value)?`${field==='currents'?'Towards':'From'} ${direction.toFixed(0)}° · U ${raw.toFixed(2)}, V ${other.toFixed(2)} m/s`:(model.geometry.native?'Stored raster cell · ':'Nearest native grid point · ')+date(current.ms);
  if(globe){globe.pickPoint=ll;if(globeMode)globe.draw();}
  $('probe').hidden=false;if(marker)map.removeLayer(marker);marker=L.circleMarker(ll,{radius:5,weight:2,color:'white',fillColor:'#142c3b',fillOpacity:1}).addTo(map);
}
async function open(){
  stop();resetSources();++sequence;clearTimeout(noticeTimer);loading=true;$('exportField').disabled=true;controller?.abort();loadController?.abort();loadController=new AbortController();const signal=loadController.signal;
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
    for(const [value,label] of [['wind','Wind'],['currents','Surface currents'],['temperature','Temperature'],['rain','Precipitation'],['cloud','Cloud cover']])quick.append(new Option(label,value));
    const all=document.createElement('optgroup');all.label='All variables & levels';
    for(const o of [...$('allFields').options].slice(1)){const copy=o.cloneNode(true);copy.value='field:'+o.value;all.append(copy);}
    $('mobileParameter').replaceChildren(quick,all);$('mobileParameter').disabled=false;
    for(const b of document.querySelectorAll('[data-field]')){const name={temperature:'2t',rain:'tp',cloud:'tcc'}[b.dataset.field];b.disabled=b.dataset.field==='currents'?!(usable(find('sve'))&&usable(find('svn'))&&pairWind(find('sve'),find('svn')).length):b.dataset.field==='wind'?!((usable(surfaceField('10u',10))&&usable(surfaceField('10v',10)))||windLevels.length):b.dataset.field==='temperature'?!(usable(find(name))||tempLevels.length):!usable(find(name));b.title=b.disabled?'This dataset/browser does not expose a supported codec for this layer.':'';}
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
    if(document.querySelector(`[data-field=${field}]`).disabled)field=[...document.querySelectorAll('[data-field]')].find(b=>!b.disabled)?.dataset.field||'wind';
    if(!custom&&document.querySelector(`[data-field=${field}]`).disabled)custom=views.find(usable);
    if([...$('level').options].some(o=>o.value===query.get('level')))$('level').value=query.get('level');
    const lat=Number(query.get('lat')),lon=Number(query.get('lon')),zoom=Number(query.get('zoom'));
    if(!model.geometry.native&&query.has('lat')&&query.has('lon')&&Number.isFinite(lat)&&Number.isFinite(lon))map.setView([Math.max(-80,Math.min(80,lat)),lon],Math.max(2,Math.min(10,zoom||4)));
    document.querySelector('.tools-link').href='./viewer.html?dataset='+encodeURIComponent($('dataset').value);
    if(!custom&&document.querySelector(`[data-field=${field}]`).disabled)throw new Error('No browser-decodable field found. Use AV1/HEVC profiles with supported bit depths, or the native AVENC tools.');
    if(custom)$('allFields').value=custom.key;
    setHeadings();if(updateFrames(query.get('time')?Number(query.get('time')):undefined))await render();
    if(!initialViewApplied&&current){initialViewApplied=true;if(query.get('view')==='3d'&&!model.geometry.native){await setGlobeMode(true);if(globeMode)globe.setView(Number(query.get('lat')||46),Number(query.get('lon')||5),Number(query.get('globe_zoom')||1));}}
  }catch(e){if(e.name!=='AbortError'){loading=false;tell(e.message,true);}}finally{if(!signal.aborted){$('connect').disabled=false;$('datasetSelect').disabled=false;syncDatasetSelection();}}
}
for(const b of document.querySelectorAll('[data-field]'))b.onclick=()=>changeLayer(b.dataset.field);
$('mobileParameter').onchange=()=>{const v=$('mobileParameter').value;if(v.startsWith('field:')){$('allFields').value=v.slice(6);$('allFields').onchange();}else changeLayer(v);};
$('level').onchange=()=>{if(custom){const v=views.find(v=>v.name===custom.name&&v.kind===custom.kind&&v.level===$('level').value&&v.member===custom.member);if(!v){$('level').value=custom.level;return;}custom=v;$('allFields').value=v.key;$('mobileParameter').value='field:'+v.key;}closeMobilePanel();const t=frames[index]?.ms;stop();resetSources();setHeadings();if(updateFrames(t))render();};
$('allFields').onchange=()=>{if(!$('allFields').value)return;closeMobilePanel();stop();resetSources();const t=frames[index]?.ms;custom=views.find(v=>v.key===$('allFields').value);setHeadings();if(updateFrames(t))render();};
$('time').oninput=()=>{stop();index=Number($('time').value);render();};$('previous').onclick=()=>{stop();step(-1);};$('next').onclick=()=>{stop();step(1);};
$('play').onclick=()=>startPlay(1);$('playReverse').onclick=()=>startPlay(-1);
$('opacity').oninput=()=>weather.render();$('arrows').onchange=()=>weather.render();$('flow').onchange=()=>weather.render();$('basemap').onchange=()=>{basemap($('basemap').value);weather.render();};
$('home').onclick=()=>globeMode?globe.setView(46,5,1):model.geometry?.native?fitNative():map.setView([46,5],4);$('world').onclick=()=>globeMode?globe.setView(0,0,.9):model.geometry?.native?fitNative():map.setView([20,0],2);
$('closeProbe').onclick=()=>{if(globe){globe.pickPoint=null;if(globeMode)globe.draw();}point=null;$('probe').hidden=true;if(marker)map.removeLayer(marker);marker=null;};map.on('click',e=>handleMapPick(e.latlng));
$('connect').onclick=open;$('dataset').onkeydown=e=>{if(e.key==='Enter')open();};
$('share').onclick=async()=>{const u=new URL(location.href);u.searchParams.set('view',globeMode?'3d':'2d');if(globeMode)u.searchParams.set('globe_zoom',globe.zoom.toFixed(3));else u.searchParams.delete('globe_zoom');u.searchParams.set('layer',field);u.searchParams.set('level',$('level').value);if(model.geometry.native){u.searchParams.delete('lat');u.searchParams.delete('lon');u.searchParams.set('grid_row',(-map.getCenter().lat).toFixed(3));u.searchParams.set('grid_column',map.getCenter().lng.toFixed(3));}else{u.searchParams.delete('grid_row');u.searchParams.delete('grid_column');u.searchParams.set('lat',(globeMode?globe.lat:map.getCenter().lat).toFixed(4));u.searchParams.set('lon',(globeMode?globe.lon:map.getCenter().lng).toFixed(4));}u.searchParams.set('zoom',map.getZoom());u.searchParams.delete('field');u.searchParams.set('dataset',$('dataset').value);if(frames[index])u.searchParams.set('time',frames[index].ms);if(custom)u.searchParams.set('field',custom.key);try{await navigator.clipboard.writeText(u.href);tell('View link copied');setTimeout(()=>$('message').classList.add('ready'),1800);}catch{tell('Copy this URL: '+u.href);}};
document.addEventListener('keydown',e=>{if(/INPUT|SELECT|TEXTAREA|BUTTON|CANVAS/.test(e.target.tagName))return;if(e.key==='ArrowRight'){e.preventDefault();stop();step(1);}if(e.key==='ArrowLeft'){e.preventDefault();stop();step(-1);}if(e.code==='Space'){e.preventDefault();$('play').click();}});
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
function updateViewButton(){
  $('toggle3d').textContent=globeMode?'2D':'3D';$('toggle3d').setAttribute('aria-pressed',String(globeMode));$('toggle3d').disabled=viewBusy||loading||!current||Boolean(model.geometry?.native);
  $('toggle3d').title=model.geometry?.native?'3D requires recognized geographic coordinates':globeMode?'Switch to 2D map':'Switch to 3D globe';
}
function updateGlobe(){
  if(!globe)return;if(!current){globe.clear();return;}
  try{globe.setField(current,model.geometry,activeConfig,palette(activeConfig),{opacity:Number($('opacity').value)/100,flow:$('flow').checked,arrows:$('arrows').checked,basemap:$('basemap').value,rain:field==='rain',motionScale:activeConfig.motionScale||1});}catch(e){if(viewBusy)throw e;setGlobeMode(false);tell(e.message,true);}
}
async function setGlobeMode(enabled){
  if(enabled===globeMode||viewBusy)return;if(enabled&&(!current||model.geometry?.native))return;
  viewBusy=true;updateViewButton();
  try{
    if(enabled){
      if(!globe){const {AvencGlobe}=await import('./avenc_globe.js');if(!current||model.geometry?.native)return;globe=new AvencGlobe($('globe'),{onPick:ll=>{closeMobilePanel();handleMapPick(ll);},onError:message=>{const old=globe;setGlobeMode(false).finally(()=>{old?.destroy();if(globe===old)globe=null;});tell(message,true);}});}
      const centre=map.getCenter();globeMode=true;document.body.classList.add('globe-view');$('globe').hidden=false;$('globeHint').hidden=false;$('globeCredit').hidden=false;basemap('none');
      globe.setView(centre.lat,centre.lng,1);globe.setActive(true);updateGlobe();
      if(point){globe.pickPoint=point;globe.draw();}
    }else{
      globeMode=false;globe?.setActive(false);document.body.classList.remove('globe-view');$('globe').hidden=true;$('globeHint').hidden=true;$('globeCredit').hidden=true;
      if(!model.geometry?.native&&globe)map.setView([Math.max(-85,Math.min(85,globe.lat)),globe.lon],map.getZoom(),{animate:false});
      basemap($('basemap').value);map.invalidateSize();weather.render();
    }
    const light=[...$('basemap').options].find(o=>o.value==='streets');light.textContent=globeMode?'Light globe':'Streets';
  }catch(e){globeMode=false;document.body.classList.remove('globe-view');$('globe').hidden=true;$('globeHint').hidden=true;$('globeCredit').hidden=true;globe?.setActive(false);basemap($('basemap').value);tell(e.message,true);}
  finally{viewBusy=false;updateViewButton();}
}
$('toggle3d').onclick=()=>setGlobeMode(!globeMode);$('globeZoomIn').onclick=()=>globe?.zoomBy(1.25);$('globeZoomOut').onclick=()=>globe?.zoomBy(1/1.25);
const emitters=new EmitterSimulation();let placingSource=false;
function canTrace(){return Boolean(current?.v&&!model.geometry?.native&&!custom&&['wind','currents'].includes(field));}
function updateSourceControls(){
 const enabled=canTrace()&&!loading;$('sourcesToggle').disabled=!enabled;$('placeSource').disabled=!enabled||emitters.sources.length>=6;
 for(const id of ['diffusion','plumeDensity','plumeLifetime'])$(id).disabled=loading;$('clearSources').disabled=!emitters.sources.length;
 if(!canTrace())setPlacing(false);
}
function setPlacing(value){placingSource=value;$('placeSource').setAttribute('aria-pressed',String(value));$('placeSource').textContent=value?'⌖ Click the map to release':'⌖ Place source on map';$('sourceHint').hidden=!value;document.body.classList.toggle('placing-source',value);}
function closeSources(){$('sourcesPanel').hidden=true;$('sourcesToggle').setAttribute('aria-expanded','false');}
function resetSources(){emitters.clear();setPlacing(false);closeSources();updateSourceList();}
function updateSourceList(){
 $('sourceList').replaceChildren(...emitters.sources.map(s=>{
  const row=document.createElement('div');row.className='source-row';const dot=document.createElement('span');dot.className='source-swatch';dot.style.background=s.color;
  const label=document.createElement('div');label.className='source-description';const title=document.createElement('strong');title.textContent=`S${s.id} · ${s.lat.toFixed(2)}°, ${s.lon.toFixed(2)}°`;
  const details=document.createElement('div');details.textContent=date(s.ms);label.append(title,details);
  const remove=document.createElement('button');remove.textContent='×';remove.setAttribute('aria-label',`Remove source S${s.id}`);remove.onclick=()=>{emitters.remove(s.id);updateSourceList();updateSourceControls();if(loading)render();};row.append(dot,label,remove);return row;
 }));
 const sources=emitters.sources,all=sources.flatMap(s=>s.state?.particles||[]),stopped=all.filter(p=>!p.active).length;
 $('sourceStatus').textContent=sources.length?`${sources.length} source${sources.length===1?'':'s'} · ${all.length} particles${stopped?' · '+stopped+' stopped at missing data':''}`:'No sources yet · up to 6 colours';$('clearSources').disabled=!sources.length;
}
function handleMapPick(ll){
 if(!placingSource){probe(ll);return;}if(loading||!canTrace())return;
 if(!sampleVelocity(model.geometry,current.u,current.v,ll.lat,ll.lng)){tell('Place the source on a cell with both flow components.',true);return;}
 stop();if(emitters.sources.length>=6){tell('Remove a source before placing another.',true);setPlacing(false);return;}const source=emitters.add(ll.lat,ll.lng,index,current.ms,$('sourceColor').value);source.state={index,ms:current.ms,particles:[{id:0,bornMs:current.ms,lat:source.lat,lon:source.lon,active:true,track:[{lat:source.lat,lon:source.lon}]}]};
 setPlacing(false);updateSourceList();updateSourceControls();$('message').classList.add('ready');
 const colors=$('sourceColor');colors.selectedIndex=(colors.selectedIndex+1)%colors.options.length;
 $('sourcesPanel').hidden=false;$('sourcesToggle').setAttribute('aria-expanded','true');
}
$('sourcesToggle').onclick=()=>{const open=$('sourcesPanel').hidden;closeOptions();$('sourcesPanel').hidden=!open;$('sourcesToggle').setAttribute('aria-expanded',String(open));};$('closeSources').onclick=()=>{closeSources();setPlacing(false);};
$('placeSource').onclick=()=>{if(placingSource){setPlacing(false);return;}stop();setPlacing(true);closeSources();closeMobilePanel();};$('cancelSource').onclick=()=>setPlacing(false);
$('clearSources').onclick=()=>{emitters.clear();setPlacing(false);updateSourceList();updateSourceControls();if(loading)render();};
$('diffusion').onchange=()=>{
 const value=Number($('diffusion').value);if($('diffusion').value.trim()===''||!Number.isFinite(value)||value<0||value>100000){$('diffusion').value=emitters.diffusion;return;}
 stop();emitters.setDiffusion(value);$('diffusionHint').textContent=value?`1 h radial RMS spread ≈ ${(Math.sqrt(4*value*3600)/1000).toFixed(2)} km · repeatable random paths`:'0 = advection only · no random spreading';
 if(current&&emitters.sources.length)render();else updateSourceList();
};
for(const id of ['plumeDensity','plumeLifetime'])$(id).onchange=()=>{
 stop();emitters.setEmission(Number($('plumeDensity').value),Number($('plumeLifetime').value));if(current&&emitters.sources.length)render();
};
document.addEventListener('keydown',e=>{if(e.key==='Escape'){setPlacing(false);closeSources();}});
const tracerOverlay=new TracerOverlay({getSize:()=>({width:innerWidth,height:innerHeight}),getSources:()=>canTrace()?emitters.sources:[],getSettings:()=>({paths:$('showPaths').checked,opacity:Number($('plumeOpacity').value)/100,size:Number($('plumeSize').value),lifetimeHours:emitters.lifetimeHours}),project:(lat,lon)=>{
 if(globeMode){const p=globe?.screen(lat,lon);return p&&p.z>.08&&p.x>=0&&p.y>=0&&p.x<=innerWidth&&p.y<=innerHeight?p:null;}
 const centre=map.getCenter().lng,lng=centre+wrapLon(lon-centre),p=map.latLngToContainerPoint([lat,lng]);return p.x>=0&&p.y>=0&&p.x<=innerWidth&&p.y<=innerHeight?p:null;
}});

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
# Vector archives also contain native u and v (m/s).
# Preview values are highly compressed, not reference data.`;}
function showDownload(){
  if(!current||loading)return;stop();
  const f=frames[index],selected=currentViews(),parts=[f,...(f.v?[f.v]:[])];
  const variables=parts.map((p,i)=>({name:selected[i].name,stream:p.group.stream_index,frame:p.frame}));
  const wholeViews=views.filter(v=>selected.some(p=>p.name===v.name&&p.rawCode===v.rawCode&&(p.name!=='unknown'||p.rawCode||v===p)));
  const wholeGroups=[...new Set(wholeViews.flatMap(v=>v.frames.map(f=>f.group)))];
  const examples=accessExamples({url:new URL($('dataset').value,location.href).href,variables,wholeGroups,unit:activeConfig.unit,factor:activeConfig.factor,offset:activeConfig.offset,native:model.geometry.native});
  $('cliExample').textContent=examples.cli;$('xarrayExample').textContent=examples.python;$('pythonExample').textContent=pythonExample();
  $('downloadContext').textContent=activeConfig.title+' · '+(custom?custom.kind+' '+custom.level:field==='currents'?'Sea surface':$('level').selectedOptions[0].textContent)+' · '+date(current.ms);
  $('exportStatus').textContent='';$('pythonDownload').disabled=exportBusy;$('pythonDialog').showModal();
}
$('exportField').onclick=showDownload;$('closePython').onclick=()=>$('pythonDialog').close();
for(const [button,code,label] of [['copyCli','cliExample','Command'],['copyXarray','xarrayExample','Python example'],['copyPython','pythonExample','NumPy example']])$(button).onclick=async()=>{try{await navigator.clipboard.writeText($(code).textContent);$('exportStatus').textContent=label+' copied.';}catch{$('exportStatus').textContent='Select and copy the example above.';}};
async function exportCurrent(){
  if(!current||loading||exportBusy)return;exportBusy=true;stop();const snapshot=current,config={...activeConfig},geometry={...model.geometry},name=downloadName();
  const metadata={variable:custom?.name||(snapshot.v?(field==='currents'?'sea_surface_speed':'wind_speed'):field),title:config.title,level:custom?.level||(field==='currents'?'0':$('level').value),type_of_level:custom?.kind||(field==='currents'||$('level').value==='surface'?'surface':'isobaricInhPa'),valid_time:new Date(snapshot.ms).toISOString(),source_url:new URL($('dataset').value,location.href).href,grib_record:snapshot.record};
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
window.avencMet={model,map,get state(){return {field,custom:custom?.key,index,frames:frames.length,ready:Boolean(current)&&!loading,loading,playing,validTime:current?.ms,playDirection,diffusion:emitters.diffusion,sources:emitters.sources.map(s=>({id:s.id,lat:s.lat,lon:s.lon,index:s.index,ms:s.ms,color:s.color,state:s.state?{index:s.state.index,particles:s.state.particles.map(p=>({id:p.id,lat:p.lat,lon:p.lon,active:p.active}))}:null})),native:Boolean(model.geometry?.native),view:globeMode?'3d':'2d',globe:globeMode?{lat:globe.lat,lon:globe.lon,zoom:globe.zoom}:null};}};
start();
