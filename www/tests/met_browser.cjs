// Optional real IFS fixture integration; see ../MET_VIEWER.md.
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),os=require('node:os'),{execFileSync}=require('node:child_process');
const root=path.resolve(__dirname,'../..'),fixture=process.env.AVENC_MET_FIXTURE||path.join(root,'www/dataset/20261019.browser_svt10.mp4');
const native=process.env.AVENC_MET_BIN||path.join(root,'bin/avenc2grib'),screens=process.env.AVENC_MET_SCREENSHOTS||os.tmpdir();
const {chromium}=require('playwright');
let browser;
(async()=>{
browser=await chromium.launch({executablePath:process.env.CHROME_EXECUTABLE||undefined,headless:true});
const page=await browser.newPage({viewport:{width:1480,height:1000}}),errors=[],traffic=[];
page.on('pageerror',e=>errors.push(e.message));page.on('response',r=>{if(new URL(r.url()).pathname.endsWith('.mp4'))traffic.push({status:r.status(),range:r.headers()['content-range']});});
await page.goto(process.env.AVENC_MET_URL||'http://127.0.0.1:8765/index.html?dataset='+encodeURIComponent('./dataset/'+path.basename(fixture)));
const ready=async(field)=>{await page.waitForFunction(f=>window.avencMet?.state.ready&&(!f||window.avencMet.state.field===f),field,{timeout:90000});};
await ready('wind');
const initial=await page.evaluate(()=>({state:avencMet.state,bytes:avencMet.model.transport.bytes,geometry:avencMet.model.geometry,software:avencMet.model.decodeHardwareAcceleration}));assert.equal(initial.geometry.dx,.25);assert(initial.bytes<10e6);console.log('INITIAL',initial);
async function compare(name){
 const group=await page.evaluate(name=>avencMet.model.groups.find(g=>g.short_name===name),name);
 const fi=await page.evaluate(id=>Number([...avencMet.model.gridCache.keys()].find(k=>k.startsWith(id+':')).split(':')[1]),group.id);
 const sample=await page.evaluate(({id,fi})=>{const a=avencMet.model.gridCache.get(id+':'+fi);return Array.from({length:1000},(_,i)=>{const k=(i*104729)%a.length;return [k,Number.isFinite(a[k])?a[k]:null];});},{id:group.id,fi});
 const raw=execFileSync(native,['--stream-id',String(group.stream_index),'--frame-start',String(fi),'--frame-count','1',fixture],{maxBuffer:10e6});
 let max=0,nan=0;for(const [k,v]of sample){const actual=raw.readFloatLE(k*4);if(v===null){assert(!Number.isFinite(actual));nan++;}else max=Math.max(max,Math.abs(v-actual));}
 assert(max<1e-4,`${name} browser/native max difference ${max}`);console.log('NATIVE',name,{frame:fi,points:sample.length,max_difference:max,missing:nan});
}
await compare('10u');await compare('10v');
await page.waitForTimeout(250);await page.screenshot({path:path.join(screens,'avenc-met-wind-final.png')});
await page.locator('[data-field=temperature]').click();await ready('temperature');await compare('2t');
await page.screenshot({path:path.join(screens,'avenc-met-temperature.png')});
await page.locator('[data-field=rain]').click();await ready('rain');await compare('tp');
await page.screenshot({path:path.join(screens,'avenc-met-rain.png')});
const before=await page.evaluate(()=>avencMet.state.validTime);await page.locator('#next').click();await page.waitForFunction(ms=>avencMet.state.ready&&avencMet.state.validTime>ms,before,{timeout:30000});
await page.locator('[data-field=wind]').click();await ready('wind');await page.locator('#level').selectOption('850');await page.waitForFunction(()=>avencMet.state.ready&&document.querySelector('#level').value==='850',{timeout:90000});
await page.mouse.click(800,470);assert(await page.locator('#probe').isVisible());
const bytesBeforePan=await page.evaluate(()=>avencMet.model.transport.bytes);await page.locator('#world').click();await page.waitForTimeout(1000);assert.equal(await page.evaluate(()=>avencMet.model.transport.bytes),bytesBeforePan,'Map zoom does not download another field');
await page.setViewportSize({width:390,height:844});await page.waitForTimeout(300);await page.screenshot({path:path.join(screens,'avenc-met-mobile.png')});
assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
assert(!await page.locator('#layersPanel').isVisible(),'Mobile layers panel starts collapsed');
await page.locator('#closeProbe').click();assert(!await page.locator('#probe').isVisible());
await page.locator('#panelToggle').click();assert(await page.locator('#layersPanel').isVisible());
await page.locator('#mobileParameter').selectOption('temperature');await ready('temperature');assert(!await page.locator('#layersPanel').isVisible(),'Choosing a mobile layer reveals the map');
await page.context().grantPermissions(['clipboard-read','clipboard-write']);
await page.locator('#share').click();
const shared=new URL(await page.evaluate(()=>navigator.clipboard.readText()));
assert.equal(shared.searchParams.get('layer'),'temperature');assert.equal(shared.searchParams.get('level'),'850');
assert(shared.searchParams.has('lat')&&shared.searchParams.has('time'));
await page.goto(shared.href);await ready('temperature');assert.equal(await page.locator('#level').inputValue(),'850');
assert(traffic.every(r=>r.status===206));assert.equal(errors.length,0,errors.join('\n'));
console.log('PASS: real U/V, temperature, rain, forecast-step change, 850-hPa wind, point probe, zoom without field download, mobile viewport; all MP4 responses 206.',{range_requests:traffic.length,bytes:await page.evaluate(()=>avencMet.model.transport.bytes)});
await browser.close();
})().catch(e=>{console.error(e);process.exitCode=1;}).finally(()=>browser?.close());
