import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
const source=await readFile(new URL('../js/avenc_met_core.js',import.meta.url),'utf8');
const {resolveVariable,displayConfiguration,formatReading}=await import('data:text/javascript;base64,'+Buffer.from(source).toString('base64'));
const catalogue=JSON.parse(await readFile(new URL('../data/ifs_variables.json',import.meta.url),'utf8'));
assert.equal(catalogue.schema,'ecflix.variables.v1');assert.equal(Object.keys(catalogue.variables).length,53);
for(const [name,v]of Object.entries(catalogue.variables)){
 assert(v.units&&v.units!=='unknown',name);assert(v.name&&v.display.unit,name);
 assert(Number.isFinite(v.display.factor)&&Number.isFinite(v.display.offset),name);
}
for(const [code,name]of [['0/2/62','ewss'],['0/2/63','nsss'],['0/6/192','tcc'],['0/1/198','sf'],['10/3/14','sve']]){
 const v=resolveVariable('unknown',code,catalogue);assert.equal(v.name,name);assert(v.entry);
}
assert.equal(resolveVariable('gust','0/2/22',catalogue).name,'10fg','Explicit legacy aliases disambiguate raw gust codes');
assert.equal(resolveVariable('unknown','0/0/0',catalogue).entry,null,'Ambiguous raw temperature code must not guess a statistical field');
const unknown=displayConfiguration({name:'unlisted'});assert.equal(unknown.unit,'unit unknown');assert.equal(unknown.factor,1);
const cfg=name=>displayConfiguration({name,variable:catalogue.variables[name]});
assert.equal(273.15*cfg('2t').factor+cfg('2t').offset,0);
assert.equal(.001*cfg('tp').factor,1);assert.equal(.5*cfg('tcc').factor,50);
assert.equal(cfg('nsss').unit,'N·s/m²');assert.equal(cfg('nsss').factor,1);
assert.equal(cfg('r').factor,1,'Relative humidity already uses percent');
assert.equal(formatReading(1e6,cfg('nsss')),'1.00e+6');
console.log('PASS: 53 catalogue units, raw unknown resolution, ambiguity rejection, display conversions and readable precision.');
