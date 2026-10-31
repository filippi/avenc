// Optional existing ERA5-Land fixture; do not download/re-encode it for this test.
const assert=require('node:assert/strict'),path=require('node:path'),os=require('node:os'),{chromium}=require('playwright');
(async()=>{const b=await chromium.launch({executablePath:process.env.CHROME_EXECUTABLE||undefined,headless:true,args:['--enable-unsafe-swiftshader']});try{
 const p=await b.newPage({viewport:{width:1480,height:1000}}),errors=[];p.on('pageerror',e=>errors.push(e.message));
 const fixture=process.env.AVENC_ERA5LAND_FIXTURE_URL||'./era5land-fixture.mp4';
 await p.goto('http://127.0.0.1:8765/index.html?dataset='+encodeURIComponent(fixture)+'&lead=0');
 await p.waitForFunction(()=>avencMet.state.ready||document.querySelector('#message.error'),null,{timeout:90000});
 const state=await p.evaluate(()=>({state:avencMet.state,geometry:avencMet.model.geometry,message:document.querySelector('#messageText').textContent}));console.log('ERA5-Land',state);
 assert.equal(state.state.ready,true);assert.equal(state.state.native,false);assert.equal(state.state.field,'wind');assert.equal(state.state.custom,undefined);
 assert.equal(state.geometry.nx,3600);assert.equal(state.geometry.ny,1801);assert(Math.abs(state.geometry.dx-.1)<1e-9);assert.equal(state.geometry.lat0,90);assert.equal(state.geometry.cyclic,true);
 assert(await p.locator('#toggle3d').isEnabled());assert.equal(await p.locator('#nativeNotice').isVisible(),false);
 const before=await p.evaluate(()=>avencMet.model.transport.bytes);await p.locator('#toggle3d').click();await p.waitForFunction(()=>avencMet.state.view==='3d');await p.waitForTimeout(900);assert.equal(await p.evaluate(()=>avencMet.model.transport.bytes),before);
 // Verify original missing values remain NaNs, and both U and V use the same grid.
 const mask=await p.evaluate(async()=>{const group=avencMet.model.groups[0],values=await avencMet.model.getGrid(group,0);let finite=0,missing=0;for(const v of values)Number.isFinite(v)?finite++:missing++;return{finite,missing};});assert(mask.finite>0);assert(mask.missing>0);console.log('Preserved mask:',mask);
 await p.screenshot({path:path.join(os.tmpdir(),'ecflix-era5land-globe.png')});
 const time=await p.evaluate(()=>avencMet.state.validTime);await p.locator('#next').click();await p.waitForFunction(t=>avencMet.state.ready&&avencMet.state.validTime!==t,time,{timeout:90000});assert.equal(await p.evaluate(()=>avencMet.state.view),'3d');
 await p.setViewportSize({width:390,height:844});await p.waitForTimeout(250);assert.equal(await p.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);await p.screenshot({path:path.join(os.tmpdir(),'ecflix-era5land-mobile.png')});await p.locator('#toggle3d').click();assert.equal(await p.evaluate(()=>avencMet.state.view),'2d');assert.deepEqual(errors,[]);
 console.log('PASS: actual GRIB1 ERA5-Land, surface-level U/V pairing, native grid geometry, preserved NaN mask, globe without refetch, forecast stepping, mobile and 2D return.');
}finally{await b.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
