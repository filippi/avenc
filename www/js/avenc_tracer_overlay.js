// Soft dye/smoke puffs in one shared geographic overlay for 2D and 3D.
// Small cached sprites + source-over blending avoid per-particle gradients/shaders.
const wrap=x=>((x+180)%360+360)%360-180;
export class TracerOverlay{
 constructor({project,getSources,getSize,getSettings=()=>({})}){
  this.project=project;this.getSources=getSources;this.getSize=getSize;this.getSettings=getSettings;this.sprites=new Map();this.transitions=new Map();
  this.canvas=document.createElement('canvas');this.canvas.className='tracer-overlay';this.canvas.setAttribute('aria-hidden','true');document.body.append(this.canvas);
  this.tick=this.tick.bind(this);this.raf=requestAnimationFrame(this.tick);
 }
 sprite(color){
  if(this.sprites.has(color))return this.sprites.get(color);
  const c=document.createElement('canvas');c.width=c.height=64;const ctx=c.getContext('2d'),g=ctx.createRadialGradient(32,32,0,32,32,32);
  const rgb=/^#[0-9a-f]{6}$/i.test(color)?[1,3,5].map(i=>parseInt(color.slice(i,i+2),16)):[133,238,216];
  for(const [r,a]of [[0,.65],[.22,.5],[.52,.22],[.8,.055],[1,0]])g.addColorStop(r,`rgba(${rgb.join(',')},${a})`);
  ctx.fillStyle=g;ctx.fillRect(0,0,64,64);this.sprites.set(color,c);if(this.sprites.size>6)this.sprites.delete(this.sprites.keys().next().value);return c;
 }
 tick(now){this.raf=requestAnimationFrame(this.tick);if(document.hidden||now-(this.last||0)<33)return;this.last=now;this.draw(now);}
 draw(now=0){
  const sources=this.getSources();if(!sources.length&&this.empty)return;this.empty=!sources.length;
  const settings=this.getSettings(),motion=!matchMedia('(prefers-reduced-motion: reduce)').matches;
  const {width,height}=this.getSize(),dpr=Math.min(devicePixelRatio||1,2),c=this.canvas,ctx=c.getContext('2d');
  if(c.width!==Math.round(width*dpr)||c.height!==Math.round(height*dpr)){c.width=Math.round(width*dpr);c.height=Math.round(height*dpr);}
  ctx.resetTransform();ctx.clearRect(0,0,c.width,c.height);ctx.scale(dpr,dpr);ctx.globalCompositeOperation='source-over';
  for(const id of this.transitions.keys())if(!sources.some(s=>s.id===id))this.transitions.delete(id);
  for(const s of sources){
   const particles=s.state?.particles||[],sprite=this.sprite(s.color);let transition=this.transitions.get(s.id);
   if(transition?.state!==s.state){
    const previous=transition?.state,adjacent=previous&&s.state&&Math.abs(previous.index-s.state.index)===1;
    transition={state:s.state,from:adjacent?new Map(previous.particles.map(p=>[p.id,p])):null,start:now};this.transitions.set(s.id,transition);
   }
   // Ease only neighbouring forecast frames. Long seeks snap; pause adds no motion.
   const mix=motion&&transition.from?Math.min(1,Math.max(0,(now-transition.start)/650)):1;
   ctx.strokeStyle=s.color;ctx.lineWidth=1;ctx.lineCap='round';
   for(const p of particles){
    if(settings.paths){
     ctx.globalAlpha=.2;ctx.beginPath();let previous=null;
     for(const ll of p.track){const q=this.project(ll.lat,ll.lon);if(!q||!previous||Math.hypot(q.x-previous.x,q.y-previous.y)>width/2){if(q)ctx.moveTo(q.x,q.y);}else ctx.lineTo(q.x,q.y);previous=q;}ctx.stroke();
    }
    const old=transition.from?.get(p.id),lat=old?old.lat+(p.lat-old.lat)*mix:p.lat,lon=old?old.lon+wrap(p.lon-old.lon)*mix:p.lon,q=this.project(lat,lon);if(!q)continue;
    const age=Math.abs((s.state?.ms??s.ms)-(p.bornMs??s.ms))/3600000,life=settings.lifetimeHours??Infinity,fade=Math.sqrt(Math.max(0,1-age/life));
    const radius=(settings.size||12)*(1+Math.min(1,age/24)*.7)*(q.z===undefined?1:.55+.45*Math.sqrt(Math.max(0,q.z)));
    ctx.globalAlpha=.18*(settings.opacity??.9)*fade*(p.active?1:.45)*(old||mix===1?1:mix);ctx.drawImage(sprite,q.x-radius,q.y-radius,radius*2,radius*2);
   }
   if(mix===1)transition.from=null;
   const q=this.project(s.lat,s.lon);if(!q)continue;
   const pulse=motion?(now%1800)/1800:0;
   ctx.globalAlpha=.7*(1-pulse);ctx.lineWidth=1.2;ctx.beginPath();ctx.arc(q.x,q.y,7+pulse*12,0,Math.PI*2);ctx.stroke();
   ctx.globalAlpha=1;ctx.fillStyle='#091c29';ctx.beginPath();ctx.arc(q.x,q.y,5,0,Math.PI*2);ctx.fill();ctx.strokeStyle=s.color;ctx.stroke();
   ctx.fillStyle=s.color;ctx.beginPath();ctx.arc(q.x,q.y,2,0,Math.PI*2);ctx.fill();
   ctx.font='600 11px system-ui';ctx.fillStyle='#08131fd9';ctx.fillRect(q.x+10,q.y-12,27,18);ctx.fillStyle=s.color;ctx.fillText('S'+s.id,q.x+14,q.y+1);
  }
  ctx.globalAlpha=1;
 }
 destroy(){cancelAnimationFrame(this.raf);this.canvas.remove();this.sprites.clear();this.transitions.clear();}
}
