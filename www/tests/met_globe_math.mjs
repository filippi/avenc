import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
const text=await readFile(new URL('../js/avenc_globe_math.js',import.meta.url),'utf8');
const {globeBasis,projectGlobe,pickGlobe,wrapLongitude}=await import('data:text/javascript;base64,'+Buffer.from(text).toString('base64'));
for(const [lat,lon] of [[0,0],[46,5],[-75,179],[89,-179]]){
 const basis=globeBasis(lat,lon),centre=projectGlobe(lat,lon,basis);assert(Math.abs(centre.x)<1e-12&&Math.abs(centre.y)<1e-12&&Math.abs(centre.z-1)<1e-12);const picked=pickGlobe(0,0,basis);assert(Math.abs(picked.lat-lat)<1e-10);assert(Math.abs(wrapLongitude(picked.lng-lon))<1e-10);
 for(const [x,y] of [[.1,.2],[-.7,.3],[0,-.99]]){const ll=pickGlobe(x,y,basis),p=projectGlobe(ll.lat,ll.lng,basis);assert(Math.abs(p.x-x)<1e-10&&Math.abs(p.y-y)<1e-10&&p.z>=0);}
 assert.equal(pickGlobe(1,1,basis),null);assert(projectGlobe(-lat,wrapLongitude(lon+180),basis).z<0);
}
assert.equal(wrapLongitude(540),-180);assert.equal(wrapLongitude(-181),179);
console.log('PASS: globe centre, forward/inverse projection, poles/dateline, back-face visibility and off-globe rejection.');
