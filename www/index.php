<?php
declare(strict_types=1);

$baseDir = __DIR__ . DIRECTORY_SEPARATOR . 'datasets';
$webPrefix = './datasets/';
$rows = [];

if (is_dir($baseDir)) {
    $entries = scandir($baseDir);
    if ($entries !== false) {
        foreach ($entries as $name) {
            if ($name === '.' || $name === '..') {
                continue;
            }
            $full = $baseDir . DIRECTORY_SEPARATOR . $name;
            if (!is_file($full)) {
                continue;
            }
            if (!preg_match('/\.mp4$/i', $name)) {
                continue;
            }
            $rows[] = [
                'name' => $name,
                'url' => $webPrefix . rawurlencode($name),
                'size' => filesize($full) ?: 0,
                'mtime' => filemtime($full) ?: 0,
            ];
        }
    }
}

usort($rows, static function (array $a, array $b): int {
    return strnatcasecmp($a['name'], $b['name']);
});

function human_bytes(int $bytes): string {
    if ($bytes <= 0) return '0 B';
    $units = ['B', 'KB', 'MB', 'GB', 'TB'];
    $v = (float)$bytes;
    $u = 0;
    while ($v >= 1024.0 && $u < count($units) - 1) {
        $v /= 1024.0;
        $u++;
    }
    $d = ($v >= 100.0) ? 0 : (($v >= 10.0) ? 1 : 2);
    return number_format($v, $d) . ' ' . $units[$u];
}
?><!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>AVENC Dataset List</title>
  <style>
    :root {
      --bg: #0a111a;
      --panel: #111b27;
      --line: #20344a;
      --txt: #d9e2ef;
      --muted: #8fa2ba;
      --accent: #2db1ff;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: "IBM Plex Sans", "Segoe UI", sans-serif;
      color: var(--txt);
      background: radial-gradient(circle at 10% 5%, #122336 0%, var(--bg) 46%);
    }
    .wrap {
      max-width: 1500px;
      margin: 0 auto;
      padding: 14px;
      display: grid;
      gap: 12px;
    }
    .panel {
      border: 1px solid var(--line);
      border-radius: 10px;
      background: linear-gradient(180deg, rgba(255,255,255,0.03), rgba(255,255,255,0.01));
      padding: 12px;
    }
    .header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 10px;
      flex-wrap: wrap;
    }
    .header h1 {
      margin: 0;
      font-size: 20px;
      font-weight: 700;
    }
    .header a {
      color: var(--accent);
      text-decoration: none;
      font-weight: 600;
    }
    .header a:hover { text-decoration: underline; }
    .grid {
      display: grid;
      grid-template-columns: minmax(660px, 1fr) 460px;
      gap: 12px;
      align-items: start;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 13px;
    }
    th, td {
      border-bottom: 1px solid var(--line);
      padding: 8px 6px;
      text-align: left;
      vertical-align: top;
    }
    th {
      color: var(--muted);
      font-weight: 600;
      font-size: 12px;
      text-transform: uppercase;
      letter-spacing: 0.03em;
    }
    td code {
      color: #cfe4ff;
      font-size: 12px;
      word-break: break-word;
    }
    .actions {
      display: flex;
      gap: 8px;
      flex-wrap: wrap;
    }
    .btn, button {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      min-height: 28px;
      min-width: 88px;
      border: 1px solid #356088;
      border-radius: 8px;
      background: #123155;
      color: var(--txt);
      text-decoration: none;
      font-size: 12px;
      padding: 0 10px;
      cursor: pointer;
    }
    .btn:hover, button:hover { background: #17406f; }
    .status {
      min-height: 20px;
      color: var(--muted);
      font-size: 12px;
      margin-bottom: 8px;
    }
    pre {
      margin: 0;
      white-space: pre-wrap;
      word-break: break-word;
      font-family: ui-monospace, Menlo, Consolas, monospace;
      font-size: 12px;
      line-height: 1.45;
      color: #cfe2f8;
      max-height: 74vh;
      overflow: auto;
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 10px;
      background: rgba(7, 14, 24, 0.6);
    }
    .meta-top {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 8px;
      margin-bottom: 8px;
      font-size: 12px;
      color: var(--muted);
    }
    .meta-top div {
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 6px 8px;
      background: rgba(7, 14, 24, 0.5);
    }
    @media (max-width: 1200px) {
      .grid { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="panel header">
      <h1>AVENC Datasets</h1>
      <div>
        <a href="./viewer.html">Open Viewer</a>
        <span style="color:var(--muted);margin:0 8px;">|</span>
        <a href="./extractor.html">Open Extractor</a>
        <span style="color:var(--muted);margin:0 8px;">|</span>
        <a href="./about.html">About</a>
      </div>
    </div>

    <div class="grid">
      <div class="panel">
        <table>
          <thead>
            <tr>
              <th>File</th>
              <th>Size</th>
              <th>Modified</th>
              <th>Actions</th>
            </tr>
          </thead>
          <tbody>
            <?php if (count($rows) === 0): ?>
              <tr><td colspan="4">No MP4 found in <code>www/datasets</code>.</td></tr>
            <?php else: ?>
              <?php foreach ($rows as $row): ?>
                <?php
                  $url = $row['url'] . '?v=' . $row['mtime'] . '-' . $row['size'];
                  $viewer = './viewer.html?dataset=' . rawurlencode($url);
                  $extractor = './extractor.html?dataset=' . rawurlencode($url);
                  $mtime = $row['mtime'] > 0 ? date('Y-m-d H:i:s', $row['mtime']) : '-';
                ?>
                <tr>
                  <td><code><?= htmlspecialchars($row['name'], ENT_QUOTES, 'UTF-8') ?></code></td>
                  <td><?= htmlspecialchars(human_bytes((int)$row['size']), ENT_QUOTES, 'UTF-8') ?></td>
                  <td><?= htmlspecialchars($mtime, ENT_QUOTES, 'UTF-8') ?></td>
                  <td>
                    <div class="actions">
                      <a class="btn" href="<?= htmlspecialchars($viewer, ENT_QUOTES, 'UTF-8') ?>">Open in viewer</a>
                      <a class="btn" href="<?= htmlspecialchars($extractor, ENT_QUOTES, 'UTF-8') ?>">Open in extractor</a>
                      <button type="button" class="meta-btn" data-url="<?= htmlspecialchars($url, ENT_QUOTES, 'UTF-8') ?>">Metadata</button>
                    </div>
                  </td>
                </tr>
              <?php endforeach; ?>
            <?php endif; ?>
          </tbody>
        </table>
      </div>

      <div class="panel">
        <label><input type="checkbox" id="fastMode" checked> Fast metadata</label>
        <div class="status">Uncheck to include video codec details.</div>
        <div class="status" id="metaStatus">Select a dataset row and click <strong>Metadata</strong>.</div>
        <div class="meta-top">
          <div>Dataset: <span id="metaDataset">-</span></div>
          <div>Parse time: <span id="metaTime">-</span></div>
          <div>Codec(s): <span id="metaCodecs">-</span></div>
          <div>Bit depth(s): <span id="metaBits">-</span></div>
        </div>
        <pre id="metaJson">{
  "info": "metadata panel"
}</pre>
      </div>
    </div>
  </div>

  <script src="https://cdn.jsdelivr.net/npm/mp4box@0.5.3/dist/mp4box.all.min.js"></script>
  <script type="module">
    import { inspectAvencDataset, formatBytes } from "./js/avenc_metadata.js";

    const statusEl = document.getElementById("metaStatus");
    const jsonEl = document.getElementById("metaJson");
    const datasetEl = document.getElementById("metaDataset");
    const timeEl = document.getElementById("metaTime");
    const codecsEl = document.getElementById("metaCodecs");
    const bitsEl = document.getElementById("metaBits");

    const fastMode = document.getElementById("fastMode");
    const summaries = new Map();
    let activeRequest = null;
    let selectedUrl = "";

    async function loadMeta(url) {
      activeRequest?.abort();
      const request = new AbortController();
      activeRequest = request;
      selectedUrl = url;
      const fast = fastMode.checked;
      const cacheKey = `${fast}:${url}`;
      datasetEl.textContent = url;
      timeEl.textContent = "...";
      codecsEl.textContent = "...";
      bitsEl.textContent = "...";
      statusEl.textContent = `Parsing metadata (range scan): ${url}`;
      jsonEl.textContent = "{\n  \"loading\": true\n}";
      let meta = summaries.get(cacheKey);
      const cached = Boolean(meta);
      try {
        if (!meta) meta = await inspectAvencDataset(url, { fast, signal: request.signal });
      } catch (err) {
        if (request.signal.aborted) return;
        statusEl.textContent = `Metadata parse failed: ${err.message}`;
        return;
      }
      if (request.signal.aborted) return;
      if (meta.ok && !cached) {
        summaries.set(cacheKey, meta);
        if (summaries.size > 8) summaries.delete(summaries.keys().next().value);
      }
      const out = { ...meta };
      if (typeof out.file_size_bytes === "number") {
        out.file_size_human = formatBytes(out.file_size_bytes);
      }
      if (typeof out.raw32_equivalent_total_bytes === "number") {
        out.raw32_equivalent_total_human = formatBytes(out.raw32_equivalent_total_bytes);
      }
      const codecs =
        Array.isArray(out.codecs) && out.codecs.length > 0
          ? out.codecs
          : (Array.isArray(out.codec_families) ? out.codec_families : []);
      const bits = Array.isArray(out.bit_depths) ? out.bit_depths : [];
      codecsEl.textContent = codecs.length > 0 ? codecs.join(", ") : (fast ? "Full inspection required" : "-");
      bitsEl.textContent = bits.length > 0 ? bits.map((b) => `${b}-bit`).join(", ") : "-";
      timeEl.textContent = `${Number(out.parse_time_ms || 0).toFixed(3)} ms`;
      if (out.ok) {
        statusEl.textContent = `Metadata parsed (${out.marker_prefix}, ${cached ? "cached; no new reads" : `${timeEl.textContent}, ${formatBytes(out.bytes_fetched)} fetched`}).`;
      } else {
        statusEl.textContent = `Metadata parse failed: ${out.error || "unknown error"}`;
      }
      jsonEl.textContent = JSON.stringify(out, null, 2);
    }

    fastMode.addEventListener("change", () => { if (selectedUrl) loadMeta(selectedUrl); });

    document.querySelectorAll(".meta-btn").forEach((btn) => {
      btn.addEventListener("click", async () => {
        const url = btn.dataset.url || "";
        if (!url) return;
        await loadMeta(url);
      });
    });
  </script>
</body>
</html>
