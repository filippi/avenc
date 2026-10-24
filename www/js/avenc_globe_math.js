// Shared orthographic globe projection. Angles are geographic degrees.
const D=Math.PI/180;
export const wrapLongitude=lon=>((lon+180)%360+360)%360-180;
export function globeBasis(lat,lon){const p=lat*D,l=lon*D;return {right:[Math.cos(l),0,-Math.sin(l)],up:[-Math.sin(p)*Math.sin(l),Math.cos(p),-Math.sin(p)*Math.cos(l)],front:[Math.cos(p)*Math.sin(l),Math.sin(p),Math.cos(p)*Math.cos(l)]};}
export function spherePoint(lat,lon){const p=lat*D,l=lon*D;return [Math.cos(p)*Math.sin(l),Math.sin(p),Math.cos(p)*Math.cos(l)];}
export function projectGlobe(lat,lon,basis){const p=spherePoint(lat,lon),dot=a=>p.reduce((n,v,i)=>n+v*a[i],0);return {x:dot(basis.right),y:dot(basis.up),z:dot(basis.front)};}
export function pickGlobe(x,y,basis){const r2=x*x+y*y;if(r2>1)return null;const z=Math.sqrt(Math.max(0,1-r2)),p=basis.right.map((v,i)=>v*x+basis.up[i]*y+basis.front[i]*z);return {lat:Math.asin(Math.max(-1,Math.min(1,p[1])))/D,lng:wrapLongitude(Math.atan2(p[0],p[2])/D)};}
