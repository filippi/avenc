// Optional existing POF fixture; no download/re-encode required.
const assert=require('node:assert/strict');
const {chromium}=require('playwright');
(async()=>{const browser=await chromium.launch({executablePath:process.env.CHROME_EXECUTABLE||undefined,headless:true,args:['--enable-unsafe-swiftshader']});
try{
 const page=await browser.newPage({viewport:{width:1400,height:950}}),errors=[];page.on('pageerror',e=>{errors.push(e.message);console.log('PAGEERROR',e.message)});
 const url=new URL(process.env.AVENC_MET_URL||'http://127.0.0.1:8765/index.html');url.searchParams.set('dataset',process.env.AVENC_POF_FIXTURE_URL||'./dataset/pof-fixture.mp4');await page.goto(url.href);
 try{await page.waitForFunction(()=>window.avencMet?.state.ready||document.querySelector('#message.error'),null,{timeout:45000});}catch(e){console.log('STUCK',await page.evaluate(()=>({state:window.avencMet?.state,message:document.querySelector('#messageText')?.textContent,groups:window.avencMet?.model.groups?.length,bytes:window.avencMet?.model.transport?.bytes})));throw e;}
 const state=await page.evaluate(()=>({state:avencMet.state,geometry:{...avencMet.model.geometry,latitudes:avencMet.model.geometry.latitudes?.slice(0,3),rowAtLatitude:undefined},message:document.querySelector('#messageText').textContent,unit:document.querySelector('#legendUnit').textContent,bytes:avencMet.model.transport.bytes}));console.log('POF',JSON.stringify(state));
 assert(state.state.ready,state.message);assert.equal(state.state.native,false);assert.equal(state.geometry.kind,'gaussian');assert.equal(state.geometry.gaussianN,1280);assert.equal(state.geometry.lon0,180);assert.equal(state.unit,'%');assert.equal(state.geometry.nx,5136);assert.equal(state.geometry.ny,2560);assert(state.bytes<21484340);
 assert(await page.locator('#toggle3d').isEnabled());await page.locator('#world').click();await page.waitForTimeout(500);await page.screenshot({path:require('node:path').join(require('node:os').tmpdir(),'ecflix-pof-2d.png')});
 const before=await page.evaluate(()=>avencMet.model.transport.bytes);await page.locator('#toggle3d').click();await page.waitForFunction(()=>avencMet.state.view==='3d');await page.waitForTimeout(600);assert.equal(await page.evaluate(()=>avencMet.model.transport.bytes),before);await page.screenshot({path:require('node:path').join(require('node:os').tmpdir(),'ecflix-pof-3d.png')});
 const time=state.state.validTime;await page.locator('#next').click();await page.waitForFunction(t=>avencMet.state.ready&&avencMet.state.validTime!==t,time,{timeout:120000});assert.equal(await page.evaluate(()=>avencMet.state.view),'3d');
 const gpu=await page.evaluate(async()=>{
  const {AvencGlobe}=await import('./js/avenc_globe.js'),{makeGeometry,gaussianLatitudes,gridIndex}=await import('./js/avenc_met_core.js');
  const box=document.createElement('div');box.style.cssText='position:fixed;inset:0;z-index:-1';document.body.append(box);
  const globe=new AvencGlobe(box);globe.setActive(true);
  const geo=makeGeometry(gaussianLatitudes(4),Array.from({length:8},(_,i)=>180+i*45),8,4,{gaussian:true,gaussianN:2});
  const u=Float32Array.from({length:32},(_,i)=>i),ramp=Array.from({length:256},(_,i)=>[i,i,i]),out=[];
  for(const [lat,lon] of [[85,23],[17,23],[-85,179],[40,-179]]){
   globe.setView(lat,lon,1);globe.setField({u},geo,{range:[0,31],factor:1,offset:0},ramp,{opacity:1,basemap:'none',arrows:false,flow:false});
   const pixel=new Uint8Array(4);globe.gl.readPixels(Math.floor(globe.cx*globe.dpr),Math.floor((globe.height-globe.cy)*globe.dpr),1,1,globe.gl.RGBA,globe.gl.UNSIGNED_BYTE,pixel);
   const expected=Math.round(u[gridIndex(geo,lat,lon)]/31*255);out.push({lat,lon,expected,actual:pixel[0]});if(Math.abs(pixel[0]-expected)>2)throw Error('Gaussian GPU/CPU mismatch '+JSON.stringify(out));
  }
  globe.destroy();box.remove();return out;
 });console.log('GPU',JSON.stringify(gpu));
 await page.setViewportSize({width:390,height:844});await page.waitForTimeout(300);assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);await page.screenshot({path:require('node:path').join(require('node:os').tmpdir(),'ecflix-pof-mobile.png')});
 await page.locator('#toggle3d').click();assert.equal(await page.evaluate(()=>avencMet.state.view),'2d');await page.evaluate(async()=>{const {streamGeometry}=await import('./js/avenc_met_core.js');for(const g of avencMet.model.groups)g._mapGeometry=streamGeometry({options:{crop_width:g.options.crop_width,crop_height:g.options.crop_height}});});
 await page.locator('#next').click();await page.waitForFunction(()=>avencMet.state.ready&&avencMet.model.geometry.assumed);
 assert(await page.locator('#nativeNotice').isVisible());assert((await page.locator('#nativeNotice').textContent()).includes('Assumed global'));assert.equal(await page.evaluate(()=>avencMet.model.geometry.lon0),0);
 assert.deepEqual(errors,[]);
 console.log('PASS: uploaded POF range access, Gaussian 2D/3D, true 180E origin, percent scale, CPU/GPU agreement, next day, mobile, no extra movie transfer on globe toggle.');
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1});
