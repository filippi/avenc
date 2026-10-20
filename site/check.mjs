// Lightweight, offline checks. Never execute dataset downloads or encodings.
import assert from 'node:assert/strict';
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { examples } from './assets/examples.js';
const root = fileURLToPath(new URL('.', import.meta.url));
const html = readFileSync(`${root}index.html`, 'utf8');
const { files } = JSON.parse(readFileSync(`${root}assets/datasets.json`));
assert.equal(examples.length, 8);
assert.equal(new Set(examples.map(e => e.tab)).size, examples.length);
for (const example of examples) {
  assert(example.code.includes('\n'), `${example.tab}: real multiline code`);
  assert(!example.code.includes('\\n'), `${example.tab}: no literal escaped newlines`);
  assert(example.source.startsWith('https://zenodo.org/records/23502851/files/'));
  const command = example.language === 'python' ? 'python3' : 'bash';
  const args = example.language === 'python' ? ['-c', 'import ast, sys; ast.parse(sys.stdin.read())'] : ['-n'];
  const syntax = spawnSync(command, args, { input: example.code, encoding: 'utf8' });
  assert.equal(syntax.status, 0, `${example.tab}: ${syntax.stderr}`);
}
for (const match of html.matchAll(/(?:src|href)="(assets\/[^"#]+)"/g)) assert(existsSync(root + match[1]), match[1]);
const ids = [...html.matchAll(/\bid="([^"]+)"/g)].map(m => m[1]);
assert.equal(ids.length, new Set(ids).size, 'unique HTML IDs');
for (const match of html.matchAll(/href="#([^"]+)"/g)) assert(ids.includes(match[1]), `anchor ${match[1]}`);
for (const name of ['ERALANDWIND.compact.mp4','LAMBC.compact.mp4','ENSEMBLE.compact.mp4']) assert(files.find(f => f.name === name));
const markdown = '# AVENC: public Zenodo examples\n\nInstall AVENC with `pip install avenc` and FFmpeg 5.1+ separately.\nThese commands execute on your machine; some reconstruct or download large files.\n\n' + examples.map(e => `## ${e.tab}: ${e.title}\n\n${e.description}\n\n\`\`\`${e.language}\n${e.code}\n\`\`\`\n\n${e.note}\n\n[Source file](${e.source})\n`).join('\n');
if (process.argv.includes('--write')) writeFileSync(`${root}EXAMPLES.md`, markdown);
else assert.equal(readFileSync(`${root}EXAMPLES.md`, 'utf8'), markdown, 'Markdown examples must match examples.js (node site/check.mjs --write)');
console.log(`PASS: ${examples.length} example syntaxes, ${files.length} catalog files, local assets, anchors and Markdown consistency`);
