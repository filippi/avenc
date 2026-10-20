import { examples, base } from './examples.js';

const $ = (id) => document.getElementById(id);
const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');
let active = 0;
let playing = !reducedMotion.matches;
let elapsed = 0;
let previous = performance.now();
const duration = 16000;

// Highlight text without inserting source code as HTML.
function highlight(code, language) {
  const fragment = document.createDocumentFragment();
  const pattern = /(#.*$)|("[^"\n]*"|'[^'\n]*')|(--[a-z][a-z0-9-]*)|\b(import|from|with|as|print)\b/gm;
  let start = 0;
  for (const match of code.matchAll(pattern)) {
    fragment.append(document.createTextNode(code.slice(start, match.index)));
    const token = document.createElement('span');
    token.className = match[1] ? 'token-comment' : match[2] ? 'token-string' : match[3] ? 'token-flag' : 'token-keyword';
    token.textContent = match[0];
    fragment.append(token);
    start = match.index + match[0].length;
  }
  fragment.append(document.createTextNode(code.slice(start)));
  return fragment;
}

function setPlayback(value) {
  playing = value;
  $('play-toggle').textContent = playing ? 'Ⅱ Pause' : '▶ Play';
  $('play-toggle').setAttribute('aria-label', playing ? 'Pause walkthrough' : 'Play walkthrough');
  document.body.classList.toggle('paused', !playing);
}
function select(index, manual = false) {
  active = index;
  elapsed = 0;
  const example = examples[index];
  $('example-code').replaceChildren(highlight(example.code, example.language));
  $('example-code').className = `language-${example.language}`;
  $('file-label').textContent = example.file;
  $('output-type').textContent = example.type;
  $('output-label').textContent = example.type === 'MP4' ? 'your own archive' : 'your scientific workflow';
  $('transfer-label').textContent = example.transfer;
  $('example-kicker').textContent = example.kicker;
  $('example-title').textContent = example.title;
  $('example-description').textContent = example.description;
  $('example-note').textContent = example.note;
  $('example-source').href = example.source;
  $('step-counter').textContent = `${String(index + 1).padStart(2, '0')} / ${String(examples.length).padStart(2, '0')}`;
  for (const [i, tab] of [...$('scenario-tabs').children].entries()) {
    tab.setAttribute('aria-selected', String(i === index));
    tab.tabIndex = i === index ? 0 : -1;
  }
  $('demo-panel').setAttribute('aria-labelledby', `tab-${index}`);
  $('demo-progress').style.width = '0%';
  if (manual) setPlayback(false);
}

examples.forEach((example, index) => {
  const tab = document.createElement('button');
  tab.id = `tab-${index}`;
  tab.setAttribute('role', 'tab');
  tab.setAttribute('aria-controls', 'demo-panel');
  tab.textContent = example.tab;
  tab.addEventListener('click', () => select(index, true));
  tab.addEventListener('keydown', (event) => {
    let next;
    if (event.key === 'ArrowRight') next = (index + 1) % examples.length;
    if (event.key === 'ArrowLeft') next = (index - 1 + examples.length) % examples.length;
    if (event.key === 'Home') next = 0;
    if (event.key === 'End') next = examples.length - 1;
    if (next !== undefined) {
      event.preventDefault(); select(next, true); $(`tab-${next}`).focus();
    }
  });
  $('scenario-tabs').append(tab);
});
$('play-toggle').addEventListener('click', () => setPlayback(!playing));
$('demo-panel').addEventListener('focusin', () => setPlayback(false));
$('demo-panel').addEventListener('pointerdown', () => setPlayback(false));
reducedMotion.addEventListener('change', () => { if (reducedMotion.matches) setPlayback(false); });
function tick(now) {
  if (playing && !document.hidden) {
    elapsed += Math.min(now - previous, 250);
    if (elapsed >= duration) select((active + 1) % examples.length);
    $('demo-progress').style.width = `${100 * elapsed / duration}%`;
  }
  previous = now;
  requestAnimationFrame(tick);
}

let toastTimeout;
async function copy(text) {
  try {
    await navigator.clipboard.writeText(text);
    $('toast').textContent = 'Copied. Ready for your terminal or editor.';
  } catch {
    $('toast').textContent = 'Clipboard unavailable. Select the code and copy it manually.';
  }
  $('toast').classList.add('show');
  clearTimeout(toastTimeout);
  toastTimeout = setTimeout(() => $('toast').classList.remove('show'), 3000);
}
$('copy-example').addEventListener('click', () => copy(examples[active].code));
for (const button of document.querySelectorAll('[data-copy]')) button.addEventListener('click', () => copy(button.dataset.copy));

function size(bytes) {
  return bytes >= 1e9 ? `${(bytes / 1e9).toFixed(2)} GB` : `${(bytes / 1e6).toFixed(2)} MB`;
}
async function datasets() {
  const response = await fetch(new URL('./datasets.json', import.meta.url));
  if (!response.ok) throw new Error('Dataset inventory unavailable');
  const { files } = await response.json();
  const cases = [
    { id:'ERALANDWIND', tag:'01 · REANALYSIS', title:'A month of wind.', description:'ERA5-Land U10 and V10 · February 2022 · 672 hourly frames per component · 3600 × 1801 grid.', source:['ERA5_Land_10m_u_component_of_wind_2022_02.grib','ERA5_Land_10m_v_component_of_wind_2022_02.grib'] },
    { id:'LAMBC', tag:'02 · BOUNDARY CONDITIONS', title:'A model’s next step.', description:'Multi-variable forcing for a Meso-NH limited-area model experiment, with multiple levels and forecast steps.', source:['cepBC.grib'] },
    { id:'ENSEMBLE', tag:'03 · ENSEMBLE FORECASTS', title:'Many possible futures.', description:'ECMWF forecasts initialized 30 September 2026 · U10, V10 and T2m · +0 to +144 h every 3 h.', source:['ens_20260930_all.grib'] },
  ];
  for (const item of cases) {
    const file = files.find(f => f.name === `${item.id}.compact.mp4`);
    const originals = item.source.map(name => files.find(f => f.name === name));
    const bytes = originals.reduce((sum, f) => sum + f.bytes, 0);
    const card = document.createElement('article'); card.className = 'dataset-card';
    const tag = document.createElement('div'); tag.className='eyebrow';tag.textContent=item.tag;
    const title = document.createElement('h3');title.textContent=item.title;
    const description=document.createElement('p');description.textContent=item.description;
    const pair=document.createElement('div');pair.className='size-pair';
    const value=document.createElement('strong');value.textContent=size(file.bytes);
    const label=document.createElement('span');label.textContent='Compact MP4';pair.append(value,label);
    const ratio=document.createElement('div');ratio.className='ratio';ratio.textContent=`${(bytes/file.bytes).toFixed(1)}× vs ${size(bytes)} original GRIB`;
    const link=document.createElement('a');link.href=file.url;link.textContent='Compact MP4 on Zenodo ↗';
    const high=document.createElement('a');high.href=`${base}${item.id}.high.mp4`;high.textContent='High-fidelity MP4 ↗';
    card.append(tag,title,description,pair,ratio,link,high);$('dataset-cards').append(card);
  }
  for (const file of [...files].sort((a,b)=>a.name.localeCompare(b.name))) {
    const row=document.createElement('tr');
    const name=document.createElement('td');const link=document.createElement('a');link.href=file.url;link.textContent=file.name;name.append(link);
    const bytes=document.createElement('td');bytes.textContent=size(file.bytes);
    const format=document.createElement('td');format.textContent=file.name.endsWith('.mp4')?'AVENC / MP4':'Original GRIB';
    row.append(name,bytes,format);$('dataset-files').append(row);
  }
}
setPlayback(playing);select(0);requestAnimationFrame(tick);
datasets().catch(() => {
  const message=document.createElement('p');
  message.textContent='The file list could not load. Browse the complete archive on Zenodo using the link above.';
  $('dataset-cards').append(message);
});
