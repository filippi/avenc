import assert from 'node:assert/strict';
import {timelineInfo,tickLabel,positionLabel,formatTime,referenceTime} from '../js/avenc_timeline.js';
const archive=[{ms:Date.UTC(2003,0,1),record:{dataDate:20030101}},{ms:Date.UTC(2011,6,31),record:{dataDate:20110731}}],info=timelineInfo(archive);
assert(info.archive&&info.years);assert.match(tickLabel(archive[0].ms,info),/2003/);assert.match(tickLabel(archive[1].ms,info),/2011/);assert.match(formatTime(archive[1].ms),/2011/);assert.equal(positionLabel(archive[1],1,archive,info),'2 / 2');
const forecast=[{ms:Date.UTC(2026,11,31),record:{dataDate:20261231}},{ms:Date.UTC(2027,0,1),record:{dataDate:20261231}}],f=timelineInfo(forecast);
assert(!f.archive&&f.years);assert.equal(positionLabel(forecast[1],1,forecast,f),'+24 h');assert.match(tickLabel(forecast[1].ms,f),/2027/);assert(Number.isNaN(referenceTime({})));assert(!timelineInfo([forecast[0]]).archive);
assert(timelineInfo(archive.map(f=>({...f,record:{}}))).archive);
console.log('PASS: archive years/positions, cross-year forecast leads, missing reference dates and single-frame timeline');
