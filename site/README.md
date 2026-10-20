# AVENC GitHub Pages showcase

Static site at https://filippi.github.io/avenc/.

```bash
python3 -m http.server 8765 --directory site
# Open http://localhost:8765
node site/check.mjs
```

No build system, package install or API key is needed. JavaScript modules require
HTTP serving rather than opening index.html as a file.

- `assets/examples.js`: authoritative command/Python snippets, checked against AVENC 1.0.0.
- `assets/app.js`: walkthrough, copy buttons and dataset catalog. No scientific data
  are downloaded by the page; only the small local JSON inventory is fetched.
- `assets/datasets.json`: public Zenodo file sizes and URLs, checked 20 October 2026.
- `EXAMPLES.md`: accessible, downloadable equivalent of all eight workflows.
  Regenerate after editing snippets with `node site/check.mjs --write`.
- `assets/style.css`: responsive layout, code presentation and abstract workflow animation.

Autoplay advances every 16 seconds, pauses on interaction, stops while the page
is hidden, and is disabled for reduced-motion users. Tabs support arrow keys,
Home and End. Copy requires HTTPS or localhost clipboard permission.

The diagram and GRIBView illustration are schematic, not scientific output.
Google Fonts are optional; system fonts are the fallback. There is no analytics.

The existing `www/` browser viewer is separate and is not presented as a
Range-only dataset reader: it currently downloads the complete compressed file.

Deployment: `.github/workflows/pages.yml` publishes this directory on main when
site files change. Configure the repository Pages source as **GitHub Actions**.
