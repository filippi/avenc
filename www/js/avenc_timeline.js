// UTC timeline labels shared by forecasts and long observational archives.
const DAY=86400000;
export function referenceTime(record={}){
 const dd=String(record.dataDate||''),hh=String(record.dataTime||0).padStart(4,'0');
 return /^\d{8}$/.test(dd)?Date.parse(`${dd.slice(0,4)}-${dd.slice(4,6)}-${dd.slice(6,8)}T${hh.slice(0,2)}:${hh.slice(2,4)}:00Z`):NaN;
}
export function timelineInfo(frames){
 const first=frames[0]?.ms,last=frames.at(-1)?.ms,span=last-first;
 const references=new Set(frames.map(f=>referenceTime(f.record)).filter(Number.isFinite));
 return {first,last,span,archive:references.size>1||span>31*DAY,years:new Date(first).getUTCFullYear()!==new Date(last).getUTCFullYear()};
}
export function formatTime(ms){return new Date(ms).toLocaleString('en-GB',{day:'2-digit',month:'short',year:'numeric',hour:'2-digit',minute:'2-digit',timeZone:'UTC',hour12:false})+' UTC';}
export function tickLabel(ms,info){return new Date(ms).toLocaleDateString('en-GB',info.span>366*DAY?{month:'short',year:'numeric',timeZone:'UTC'}:{day:'2-digit',month:'short',...(info.years||info.archive?{year:'numeric'}:{}),timeZone:'UTC'});}
export function positionLabel(frame,index,frames,info){
 if(info.archive)return `${index+1} / ${frames.length}`;
 const init=referenceTime(frame.record);return Number.isFinite(init)?`${frame.ms<init?'−':'+'}${Math.round(Math.abs(frame.ms-init)/3600000)} h`:'';
}
