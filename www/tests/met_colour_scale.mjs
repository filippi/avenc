import assert from 'node:assert/strict';
import {colourPosition,colourValue,colourOpacity} from '../js/avenc_colour_scale.js';
for(const range of [[0,100],[-50,50],[-10,100],[-100,-1],[1000,2000]])for(const log of [false,true]){
 const c={range,log};let prev=-1;for(let i=0;i<=100;i++){const q=i/100,v=colourValue(q,c),actual=colourPosition(v,c);assert(Math.abs(actual-q)<1e-12);assert(actual>=prev);prev=actual;}
 assert.equal(colourPosition(range[0],c),0);assert.equal(colourPosition(range[1],c),1);
}
assert.equal(colourPosition(0,{range:[-50,50],log:true}),.5);
assert.equal(colourOpacity(.09,{range:[0,100]},true),0);assert.equal(colourOpacity(.2,{range:[0,100]},true),1);assert.equal(colourOpacity(.09,{range:[-50,50]},true),1);assert.equal(colourOpacity(.09,{range:[0,100]},false),1);
console.log('PASS: linear/log inverse, monotonicity, zero/sign handling, shifted ranges and contrast transparency');
