// Display-only transforms. Scientific values and downloaded arrays are unchanged.
export function colourPosition(value,config){
 const [lo,hi]=config.range,span=Math.max(hi-lo,1e-30);let q=(value-lo)/span;
 if(config.log){const knee=span/100;
  if(lo<0&&hi>0){const t=v=>Math.sign(v)*Math.log1p(Math.abs(v)/knee);q=(t(value)-t(lo))/(t(hi)-t(lo));}
  else q=Math.log1p(Math.max(0,value-lo)/knee)/Math.log(101);
 }
 return Math.max(0,Math.min(1,q));
}
export function colourValue(q,config){
 const [lo,hi]=config.range,span=hi-lo;if(!config.log)return lo+q*span;
 const knee=span/100;if(lo<0&&hi>0){const t=v=>Math.sign(v)*Math.log1p(Math.abs(v)/knee),x=t(lo)+q*(t(hi)-t(lo));return Math.sign(x)*Math.expm1(Math.abs(x))*knee;}
 return lo+Math.expm1(q*Math.log(101))*knee;
}
export function colourOpacity(q,config,contrast){const x=Math.max(0,Math.min(1,(q-.1)/.08));return contrast&&config.range[0]>=0?x*x*(3-2*x):1;}
