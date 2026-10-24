#!/usr/bin/env python3
"""Refresh ECFlix's static MP4 list. Run after adding/removing files; no web backend."""
import argparse
import json
from pathlib import Path
from urllib.parse import quote

root = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--directory', type=Path, default=root / 'dataset')
parser.add_argument('--output', type=Path, default=root / 'datasets' / 'dataset.json')
args = parser.parse_args()
previous = json.loads(args.output.read_text()) if args.output.exists() else {}
labels = {item['url']: item.get('label') for item in previous.get('datasets', [])}
args.directory = args.directory.resolve()
try:
    args.directory.relative_to(root)
except ValueError:
    parser.error('--directory must be inside the website root')
files = sorted(args.directory.glob('*.mp4'), reverse=True)
items = []
for path in files:
    # Default layout: data files are alongside the website, outside source control.
    url = './' + quote(path.relative_to(root).as_posix())
    items.append({'url': url, 'label': labels.get(url) or path.name, 'bytes': path.stat().st_size})
# Preserve manually curated remote datasets when refreshing the local list.
items.extend(item for item in previous.get('datasets', []) if item.get('url', '').startswith(('https://', 'http://')))
default = previous.get('default', './dataset/20261021.browser_svt10.mp4')
if default not in {item['url'] for item in items}:
    default = items[0]['url'] if items else ''
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps({'schema': 'ecflix.datasets.v1', 'default': default, 'datasets': items}, indent=2) + '\n')
print(f'{len(items)} datasets written to {args.output}')
