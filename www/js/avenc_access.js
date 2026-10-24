// Copyable access recipes: CLI selects whole variables, Python selects exact frames.
export function shellQuote(value){return "'"+String(value).replaceAll("'", "'\\''")+"'";}
export function accessExamples({url,variables,wholeGroups,unit,factor=1,offset=0,native=false}){
  if(!/^https?:\/\//.test(url)||!variables.length||!wholeGroups.length)throw new Error('No HTTP field selection');
  const aliases=[...new Set(wholeGroups.map(g=>g.short_name))];
  const selector=aliases.every(n=>n&&n!=='unknown')?aliases.join(','):[...new Set(wholeGroups.map(g=>Number(g.stream_index)))].map(n=>'stream:'+n).join(',');
  const stem=variables.map(v=>v.name).join('_').replace(/[^a-z0-9_-]/gi,'_')||'variable';
  const cli=`avenc2grib --group ${shellQuote(selector)} ${shellQuote(url)} ${shellQuote(stem+'.grib')}`;
  const lines=['import numpy as np','import xarray as xr','','url = '+JSON.stringify(url),'with xr.open_dataset(url, engine="avenc", cache=False) as ds:'];
  variables.forEach((v,i)=>{
    lines.push(`    a${i} = next(a for a in ds.data_vars.values()`, `              if a.attrs.get("stream_id") == ${Number(v.stream)})`, `    a${i} = a${i}.isel({a${i}.dims[0]: ${Number(v.frame)}}).load()`);
  });
  lines.push('',variables.length===2?'field = np.hypot(a0, a1)  # selected U/V → wind speed':'field = a0');
  if(factor!==1||offset!==0)lines.push(`field = field * ${Number(factor)} + ${Number(offset)}  # displayed units`);
  lines.push('field.attrs["units"] = '+JSON.stringify(unit));
  if(native)lines.push('field = field.rename({field.dims[-2]: "row", field.dims[-1]: "column"})','field = field.assign_coords(row=np.arange(field.sizes["row"]),','                            column=np.arange(field.sizes["column"]))','field.plot(yincrease=False)  # stored row 0 at the top');
  else lines.push('field.plot()');
  return {cli,python:lines.join('\n'),selector};
}
