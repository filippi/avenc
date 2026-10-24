import {gridIndex} from './avenc_met_core.js';
import {globeBasis,projectGlobe,pickGlobe,wrapLongitude} from './avenc_globe_math.js';
const vertex=`#version 300 es
in vec2 position;void main(){gl_Position=vec4(position,0.,1.);}`;
const fragment=`#version 300 es
precision highp float;precision highp int;
uniform sampler2D valuesU,valuesV,colourRamp,earth;
uniform vec2 centre,viewport;uniform float radius,dpr;
uniform vec3 basisRight,basisUp,basisFront;
uniform ivec2 gridSize;uniform vec4 gridAxes;
uniform bool cyclic,hasV,logScale,rain,showEarth,lightEarth,hasField;
uniform vec2 limits,conversion;uniform float opacity;
uniform vec2 probe;uniform bool showProbe;
out vec4 pixel;
const float DEG=57.29577951308232;
void main(){
 vec2 p=(gl_FragCoord.xy/dpr-vec2(centre.x,viewport.y-centre.y))/radius;
 float r2=dot(p,p);
 if(r2>1.){float a=.26*exp(-42.*(sqrt(r2)-1.));pixel=vec4(.14,.54,.73,a);return;}
 float z=sqrt(1.-r2);vec3 world=basisRight*p.x+basisUp*p.y+basisFront*z;
 float lat=asin(clamp(world.y,-1.,1.))*DEG,lon=atan(world.x,world.z)*DEG;
 vec2 earthUV=vec2((lon+180.)/360.,(90.-lat)/180.);
 vec3 base=showEarth?texture(earth,earthUV).rgb:vec3(.025,.055,.08);
 if(lightEarth&&showEarth)base=mix(base,vec3(.65,.71,.7),.52);
 vec3 colour=base;
 if(hasField){
  float x=(lon-gridAxes.y)/gridAxes.z,y=(lat-gridAxes.x)/gridAxes.w;
  if(cyclic)x=mod(mod(x,float(gridSize.x))+float(gridSize.x),float(gridSize.x));
  int i=int(floor(x+.5)),j=int(floor(y+.5));if(cyclic)i=i%gridSize.x;
  if(i>=0&&i<gridSize.x&&j>=0&&j<gridSize.y){
   float v=texelFetch(valuesU,ivec2(i,j),0).r;
   if(hasV)v=length(vec2(v,texelFetch(valuesV,ivec2(i,j),0).r));
   v=v*conversion.x+conversion.y;
   if(!isnan(v)&&!isinf(v)&&!(rain&&v<.05)){
    float q=logScale?log(1.+max(0.,v))/log(1.+limits.y):(v-limits.x)/(limits.y-limits.x);
    vec3 c=texelFetch(colourRamp,ivec2(int(floor(clamp(q,0.,1.)*255.+.5)),0),0).rgb;
    colour=mix(base,c,opacity);
   }
  }
 }
 float meridian=abs(sin(lon*.104719755)),parallel=abs(sin(lat*.104719755));
 float line=(1.-smoothstep(0.,.016,min(meridian,parallel)))*.055;
 colour=mix(colour,vec3(.72,.86,.88),line);
 colour*=.78+.22*pow(z,.6);
 colour+=vec3(.06,.2,.27)*pow(1.-z,4.)*.45;
 if(showProbe){vec2 uv=vec2(probe.y/DEG,probe.x/DEG);vec3 mark=vec3(cos(uv.y)*sin(uv.x),sin(uv.y),cos(uv.y)*cos(uv.x));float dd=length(world-mark);float ring=1.-smoothstep(.002,.004,abs(dd-.012));colour=mix(colour,vec3(.97),ring);}
 pixel=vec4(colour,1.);
}`;
function program(gl){
 const shader=(type,source)=>{const s=gl.createShader(type);gl.shaderSource(s,source);gl.compileShader(s);if(!gl.getShaderParameter(s,gl.COMPILE_STATUS)){const msg=gl.getShaderInfoLog(s);gl.deleteShader(s);throw new Error('Globe shader: '+msg);}return s;};
 const v=shader(gl.VERTEX_SHADER,vertex),f=shader(gl.FRAGMENT_SHADER,fragment),p=gl.createProgram();gl.attachShader(p,v);gl.attachShader(p,f);gl.linkProgram(p);gl.deleteShader(v);gl.deleteShader(f);if(!gl.getProgramParameter(p,gl.LINK_STATUS))throw new Error('Globe shader could not link');return p;
}
export class AvencGlobe{
 constructor(container,{onPick=()=>{},onError=()=>{}}={}){
  this.container=container;this.onPick=onPick;this.onError=onError;this.active=false;this.lat=35;this.lon=5;this.zoom=1;this.pickPoint=null;
  this.canvas=document.createElement('canvas');this.canvas.className='globe-surface';this.canvas.setAttribute('aria-label','3D forecast globe: drag to rotate, scroll or pinch to zoom');this.canvas.tabIndex=0;
  this.arrows=document.createElement('canvas');this.flow=document.createElement('canvas');this.arrows.className=this.flow.className='globe-overlay';
  const gl=this.canvas.getContext('webgl2',{alpha:true,antialias:false,premultipliedAlpha:false,depth:false});if(!gl)throw new Error('3D requires WebGL 2. Use the 2D map on this device.');this.gl=gl;
  this.program=program(gl);this.uniforms=new Map();gl.useProgram(this.program);
  const buffer=this.buffer=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,buffer);gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,-1,1,-1,1,1,-1,1,1]),gl.STATIC_DRAW);const loc=gl.getAttribLocation(this.program,'position');gl.enableVertexAttribArray(loc);gl.vertexAttribPointer(loc,2,gl.FLOAT,false,0,0);
  this.textures=Array.from({length:4},(_,i)=>{gl.activeTexture(gl.TEXTURE0+i);const t=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,t);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.NEAREST);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);if(i<2)gl.texImage2D(gl.TEXTURE_2D,0,gl.R32F,1,1,0,gl.RED,gl.FLOAT,new Float32Array([NaN]));else gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,1,1,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array([16,35,48,255]));return t;});
  ['valuesU','valuesV','colourRamp','earth'].forEach((n,i)=>gl.uniform1i(this.location(n),i));
  container.append(this.canvas,this.arrows,this.flow);this.resizeObserver=new ResizeObserver(()=>this.resize());this.resizeObserver.observe(container);
  this.canvas.addEventListener('webglcontextlost',e=>{e.preventDefault();this.onError('The 3D graphics context was lost. Returned to the 2D map.');});
  this.pointers=new Map();this.canvas.addEventListener('pointerdown',e=>{this.canvas.setPointerCapture(e.pointerId);this.pointers.set(e.pointerId,[e.clientX,e.clientY]);this.dragged=false;this.pinch=null;});
  this.canvas.addEventListener('pointermove',e=>this.pointerMove(e));
  this.canvas.addEventListener('pointerup',e=>{this.pointers.delete(e.pointerId);this.pinch=null;if(!this.dragged){const r=this.canvas.getBoundingClientRect(),ll=this.pick(e.clientX-r.left,e.clientY-r.top);if(ll)this.onPick(ll);}});
  this.canvas.addEventListener('pointercancel',e=>{this.pointers.delete(e.pointerId);this.pinch=null;});
  this.canvas.addEventListener('wheel',e=>{e.preventDefault();this.zoomBy(Math.exp(-e.deltaY*.001));},{passive:false});
  this.canvas.addEventListener('keydown',e=>{const moves={ArrowLeft:[0,-8],ArrowRight:[0,8],ArrowUp:[8,0],ArrowDown:[-8,0]};if(moves[e.key]){e.preventDefault();this.setView(this.lat+moves[e.key][0],this.lon+moves[e.key][1],this.zoom);}else if(e.key==='+'||e.key==='='){e.preventDefault();this.zoomBy(1.2);}else if(e.key==='-'){e.preventDefault();this.zoomBy(1/1.2);}});
  this.readyEarth=this.loadEarth();this.tick=this.tick.bind(this);this.raf=requestAnimationFrame(this.tick);
 }
 location(name){if(!this.uniforms.has(name))this.uniforms.set(name,this.gl.getUniformLocation(this.program,name));return this.uniforms.get(name);}
 pointerMove(e){
  if(!this.pointers.has(e.pointerId))return;const old=this.pointers.get(e.pointerId);this.pointers.set(e.pointerId,[e.clientX,e.clientY]);
  if(this.pointers.size===2){const [a,b]=[...this.pointers.values()],d=Math.hypot(a[0]-b[0],a[1]-b[1]);if(this.pinch)this.zoomBy(d/this.pinch);this.pinch=d;this.dragged=true;return;}
  const dx=e.clientX-old[0],dy=e.clientY-old[1];if(Math.abs(dx)+Math.abs(dy)>1)this.dragged=true;
  this.setView(this.lat+dy/this.radius*65,this.lon-dx/this.radius*65,this.zoom);
 }
 resize(){
  if(!this.active)return;const r=this.container.getBoundingClientRect();this.width=r.width;this.height=r.height;this.dpr=Math.min(devicePixelRatio||1,2);
  for(const c of [this.canvas,this.arrows,this.flow]){c.width=Math.max(1,Math.round(this.width*this.dpr));c.height=Math.max(1,Math.round(this.height*this.dpr));}
  const phone=this.width<=760,left=phone?8:110,right=phone?8:115,top=phone?185:140,bottom=phone?105:105;
  this.cx=(this.width+left-right)/2;this.cy=(this.height+top-bottom)/2;this.baseRadius=Math.max(40,Math.min(this.width-left-right,this.height-top-bottom)*.46);this.radius=this.baseRadius*this.zoom;this.draw();
 }
 setActive(active){this.active=active;if(active)this.resize();else this.clearFlow();}
 setView(lat,lon,zoom=1){this.lat=Math.max(-89,Math.min(89,lat));this.lon=wrapLongitude(lon);this.zoom=Math.max(.65,Math.min(5,zoom));this.radius=(this.baseRadius||200)*this.zoom;this.clearFlow();if(this.active)this.draw();}
 zoomBy(factor){this.setView(this.lat,this.lon,this.zoom*factor);}
 pick(x,y){return pickGlobe((x-this.cx)/this.radius,-(y-this.cy)/this.radius,globeBasis(this.lat,this.lon));}
 screen(lat,lon){const p=projectGlobe(lat,lon,this.basis);return {x:this.cx+p.x*this.radius,y:this.cy-p.y*this.radius,z:p.z};}
 setField(current,geometry,config,colours,settings){
  const gl=this.gl;if(geometry.native)throw new Error('3D needs geographic coordinates.');if(Math.max(geometry.nx,geometry.ny)>gl.getParameter(gl.MAX_TEXTURE_SIZE))throw new Error('This grid exceeds this device’s 3D texture limit. Use 2D.');
  this.current=current;this.geometry=geometry;this.config=config;this.settings=settings;
  for(const [i,a] of [[0,current?.u],[1,current?.v]])if(a&&a!==this.uploaded?.[i]){gl.activeTexture(gl.TEXTURE0+i);gl.bindTexture(gl.TEXTURE_2D,this.textures[i]);gl.texImage2D(gl.TEXTURE_2D,0,gl.R32F,geometry.nx,geometry.ny,0,gl.RED,gl.FLOAT,a);}
  this.uploaded=[current?.u,current?.v];gl.activeTexture(gl.TEXTURE2);gl.bindTexture(gl.TEXTURE_2D,this.textures[2]);gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,256,1,0,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array(colours.flatMap(c=>[...c,255])));
  if(!this.particles)this.particles=Array.from({length:Math.min(850,this.width<760?360:850)},()=>this.seed());
  if(!current?.v||!settings.flow)this.clearFlow();if(this.active)this.draw();
 }
 clear(){this.current=null;this.clearFlow();if(this.active)this.draw();}
 clearFlow(){this.flow.getContext('2d').clearRect(0,0,this.flow.width,this.flow.height);}
 async loadEarth(){
  try{const response=await fetch(new URL('../data/ne_110m_land.geojson',import.meta.url));if(!response.ok)throw new Error('Coastline background unavailable');const data=await response.json();if(this.destroyed)return;const c=document.createElement('canvas');c.width=2048;c.height=1024;const ctx=c.getContext('2d');ctx.fillStyle='#0f293b';ctx.fillRect(0,0,c.width,c.height);ctx.fillStyle='#294956';ctx.strokeStyle='#9fb9bc';ctx.lineWidth=.65;
   for(const feature of data.features){const polys=feature.geometry.type==='Polygon'?[feature.geometry.coordinates]:feature.geometry.coordinates;for(const rings of polys){ctx.beginPath();for(const ring of rings){ring.forEach(([lon,lat],i)=>{const x=(lon+180)/360*c.width,y=(90-lat)/180*c.height;i?ctx.lineTo(x,y):ctx.moveTo(x,y);});ctx.closePath();}ctx.fill('evenodd');ctx.stroke();}}
   const gl=this.gl;gl.activeTexture(gl.TEXTURE3);gl.bindTexture(gl.TEXTURE_2D,this.textures[3]);gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,c);this.earthReady=true;if(this.active)this.draw();
  }catch(e){this.earthError=e.message;}
 }
 draw(){
  if(!this.active||!this.width||this.gl.isContextLost())return;const gl=this.gl,c=this.config||{range:[0,1],factor:1,offset:0},g=this.geometry||{nx:1,ny:1,lat0:0,lon0:0,dx:1,dy:1};gl.useProgram(this.program);gl.viewport(0,0,this.canvas.width,this.canvas.height);
  this.basis=globeBasis(this.lat,this.lon);const f=(n,v)=>gl.uniform1f(this.location(n),v),b=(n,v)=>gl.uniform1i(this.location(n),v?1:0),v2=(n,x,y)=>gl.uniform2f(this.location(n),x,y);
  v2('centre',this.cx,this.cy);v2('viewport',this.width,this.height);f('radius',this.radius);f('dpr',this.dpr);for(const [n,v] of [['basisRight',this.basis.right],['basisUp',this.basis.up],['basisFront',this.basis.front]])gl.uniform3fv(this.location(n),v);
  gl.uniform2i(this.location('gridSize'),g.nx,g.ny);gl.uniform4f(this.location('gridAxes'),g.lat0,g.lon0,g.dx,g.dy);b('cyclic',g.cyclic);b('hasField',this.current);b('hasV',this.current?.v);b('logScale',c.log);b('rain',this.settings?.rain);b('showEarth',this.settings?.basemap!=='none');b('lightEarth',this.settings?.basemap==='streets');v2('limits',...c.range);v2('conversion',c.factor,c.offset);f('opacity',this.settings?.opacity??.78);b('showProbe',this.pickPoint);v2('probe',this.pickPoint?.lat||0,this.pickPoint?.lng||0);
  gl.drawArrays(gl.TRIANGLES,0,6);this.drawArrows();
 }
 wind(lat,lon){if(!this.current?.v)return null;const k=gridIndex(this.geometry,lat,lon);if(k<0)return null;const u=this.current.u[k],v=this.current.v[k];return Number.isFinite(u)&&Number.isFinite(v)?[u,v]:null;}
 drawArrows(){const ctx=this.arrows.getContext('2d');ctx.clearRect(0,0,this.arrows.width,this.arrows.height);if(!this.settings?.arrows||!this.current?.v)return;ctx.setTransform(this.dpr,0,0,this.dpr,0,0);ctx.strokeStyle='#f2fbffb8';ctx.lineWidth=.8;
  for(let lat=-75;lat<=75;lat+=12)for(let lon=-180;lon<180;lon+=12){const p=this.screen(lat,lon);if(p.z<.15)continue;const wind=this.wind(lat,lon);if(!wind)continue;const speed=Math.hypot(...wind);if(speed<.5)continue;const next=this.screen(lat+wind[1]/speed,lon+wind[0]/speed/Math.max(.1,Math.cos(lat*Math.PI/180))),a=Math.atan2(next.y-p.y,next.x-p.x),len=Math.min(13,6+speed*.12),dx=Math.cos(a)*len,dy=Math.sin(a)*len;ctx.beginPath();ctx.moveTo(p.x-dx/2,p.y-dy/2);ctx.lineTo(p.x+dx/2,p.y+dy/2);for(const t of [-.55,.55]){ctx.moveTo(p.x+dx/2,p.y+dy/2);ctx.lineTo(p.x+dx/2-Math.cos(a+t)*3,p.y+dy/2-Math.sin(a+t)*3);}ctx.stroke();}
 }
 seed(){const g=this.geometry,lat=g?.cyclic?Math.asin(Math.random()*2-1)*180/Math.PI:(g?.lat0||0)+Math.random()*(g?.ny||180)*(g?.dy||1),lon=g?.cyclic?Math.random()*360-180:(g?.lon0||0)+Math.random()*(g?.nx||360)*(g?.dx||1);return {lat:Math.max(-89,Math.min(89,lat)),lon,age:Math.random()*120};}
 tick(now){this.raf=requestAnimationFrame(this.tick);if(!this.active||document.hidden)return;if(now-(this.last||0)<33)return;const dt=Math.min(.06,(now-(this.last||now))/1000);this.last=now;
  if(!this.current?.v||!this.settings?.flow||matchMedia('(prefers-reduced-motion: reduce)').matches)return;
  const ctx=this.flow.getContext('2d');ctx.setTransform(this.dpr,0,0,this.dpr,0,0);ctx.globalCompositeOperation='destination-in';ctx.fillStyle='rgba(0,0,0,.9)';ctx.fillRect(0,0,this.width,this.height);ctx.globalCompositeOperation='source-over';ctx.strokeStyle='#efffffad';ctx.lineWidth=.8;ctx.beginPath();
  for(const p of this.particles||[]){const v=this.wind(p.lat,p.lon);if(!v||p.age++>140){Object.assign(p,this.seed(),{age:0});continue;}const old=this.screen(p.lat,p.lon),scale=dt*3600/6371000*180/Math.PI;p.lat=Math.max(-89.5,Math.min(89.5,p.lat+v[1]*scale));p.lon=wrapLongitude(p.lon+v[0]*scale/Math.max(.05,Math.cos(p.lat*Math.PI/180)));const next=this.screen(p.lat,p.lon);if(old.z>.05&&next.z>.05&&Math.hypot(next.x-old.x,next.y-old.y)<30){ctx.moveTo(old.x,old.y);ctx.lineTo(next.x,next.y);}}
  ctx.stroke();
 }
 destroy(){this.destroyed=true;this.active=false;cancelAnimationFrame(this.raf);this.resizeObserver.disconnect();for(const t of this.textures)this.gl.deleteTexture(t);this.gl.deleteBuffer(this.buffer);this.gl.deleteProgram(this.program);this.canvas.remove();this.arrows.remove();this.flow.remove();}
}
