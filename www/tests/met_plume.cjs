// Optional real-browser canvas checks; www/serve.py must serve localhost:8765.
const assert=require('node:assert/strict'),{chromium}=require('playwright');
(async()=>{const browser=await chromium.launch({executablePath:process.env.CHROME_EXECUTABLE||undefined,headless:true,args:['--enable-unsafe-swiftshader']});try{
const page=await browser.newPage({viewport:{width:600,height:400}});await page.goto('http://127.0.0.1:8765/');
const result=await page.evaluate(async()=>{
 const {TracerOverlay}=await import('./js/avenc_tracer_overlay.js');let paths=false;const state={index:1,ms:3600000,particles:[{id:0,bornMs:0,lat:150,lon:200,active:true,track:[{lat:150,lon:100},{lat:150,lon:200}]}]};
 const sources=[{id:1,lat:70,lon:70,ms:0,color:'#ffb36b',state}],overlay=new TracerOverlay({project:(lat,lon)=>({x:lon,y:lat}),getSources:()=>sources,getSize:()=>({width:600,height:400}),getSettings:()=>({paths,size:16,lifetimeHours:72})});
 cancelAnimationFrame(overlay.raf);overlay.draw(0);const ctx=overlay.canvas.getContext('2d'),alpha=(x,y)=>ctx.getImageData(x,y,1,1).data[3];
 const puff={center:alpha(200,150),inner:alpha(205,150),edge:alpha(210,150),outside:alpha(225,150),path:alpha(140,150)};
 paths=true;overlay.draw(0);const line=alpha(140,150);paths=false;overlay.draw(0);const offAgain=alpha(140,150);
 const cached=overlay.sprite('#ffb36b')===overlay.sprite('#ffb36b');for(let i=0;i<20;i++)overlay.sprite('#'+i.toString(16).padStart(6,'0'));const cacheSize=overlay.sprites.size;
 // A neighbouring forecast interpolates once, then stays fixed while paused.
 state.particles[0].lon=200;sources[0].state={index:2,ms:7200000,particles:[{...state.particles[0],lon:240,track:[]}]};overlay.draw(100);const start=alpha(200,150);overlay.draw(750);const end=alpha(240,150),old=alpha(200,150);const snapshot=ctx.getImageData(0,100,600,100).data.slice();overlay.draw(2000);const paused=ctx.getImageData(0,100,600,100).data;const stable=snapshot.every((v,i)=>v===paused[i]);
 sources.length=0;overlay.draw(2100);const cleared=alpha(240,150)===0;overlay.destroy();return{puff,line,offAgain,cached,cacheSize,start,end,old,stable,cleared};
});assert(result.puff.center>result.puff.inner&&result.puff.inner>result.puff.edge&&result.puff.edge>0);assert.equal(result.puff.outside,0);assert.equal(result.puff.path,0);assert(result.line>0);assert.equal(result.offAgain,0);assert(result.cached&&result.cacheSize<=6);assert(result.start>0&&result.end>0&&result.old===0);assert(result.stable&&result.cleared);console.log('PASS: radial alpha falloff; independent path toggle; six-sprite cache; smooth adjacent-frame motion; no paused drift; clear.');
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
