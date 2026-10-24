// Focused browser regression: visible wind density, zoom, missing values and cleanup.
const assert=require('node:assert/strict'),{chromium}=require('playwright');
(async()=>{const browser=await chromium.launch({executablePath:process.env.CHROME_EXECUTABLE||undefined,headless:true,args:['--enable-unsafe-swiftshader']});try{
 const page=await browser.newPage({viewport:{width:1200,height:900}});
 await page.goto('http://127.0.0.1:8765/index.html');
 const result=await page.evaluate(async()=>{
  const {AvencGlobe}=await import('./js/avenc_globe.js');
  const box=document.createElement('div');box.style.cssText='position:fixed;inset:0;z-index:9999';document.body.append(box);
  const globe=new AvencGlobe(box);globe.setActive(true);
  const geo={nx:360,ny:181,lat0:90,lon0:0,dx:1,dy:-1,cyclic:true},u=new Float32Array(360*181).fill(12),v=new Float32Array(u.length).fill(4);
  globe.setField({u,v},geo,{range:[0,30],factor:1,offset:0},Array.from({length:256},(_,i)=>[i,100,150]),{opacity:1,basemap:'none',arrows:false,flow:true});
  const wide=globe.particles.length;globe.setView(65,179,4);
  const close=globe.particles.length,visible=globe.particles.filter(p=>globe.visible(globe.screen(p.lat,p.lon))).length;
  if(close<=wide||close>4200||visible<close*.98)throw Error('Invalid zoom density or offscreen seeds');
  const dpr=globe.dpr;if(dpr<1.4||globe.canvas.width*globe.canvas.height>6010000)throw Error('Unbounded or missing supersampling');
  for(let i=0;i<35;i++){cancelAnimationFrame(globe.raf);globe.tick(1000+i*40);}cancelAnimationFrame(globe.raf);
  const pixels=globe.flow.getContext('2d').getImageData(0,0,globe.flow.width,globe.flow.height).data;
  let painted=0;for(let i=3;i<pixels.length;i+=4)if(pixels[i]>20)painted++;
  if(painted<1000)throw Error('Trails are not visible');
  const original=u[0];if(original!==12)throw Error('Wind data mutated');
  globe.clearFlow();const empty=globe.flow.getContext('2d').getImageData(0,0,globe.flow.width,globe.flow.height).data;if(empty.some(x=>x))throw Error('Trail clearing failed at scaled DPR');
  globe.setField({u:new Float32Array(u.length).fill(NaN),v},geo,{range:[0,30],factor:1,offset:0},Array.from({length:256},()=>[0,0,0]),{flow:true});
  if(globe.wind(65,179)!==null)throw Error('Missing wind accepted');
  globe.destroy();box.remove();return {wide,close,visible,dpr,painted};
 });console.log(result);assert(result.painted>1000);console.log('PASS: view-focused particles, adaptive density/resolution, visible trails, mask and clearing.');
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
