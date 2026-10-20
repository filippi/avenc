# Browser tools

Serve this folder on localhost (or HTTPS). WebCodecs support depends on browser,
codec, bit depth and device. FFmpeg is not used inside the browser.

```bash
python3 -m http.server 8000 --directory www
```

Place an AVENC MP4 under `www/datasets/` and open:
`http://localhost:8000/viewer.html?dataset=./datasets/example.mp4`.
`extractor.html` uses the same dataset parameter. The optional `index.php`
dataset list needs PHP: `php -S 127.0.0.1:8000 -t www`.

The metadata reader (`js/avenc_metadata.js`) issues HTTP Range requests. The
viewer/extractor share `js/avenc_model.js`, which currently downloads the entire
MP4 before decoding. Bounded decoder queues/frame caches reduce retained decoded
frames; they do not remove the initial whole-file download. JavaScript requests
to another origin also require suitable CORS headers. CDN-hosted MP4Box/fflate
are runtime dependencies and retain their own licences.

Run the synthetic tests with Node.js 18+:

```bash
node www/tests/metadata.mjs
node www/tests/decoder.mjs
```

They exercise metadata fetching and bounded decoder behavior with mocks; they do
not certify hardware codec support in every browser.
