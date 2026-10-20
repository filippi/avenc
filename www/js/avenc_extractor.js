import { AvencModel } from "./avenc_model.js";
import * as zarr from "https://cdn.jsdelivr.net/npm/zarrita/+esm";
import { zipSync, strToU8 } from "https://cdn.jsdelivr.net/npm/fflate@0.8.2/esm/browser.js";

function clamp(v, lo, hi) {
  return Math.max(lo, Math.min(hi, v));
}

function safeNum(v, fallback = 0) {
  const x = Number(v);
  return Number.isFinite(x) ? x : fallback;
}

function sanitizeName(text, fallback = "subset") {
  const raw = String(text || "").trim();
  if (!raw) return fallback;
  const cleaned = raw.replace(/[^A-Za-z0-9._-]+/g, "_").replace(/^_+|_+$/g, "");
  return cleaned || fallback;
}

function triggerDownload(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 5000);
}

const VERT_SHADER = `
attribute vec2 a_pos;
varying vec2 v_uv;
void main() {
  v_uv = (a_pos + 1.0) * 0.5;
  v_uv.y = 1.0 - v_uv.y;
  gl_Position = vec4(a_pos, 0.0, 1.0);
}
`;

const FRAG_SHADER = `
precision highp float;
varying vec2 v_uv;
uniform sampler2D u_video;
uniform vec2 u_cropScale;
uniform vec2 u_quantRange;
uniform vec2 u_displayRange;
uniform int u_colormap;

vec3 cm_jet(float t) {
  float r = clamp(1.5 - abs(4.0 * t - 3.0), 0.0, 1.0);
  float g = clamp(1.5 - abs(4.0 * t - 2.0), 0.0, 1.0);
  float b = clamp(1.5 - abs(4.0 * t - 1.0), 0.0, 1.0);
  return vec3(r, g, b);
}

vec3 cm_viridis(float t) {
  const vec3 c0 = vec3(0.2803, 0.1652, 0.4762);
  const vec3 c1 = vec3(0.1225, 0.4720, 0.7060);
  const vec3 c2 = vec3(0.3690, 0.7880, 0.3820);
  const vec3 c3 = vec3(0.9930, 0.9060, 0.1440);
  float x = clamp(t, 0.0, 1.0);
  if (x < 0.33) return mix(c0, c1, x / 0.33);
  if (x < 0.66) return mix(c1, c2, (x - 0.33) / 0.33);
  return mix(c2, c3, (x - 0.66) / 0.34);
}

void main() {
  vec2 tc = vec2(v_uv.x * u_cropScale.x, v_uv.y * u_cropScale.y);
  float y = texture2D(u_video, tc).r;
  float value = mix(u_quantRange.x, u_quantRange.y, clamp(y, 0.0, 1.0));
  float span = max(1e-12, u_displayRange.y - u_displayRange.x);
  float norm = clamp((value - u_displayRange.x) / span, 0.0, 1.0);
  vec3 color;
  if (u_colormap == 2) color = vec3(norm, norm, norm);
  else if (u_colormap == 1) color = cm_jet(norm);
  else color = cm_viridis(norm);
  gl_FragColor = vec4(color, 1.0);
}
`;

function createShader(gl, type, source) {
  const shader = gl.createShader(type);
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    throw new Error(gl.getShaderInfoLog(shader) || "Shader compile failed");
  }
  return shader;
}

function createProgram(gl, vsSource, fsSource) {
  const vs = createShader(gl, gl.VERTEX_SHADER, vsSource);
  const fs = createShader(gl, gl.FRAGMENT_SHADER, fsSource);
  const program = gl.createProgram();
  gl.attachShader(program, vs);
  gl.attachShader(program, fs);
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    throw new Error(gl.getProgramInfoLog(program) || "Program link failed");
  }
  return program;
}

class AvencWebGLRenderer {
  constructor(canvas) {
    const gl = canvas.getContext("webgl", { preserveDrawingBuffer: false, antialias: false });
    if (!gl) throw new Error("WebGL is not available in this browser.");
    this.gl = gl;
    this.canvas = canvas;
    this.program = createProgram(gl, VERT_SHADER, FRAG_SHADER);
    gl.useProgram(this.program);

    this.aPos = gl.getAttribLocation(this.program, "a_pos");
    this.uVideo = gl.getUniformLocation(this.program, "u_video");
    this.uCropScale = gl.getUniformLocation(this.program, "u_cropScale");
    this.uQuantRange = gl.getUniformLocation(this.program, "u_quantRange");
    this.uDisplayRange = gl.getUniformLocation(this.program, "u_displayRange");
    this.uColormap = gl.getUniformLocation(this.program, "u_colormap");

    const quad = new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]);
    this.vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, quad, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(this.aPos);
    gl.vertexAttribPointer(this.aPos, 2, gl.FLOAT, false, 0, 0);

    this.tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  }

  resizeTo(width, height) {
    this.canvas.width = width;
    this.canvas.height = height;
    this.gl.viewport(0, 0, width, height);
  }

  render(videoFrame, uniforms) {
    const gl = this.gl;
    gl.useProgram(this.program);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, videoFrame);
    gl.uniform1i(this.uVideo, 0);
    gl.uniform2f(this.uCropScale, uniforms.cropScaleX, uniforms.cropScaleY);
    gl.uniform2f(this.uQuantRange, uniforms.quantMin, uniforms.quantMax);
    gl.uniform2f(this.uDisplayRange, uniforms.displayMin, uniforms.displayMax);
    const cmap = uniforms.colormap === "gray" ? 2 : (uniforms.colormap === "jet" ? 1 : 0);
    gl.uniform1i(this.uColormap, cmap);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  }
}

class AvencExtractor {
  constructor() {
    this.model = new AvencModel([]);
    this.groupSelect = document.getElementById("groupSelect");
    this.colormapSelect = document.getElementById("colormapSelect");
    this.rateSelect = document.getElementById("rateSelect");
    this.playBtn = document.getElementById("playBtn");
    this.timeline = document.getElementById("timeline");
    this.frameLabel = document.getElementById("frameLabel");
    this.timeLabel = document.getElementById("timeLabel");
    this.startSlider = document.getElementById("startSlider");
    this.endSlider = document.getElementById("endSlider");
    this.startFrameInput = document.getElementById("startFrameInput");
    this.endFrameInput = document.getElementById("endFrameInput");
    this.startTimeInput = document.getElementById("startTimeInput");
    this.endTimeInput = document.getElementById("endTimeInput");
    this.startTimeLabel = document.getElementById("startTimeLabel");
    this.endTimeLabel = document.getElementById("endTimeLabel");

    this.statusText = document.getElementById("statusText");
    this.decodeProgressWrap = document.getElementById("decodeProgressWrap");
    this.decodeProgressFill = document.getElementById("decodeProgressFill");

    this.canvasWrap = document.getElementById("canvasWrap");
    this.canvas = document.getElementById("glCanvas");
    this.roiOverlay = document.getElementById("roiOverlay");
    this.roiBox = document.getElementById("roiBox");
    this.renderer = new AvencWebGLRenderer(this.canvas);

    this.niStartInput = document.getElementById("niStartInput");
    this.niEndInput = document.getElementById("niEndInput");
    this.njStartInput = document.getElementById("njStartInput");
    this.njEndInput = document.getElementById("njEndInput");
    this.roiWidthInput = document.getElementById("roiWidthInput");
    this.roiHeightInput = document.getElementById("roiHeightInput");

    this.latNorthInput = document.getElementById("latNorthInput");
    this.latSouthInput = document.getElementById("latSouthInput");
    this.lonWestInput = document.getElementById("lonWestInput");
    this.lonEastInput = document.getElementById("lonEastInput");

    this.extractZarrBtn = document.getElementById("extractZarrBtn");
    this.selectionInfo = document.getElementById("selectionInfo");

    this.currentFrame = 0;
    this.playing = false;
    this.lastTick = 0;
    this.playbackHoursPerSecond = Number(this.rateSelect.value || 1);

    this.startFrame = 0;
    this.endFrame = 0;
    this.frameLabels = [];
    this.frameEpochMs = [];
    this.uiSync = false;

    this.roi = null;
    this.dragState = null;
    this.rowLat = [];
    this.colLon = [];
    this.currentDatasetUrl = "";
  }

  getRequestedDatasetFromUrl() {
    try {
      const u = new URL(window.location.href);
      return (u.searchParams.get("dataset") || "").trim();
    } catch {
      return "";
    }
  }

  setDecodeProgress(percent, active) {
    const pct = Math.max(0, Math.min(100, Number(percent || 0)));
    this.decodeProgressFill.style.width = `${pct.toFixed(1)}%`;
    if (active) this.decodeProgressWrap.classList.add("active");
    else this.decodeProgressWrap.classList.remove("active");
  }

  bindEvents() {
    window.addEventListener("pagehide", () => { this.pause(); this.model.close(); });
    window.addEventListener("pageshow", ev => {
      if (ev.persisted && this.model.currentDataset) this.loadDatasetByUrl(this.model.currentDataset);
    });
    this.model.addEventListener("status", (ev) => {
      this.statusText.textContent = ev.detail.message || "";
      if (/decoded|error|loaded|ready/i.test(this.statusText.textContent)) {
        this.setDecodeProgress(0, false);
      }
    });

    this.model.addEventListener("progress", (ev) => {
      const d = ev.detail;
      const pct = d.total > 0 ? (100 * d.value / d.total) : 0;
      this.statusText.textContent = `${d.step}: ${d.value}/${d.total} ${pct.toFixed(1)}% ${d.label || ""}`.trim();
      this.setDecodeProgress(pct, true);
    });

    this.model.addEventListener("loaded", () => {
      this.refreshGroupMenu();
    });

    this.groupSelect.addEventListener("change", async () => {
      this.pause();
      this.setDecodeProgress(0, false);
      const prev = this.getRoiNormalized();
      const prevRange = this.getRangeNormalized();
      await this.model.selectGroup(Number(this.groupSelect.value));
      this.initializeGroupState(prev, prevRange);
      await this.renderCurrentFrame();
      this.updateSelectionInfo();
    });

    this.colormapSelect.addEventListener("change", () => {
      this.renderCurrentFrame();
    });

    this.rateSelect.addEventListener("change", () => {
      this.playbackHoursPerSecond = Number(this.rateSelect.value || 1);
    });

    this.playBtn.addEventListener("click", () => this.togglePlay());

    this.timeline.addEventListener("input", async () => {
      this.currentFrame = clamp(Number(this.timeline.value || 0), 0, Math.max(0, this.model.getCurrentFrameCount() - 1));
      await this.renderCurrentFrame();
    });

    this.startSlider.addEventListener("input", async () => {
      const value = clamp(Number(this.startSlider.value || 0), 0, this.endFrame);
      this.startFrame = value;
      if (this.currentFrame < this.startFrame) this.currentFrame = this.startFrame;
      this.syncRangeControls("start-slider");
      await this.renderCurrentFrame();
      this.updateSelectionInfo();
    });

    this.endSlider.addEventListener("input", async () => {
      const n = Math.max(0, this.model.getCurrentFrameCount() - 1);
      const value = clamp(Number(this.endSlider.value || n), this.startFrame, n);
      this.endFrame = value;
      if (this.currentFrame > this.endFrame) this.currentFrame = this.endFrame;
      this.syncRangeControls("end-slider");
      await this.renderCurrentFrame();
      this.updateSelectionInfo();
    });

    this.startFrameInput.addEventListener("change", async () => {
      const value = clamp(Number(this.startFrameInput.value || 0), 0, this.endFrame);
      this.startFrame = value;
      if (this.currentFrame < this.startFrame) this.currentFrame = this.startFrame;
      this.syncRangeControls("start-frame");
      await this.renderCurrentFrame();
      this.updateSelectionInfo();
    });

    this.endFrameInput.addEventListener("change", async () => {
      const n = Math.max(0, this.model.getCurrentFrameCount() - 1);
      const value = clamp(Number(this.endFrameInput.value || n), this.startFrame, n);
      this.endFrame = value;
      if (this.currentFrame > this.endFrame) this.currentFrame = this.endFrame;
      this.syncRangeControls("end-frame");
      await this.renderCurrentFrame();
      this.updateSelectionInfo();
    });

    this.startTimeInput.addEventListener("change", async () => {
      const idx = this.findNearestFrameByTimeText(this.startTimeInput.value);
      if (idx == null) return;
      this.startFrame = clamp(idx, 0, this.endFrame);
      if (this.currentFrame < this.startFrame) this.currentFrame = this.startFrame;
      this.syncRangeControls("start-time");
      await this.renderCurrentFrame();
      this.updateSelectionInfo();
    });

    this.endTimeInput.addEventListener("change", async () => {
      const idx = this.findNearestFrameByTimeText(this.endTimeInput.value);
      if (idx == null) return;
      this.endFrame = clamp(idx, this.startFrame, Math.max(0, this.model.getCurrentFrameCount() - 1));
      if (this.currentFrame > this.endFrame) this.currentFrame = this.endFrame;
      this.syncRangeControls("end-time");
      await this.renderCurrentFrame();
      this.updateSelectionInfo();
    });

    const pixelInputs = [
      this.niStartInput,
      this.niEndInput,
      this.njStartInput,
      this.njEndInput,
      this.roiWidthInput,
      this.roiHeightInput
    ];
    for (const el of pixelInputs) {
      el.addEventListener("change", () => this.applyRoiFromPixelInputs());
    }

    const latLonInputs = [this.latNorthInput, this.latSouthInput, this.lonWestInput, this.lonEastInput];
    for (const el of latLonInputs) {
      el.addEventListener("change", () => this.applyRoiFromLatLonInputs());
    }

    const handles = this.roiBox.querySelectorAll(".handle");
    handles.forEach((h) => {
      h.addEventListener("pointerdown", (ev) => {
        ev.preventDefault();
        ev.stopPropagation();
        const handle = h.dataset.handle || "";
        this.startDrag("resize", ev, { handle });
      });
    });

    this.roiBox.addEventListener("pointerdown", (ev) => {
      if (ev.target && ev.target.classList && ev.target.classList.contains("handle")) return;
      ev.preventDefault();
      this.startDrag("move", ev, {});
    });

    this.canvasWrap.addEventListener("pointerdown", (ev) => {
      const target = ev.target;
      if (target === this.roiBox || (target && target.classList && target.classList.contains("handle"))) return;
      ev.preventDefault();
      this.startDrag("draw", ev, {});
    });

    window.addEventListener("resize", () => this.updateRoiOverlay());

    this.extractZarrBtn.addEventListener("click", async () => {
      if (this.extractZarrBtn.disabled) return;
      this.extractZarrBtn.disabled = true;
      const prevLabel = this.extractZarrBtn.textContent;
      this.extractZarrBtn.textContent = "Extracting...";
      try {
        await this.extractSelectionToZarr();
      } catch (err) {
        this.statusText.textContent = `Zarr extraction failed: ${err?.message || err}`;
      } finally {
        this.extractZarrBtn.disabled = false;
        this.extractZarrBtn.textContent = prevLabel || "Extract Zarr";
      }
    });
  }

  async initialize() {
    this.bindEvents();
    this.colormapSelect.value = "gray";
    const dataset = this.getRequestedDatasetFromUrl();
    if (!dataset) {
      this.statusText.textContent = "Missing dataset parameter. Open via index.php or add ?dataset=./datasets/<file>.mp4";
      return;
    }
    await this.loadDatasetByUrl(dataset);
  }

  refreshGroupMenu() {
    const groups = this.model.getGroupList();
    this.groupSelect.innerHTML = "";
    for (const g of groups) {
      const opt = document.createElement("option");
      opt.value = String(g.id);
      opt.textContent = g.label;
      this.groupSelect.appendChild(opt);
    }
    if (groups.length) this.groupSelect.value = String(groups[0].id);
  }

  getRoiNormalized() {
    const gi = this.model.getCurrentGroupInfo();
    if (!this.roi || !gi) return null;
    const nx = Math.max(1, gi.cropWidth);
    const ny = Math.max(1, gi.cropHeight);
    return {
      x0: this.roi.ni0 / nx,
      x1: (this.roi.ni1 + 1) / nx,
      y0: this.roi.nj0 / ny,
      y1: (this.roi.nj1 + 1) / ny
    };
  }

  getRangeNormalized() {
    const n = Math.max(1, this.model.getCurrentFrameCount());
    return {
      t0: this.startFrame / n,
      t1: this.endFrame / n,
      tc: this.currentFrame / n
    };
  }

  initializeGroupState(prevRoiNorm = null, prevRangeNorm = null) {
    const gi = this.model.getCurrentGroupInfo();
    if (!gi) return;

    this.buildAxisCaches();
    this.buildFrameTimeCaches();

    const n = Math.max(1, this.model.getCurrentFrameCount());
    if (prevRangeNorm) {
      this.startFrame = clamp(Math.floor(prevRangeNorm.t0 * n), 0, n - 1);
      this.endFrame = clamp(Math.floor(prevRangeNorm.t1 * n), this.startFrame, n - 1);
      this.currentFrame = clamp(Math.floor(prevRangeNorm.tc * n), this.startFrame, this.endFrame);
    } else {
      this.startFrame = 0;
      this.endFrame = n - 1;
      this.currentFrame = clamp(this.currentFrame, 0, n - 1);
    }

    if (prevRoiNorm) {
      const nx = Math.max(1, gi.cropWidth);
      const ny = Math.max(1, gi.cropHeight);
      const ni0 = clamp(Math.floor(prevRoiNorm.x0 * nx), 0, nx - 1);
      const ni1 = clamp(Math.ceil(prevRoiNorm.x1 * nx) - 1, ni0, nx - 1);
      const nj0 = clamp(Math.floor(prevRoiNorm.y0 * ny), 0, ny - 1);
      const nj1 = clamp(Math.ceil(prevRoiNorm.y1 * ny) - 1, nj0, ny - 1);
      this.roi = this.clampRoi({ ni0, ni1, nj0, nj1 });
    } else if (!this.roi) {
      const nx = Math.max(1, gi.cropWidth);
      const ny = Math.max(1, gi.cropHeight);
      const w = Math.max(1, Math.floor(nx * 0.28));
      const h = Math.max(1, Math.floor(ny * 0.28));
      const ni0 = Math.max(0, Math.floor((nx - w) / 2));
      const nj0 = Math.max(0, Math.floor((ny - h) / 2));
      this.roi = this.clampRoi({ ni0, nj0, ni1: ni0 + w - 1, nj1: nj0 + h - 1 });
    } else {
      this.roi = this.clampRoi(this.roi);
    }

    this.syncRangeControls("init");
    this.syncRoiInputsFromState();
    this.updateRoiOverlay();
    this.updateSelectionInfo();
  }

  buildAxisCaches() {
    this.rowLat = [];
    this.colLon = [];
    const gi = this.model.getCurrentGroupInfo();
    if (!gi) return;
    const nx = Math.max(1, gi.cropWidth);
    const ny = Math.max(1, gi.cropHeight);
    const iMid = Math.floor(nx / 2);
    const jMid = Math.floor(ny / 2);

    for (let j = 0; j < ny; j += 1) {
      const ll = this.model.getLatLonAt(iMid, j);
      this.rowLat.push(Number(ll?.lat));
    }
    for (let i = 0; i < nx; i += 1) {
      const ll = this.model.getLatLonAt(i, jMid);
      this.colLon.push(Number(ll?.lon));
    }
  }

  buildFrameTimeCaches() {
    const n = this.model.getCurrentFrameCount();
    this.frameLabels = new Array(n).fill("");
    this.frameEpochMs = new Array(n).fill(NaN);
    const records = Array.isArray(this.model.currentGroup?.records) ? this.model.currentGroup.records : [];
    for (const rec of records) {
      const idx = Number(rec?.frame_index);
      if (!Number.isInteger(idx) || idx < 0 || idx >= n) continue;
      const ts = String(rec?.timestamp || "").trim();
      this.frameLabels[idx] = ts || this.model.getFrameTimestampLabel(idx);
      const ms = Date.parse(this.frameLabels[idx]);
      this.frameEpochMs[idx] = Number.isFinite(ms) ? ms : NaN;
    }

    for (let i = 0; i < n; i += 1) {
      if (!this.frameLabels[i]) {
        this.frameLabels[i] = this.model.getFrameTimestampLabel(i);
        const ms = Date.parse(this.frameLabels[i]);
        this.frameEpochMs[i] = Number.isFinite(ms) ? ms : NaN;
      }
    }
  }

  getFrameLabel(idx) {
    const i = clamp(Number(idx || 0), 0, Math.max(0, this.frameLabels.length - 1));
    return this.frameLabels[i] || this.model.getFrameTimestampLabel(i) || "--";
  }

  findNearestFrameByTimeText(text) {
    const ms = Date.parse(String(text || "").trim());
    if (!Number.isFinite(ms)) return null;
    let bestIdx = null;
    let bestDist = Number.POSITIVE_INFINITY;
    for (let i = 0; i < this.frameEpochMs.length; i += 1) {
      const t = this.frameEpochMs[i];
      if (!Number.isFinite(t)) continue;
      const d = Math.abs(t - ms);
      if (d < bestDist) {
        bestDist = d;
        bestIdx = i;
      }
    }
    if (bestIdx == null) return null;
    return bestIdx;
  }

  syncRangeControls(reason = "") {
    const n = Math.max(1, this.model.getCurrentFrameCount());
    const max = n - 1;
    this.startFrame = clamp(this.startFrame, 0, max);
    this.endFrame = clamp(this.endFrame, this.startFrame, max);
    this.currentFrame = clamp(this.currentFrame, 0, max);

    this.uiSync = true;
    this.timeline.min = "0";
    this.timeline.max = String(max);
    this.timeline.value = String(this.currentFrame);

    this.startSlider.min = "0";
    this.startSlider.max = String(max);
    this.startSlider.value = String(this.startFrame);

    this.endSlider.min = "0";
    this.endSlider.max = String(max);
    this.endSlider.value = String(this.endFrame);

    this.startFrameInput.min = "0";
    this.startFrameInput.max = String(max);
    this.startFrameInput.value = String(this.startFrame);

    this.endFrameInput.min = "0";
    this.endFrameInput.max = String(max);
    this.endFrameInput.value = String(this.endFrame);

    const sLabel = this.getFrameLabel(this.startFrame);
    const eLabel = this.getFrameLabel(this.endFrame);
    this.startTimeLabel.textContent = sLabel;
    this.endTimeLabel.textContent = eLabel;
    this.startTimeInput.value = sLabel;
    this.endTimeInput.value = eLabel;

    this.uiSync = false;
    if (reason) {
      this.statusText.textContent = `Selection time range: [${this.startFrame}, ${this.endFrame}]`;
    }
  }

  async loadDatasetByUrl(url) {
    this.pause();
    this.setDecodeProgress(0, false);
    try {
      this.currentDatasetUrl = String(url || "");
      await this.model.loadDatasetByUrl(url);
      this.initializeGroupState(null, null);
      await this.renderCurrentFrame();
      this.updateSelectionInfo();
    } catch (err) {
      this.statusText.textContent = `Load error: ${err.message || err}`;
    }
  }

  async renderCurrentFrame() {
    const renderRequest = this._renderRequest = (this._renderRequest || 0) + 1;
    const n = this.model.getCurrentFrameCount();
    if (!n) return;
    this.currentFrame = clamp(this.currentFrame, 0, n - 1);

    try {
      const frameVis = await this.model.getFrameVisual(this.currentFrame, "frame");
      if (renderRequest !== this._renderRequest) return;
      this.renderer.resizeTo(frameVis.cropWidth, frameVis.cropHeight);
      this.renderer.render(frameVis.frame, {
        cropScaleX: frameVis.cropWidth / Math.max(1, frameVis.encodedWidth),
        cropScaleY: frameVis.cropHeight / Math.max(1, frameVis.encodedHeight),
        quantMin: frameVis.quantMin,
        quantMax: frameVis.quantMax,
        displayMin: frameVis.displayMin,
        displayMax: frameVis.displayMax,
        colormap: this.colormapSelect.value
      });
      this.frameLabel.textContent = `frame=${this.currentFrame}`;
      this.timeLabel.textContent = `time=${this.getFrameLabel(this.currentFrame)}`;
      this.timeline.value = String(this.currentFrame);
      this.updateRoiOverlay();
      this.updateSelectionInfo();
    } catch (err) {
      if (err.name === "AbortError" || renderRequest !== this._renderRequest) return;
      this.statusText.textContent = `Render error: ${err.message || err}`;
    }
  }

  play() {
    if (this.playing) return;
    this.playing = true;
    this.playBtn.textContent = "Pause";
    this.lastTick = performance.now();

    const tick = async (now) => {
      if (!this.playing) return;
      const dt = now - this.lastTick;
      const frameStepHours = Math.max(1e-6, Number(this.model.getFrameStepHours() || 1));
      const speedHoursPerSecond = Math.max(0.01, Number(this.playbackHoursPerSecond || 1));
      const framesPerSecond = speedHoursPerSecond / frameStepHours;
      const stepMs = 1000 / Math.max(0.01, framesPerSecond);

      if (dt >= stepMs) {
        let next = this.currentFrame + 1;
        if (next > this.endFrame) next = this.startFrame;
        this.currentFrame = clamp(next, this.startFrame, this.endFrame);
        this.lastTick = now;
        await this.renderCurrentFrame();
      }
      requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  }

  pause() {
    this.playing = false;
    this.playBtn.textContent = "Play";
  }

  togglePlay() {
    if (this.playing) this.pause();
    else this.play();
  }

  clampRoi(roi) {
    const gi = this.model.getCurrentGroupInfo();
    if (!gi) return roi;
    const nx = Math.max(1, gi.cropWidth);
    const ny = Math.max(1, gi.cropHeight);
    let ni0 = clamp(Math.floor(Number(roi.ni0)), 0, nx - 1);
    let ni1 = clamp(Math.floor(Number(roi.ni1)), 0, nx - 1);
    let nj0 = clamp(Math.floor(Number(roi.nj0)), 0, ny - 1);
    let nj1 = clamp(Math.floor(Number(roi.nj1)), 0, ny - 1);
    if (ni0 > ni1) [ni0, ni1] = [ni1, ni0];
    if (nj0 > nj1) [nj0, nj1] = [nj1, nj0];
    return { ni0, ni1, nj0, nj1 };
  }

  setRoi(nextRoi, reason = "") {
    this.roi = this.clampRoi(nextRoi);
    this.syncRoiInputsFromState();
    this.updateRoiOverlay();
    this.updateSelectionInfo();
    if (reason) {
      this.statusText.textContent = reason;
    }
  }

  syncRoiInputsFromState() {
    const gi = this.model.getCurrentGroupInfo();
    if (!gi || !this.roi) return;
    const width = this.roi.ni1 - this.roi.ni0 + 1;
    const height = this.roi.nj1 - this.roi.nj0 + 1;
    const b = this.getRoiLatLonBounds();

    this.uiSync = true;
    this.niStartInput.value = String(this.roi.ni0);
    this.niEndInput.value = String(this.roi.ni1);
    this.njStartInput.value = String(this.roi.nj0);
    this.njEndInput.value = String(this.roi.nj1);
    this.roiWidthInput.value = String(width);
    this.roiHeightInput.value = String(height);

    this.latNorthInput.value = Number.isFinite(b.north) ? b.north.toFixed(6) : "";
    this.latSouthInput.value = Number.isFinite(b.south) ? b.south.toFixed(6) : "";
    this.lonWestInput.value = Number.isFinite(b.west) ? b.west.toFixed(6) : "";
    this.lonEastInput.value = Number.isFinite(b.east) ? b.east.toFixed(6) : "";
    this.uiSync = false;
  }

  applyRoiFromPixelInputs() {
    if (this.uiSync) return;
    const gi = this.model.getCurrentGroupInfo();
    if (!gi || !this.roi) return;

    const ni0 = clamp(Math.floor(safeNum(this.niStartInput.value, this.roi.ni0)), 0, gi.cropWidth - 1);
    const nj0 = clamp(Math.floor(safeNum(this.njStartInput.value, this.roi.nj0)), 0, gi.cropHeight - 1);

    let ni1 = clamp(Math.floor(safeNum(this.niEndInput.value, this.roi.ni1)), 0, gi.cropWidth - 1);
    let nj1 = clamp(Math.floor(safeNum(this.njEndInput.value, this.roi.nj1)), 0, gi.cropHeight - 1);

    const width = Math.floor(safeNum(this.roiWidthInput.value, (ni1 - ni0 + 1)));
    const height = Math.floor(safeNum(this.roiHeightInput.value, (nj1 - nj0 + 1)));

    if (Number.isFinite(width) && width > 0) ni1 = clamp(ni0 + width - 1, ni0, gi.cropWidth - 1);
    if (Number.isFinite(height) && height > 0) nj1 = clamp(nj0 + height - 1, nj0, gi.cropHeight - 1);

    this.setRoi({ ni0, ni1, nj0, nj1 }, "ROI updated from NI/NJ");
  }

  angularDistance(a, b) {
    const da = ((a - b + 540) % 360) - 180;
    return Math.abs(da);
  }

  nearestIndex(values, target, isLon = false) {
    if (!Array.isArray(values) || values.length === 0 || !Number.isFinite(target)) return null;
    let bestIdx = null;
    let bestDist = Number.POSITIVE_INFINITY;
    for (let i = 0; i < values.length; i += 1) {
      const v = Number(values[i]);
      if (!Number.isFinite(v)) continue;
      const d = isLon ? this.angularDistance(v, target) : Math.abs(v - target);
      if (d < bestDist) {
        bestDist = d;
        bestIdx = i;
      }
    }
    return bestIdx;
  }

  applyRoiFromLatLonInputs() {
    if (this.uiSync) return;
    const gi = this.model.getCurrentGroupInfo();
    if (!gi || !this.roi) return;

    const latN = Number(this.latNorthInput.value);
    const latS = Number(this.latSouthInput.value);
    const lonW = Number(this.lonWestInput.value);
    const lonE = Number(this.lonEastInput.value);
    if (!Number.isFinite(latN) || !Number.isFinite(latS) || !Number.isFinite(lonW) || !Number.isFinite(lonE)) {
      this.statusText.textContent = "Lat/Lon values must be numeric.";
      return;
    }

    const jA = this.nearestIndex(this.rowLat, latN, false);
    const jB = this.nearestIndex(this.rowLat, latS, false);
    const iA = this.nearestIndex(this.colLon, lonW, true);
    const iB = this.nearestIndex(this.colLon, lonE, true);
    if (jA == null || jB == null || iA == null || iB == null) {
      this.statusText.textContent = "Cannot map lat/lon to NI/NJ for this grid.";
      return;
    }

    const nj0 = Math.min(jA, jB);
    const nj1 = Math.max(jA, jB);
    const ni0 = Math.min(iA, iB);
    const ni1 = Math.max(iA, iB);
    this.setRoi({ ni0, ni1, nj0, nj1 }, "ROI updated from Lat/Lon");
  }

  getRoiLatLonBounds() {
    if (!this.roi) return { north: NaN, south: NaN, west: NaN, east: NaN };
    const corners = [
      this.model.getLatLonAt(this.roi.ni0, this.roi.nj0),
      this.model.getLatLonAt(this.roi.ni1, this.roi.nj0),
      this.model.getLatLonAt(this.roi.ni0, this.roi.nj1),
      this.model.getLatLonAt(this.roi.ni1, this.roi.nj1)
    ];
    const lats = corners.map((c) => Number(c?.lat)).filter((v) => Number.isFinite(v));
    const lons = corners.map((c) => Number(c?.lon)).filter((v) => Number.isFinite(v));
    return {
      north: lats.length ? Math.max(...lats) : NaN,
      south: lats.length ? Math.min(...lats) : NaN,
      west: lons.length ? Math.min(...lons) : NaN,
      east: lons.length ? Math.max(...lons) : NaN,
      latUnits: corners[0]?.latUnits || "degrees_north",
      lonUnits: corners[0]?.lonUnits || "degrees_east"
    };
  }

  updateSelectionInfo() {
    const gi = this.model.getCurrentGroupInfo();
    if (!gi || !this.roi) {
      this.selectionInfo.textContent = "No dataset loaded.";
      return;
    }

    const width = this.roi.ni1 - this.roi.ni0 + 1;
    const height = this.roi.nj1 - this.roi.nj0 + 1;
    const frames = this.endFrame - this.startFrame + 1;
    const b = this.getRoiLatLonBounds();

    this.selectionInfo.textContent =
      `stream_id: ${gi.streamIndex}\n` +
      `group: ${gi.shortName}_${gi.typeOfLevel}_${gi.level}\n` +
      `variable: ${gi.shortName}\n` +
      `level_type: ${gi.typeOfLevel}\n` +
      `level: ${gi.level}\n` +
      `unit: ${gi.unit || "(none)"}\n` +
      `\n` +
      `ni_start: ${this.roi.ni0}\n` +
      `ni_end: ${this.roi.ni1}\n` +
      `nj_start: ${this.roi.nj0}\n` +
      `nj_end: ${this.roi.nj1}\n` +
      `width: ${width}\n` +
      `height: ${height}\n` +
      `\n` +
      `lat_north: ${Number.isFinite(b.north) ? b.north.toFixed(6) : "n/a"} ${b.latUnits || ""}\n` +
      `lat_south: ${Number.isFinite(b.south) ? b.south.toFixed(6) : "n/a"} ${b.latUnits || ""}\n` +
      `lon_west: ${Number.isFinite(b.west) ? b.west.toFixed(6) : "n/a"} ${b.lonUnits || ""}\n` +
      `lon_east: ${Number.isFinite(b.east) ? b.east.toFixed(6) : "n/a"} ${b.lonUnits || ""}\n` +
      `\n` +
      `frame_start: ${this.startFrame}\n` +
      `frame_end: ${this.endFrame}\n` +
      `frame_count: ${frames}\n` +
      `time_start: ${this.getFrameLabel(this.startFrame)}\n` +
      `time_end: ${this.getFrameLabel(this.endFrame)}\n`;
  }

  getCanvasRects() {
    const wrapRect = this.canvasWrap.getBoundingClientRect();
    const canvasRect = this.canvas.getBoundingClientRect();
    return { wrapRect, canvasRect };
  }

  updateRoiOverlay() {
    const gi = this.model.getCurrentGroupInfo();
    if (!gi || !this.roi) return;
    const { wrapRect, canvasRect } = this.getCanvasRects();
    const nx = Math.max(1, gi.cropWidth);
    const ny = Math.max(1, gi.cropHeight);

    const x = (canvasRect.left - wrapRect.left) + (this.roi.ni0 / nx) * canvasRect.width;
    const y = (canvasRect.top - wrapRect.top) + (this.roi.nj0 / ny) * canvasRect.height;
    const w = ((this.roi.ni1 + 1 - this.roi.ni0) / nx) * canvasRect.width;
    const h = ((this.roi.nj1 + 1 - this.roi.nj0) / ny) * canvasRect.height;

    this.roiBox.style.left = `${x}px`;
    this.roiBox.style.top = `${y}px`;
    this.roiBox.style.width = `${Math.max(1, w)}px`;
    this.roiBox.style.height = `${Math.max(1, h)}px`;
  }

  pointToIndex(clientX, clientY) {
    const gi = this.model.getCurrentGroupInfo();
    if (!gi) return null;
    const { canvasRect } = this.getCanvasRects();
    if (canvasRect.width <= 0 || canvasRect.height <= 0) return null;
    const fx = (clientX - canvasRect.left) / canvasRect.width;
    const fy = (clientY - canvasRect.top) / canvasRect.height;
    const i = clamp(Math.floor(fx * gi.cropWidth), 0, gi.cropWidth - 1);
    const j = clamp(Math.floor(fy * gi.cropHeight), 0, gi.cropHeight - 1);
    return { i, j };
  }

  startDrag(mode, ev, extra) {
    const gi = this.model.getCurrentGroupInfo();
    if (!gi || !this.roi) return;
    const anchor = this.pointToIndex(ev.clientX, ev.clientY);
    if (!anchor) return;

    this.dragState = {
      mode,
      handle: extra?.handle || "",
      startX: ev.clientX,
      startY: ev.clientY,
      startRoi: { ...this.roi },
      anchor
    };

    this._dragMoveHandler = (e) => this.onDragMove(e);
    this._dragUpHandler = (e) => this.onDragEnd(e);
    window.addEventListener("pointermove", this._dragMoveHandler);
    window.addEventListener("pointerup", this._dragUpHandler, { once: true });
  }

  onDragMove(ev) {
    if (!this.dragState) return;
    const gi = this.model.getCurrentGroupInfo();
    if (!gi) return;
    const { canvasRect } = this.getCanvasRects();
    const nx = Math.max(1, gi.cropWidth);
    const ny = Math.max(1, gi.cropHeight);

    const dx = ev.clientX - this.dragState.startX;
    const dy = ev.clientY - this.dragState.startY;
    const di = Math.round((dx / Math.max(1, canvasRect.width)) * nx);
    const dj = Math.round((dy / Math.max(1, canvasRect.height)) * ny);

    const s = this.dragState.startRoi;
    let roi = { ...s };

    if (this.dragState.mode === "move") {
      const w = s.ni1 - s.ni0;
      const h = s.nj1 - s.nj0;
      let ni0 = clamp(s.ni0 + di, 0, nx - 1);
      let nj0 = clamp(s.nj0 + dj, 0, ny - 1);
      let ni1 = ni0 + w;
      let nj1 = nj0 + h;
      if (ni1 > nx - 1) {
        ni1 = nx - 1;
        ni0 = Math.max(0, ni1 - w);
      }
      if (nj1 > ny - 1) {
        nj1 = ny - 1;
        nj0 = Math.max(0, nj1 - h);
      }
      roi = { ni0, ni1, nj0, nj1 };
    } else if (this.dragState.mode === "resize") {
      const h = this.dragState.handle;
      let { ni0, ni1, nj0, nj1 } = s;
      if (h.includes("w")) ni0 += di;
      if (h.includes("e")) ni1 += di;
      if (h.includes("n")) nj0 += dj;
      if (h.includes("s")) nj1 += dj;
      roi = this.clampRoi({ ni0, ni1, nj0, nj1 });
    } else if (this.dragState.mode === "draw") {
      const p = this.pointToIndex(ev.clientX, ev.clientY);
      if (!p) return;
      roi = this.clampRoi({
        ni0: this.dragState.anchor.i,
        nj0: this.dragState.anchor.j,
        ni1: p.i,
        nj1: p.j
      });
    }

    this.setRoi(roi);
  }

  onDragEnd() {
    if (this._dragMoveHandler) {
      window.removeEventListener("pointermove", this._dragMoveHandler);
      this._dragMoveHandler = null;
    }
    this.dragState = null;
  }

  computeStride(shape) {
    const dims = Array.isArray(shape) ? shape.map((v) => Number(v)) : [];
    if (!dims.length) return [];
    const stride = new Array(dims.length).fill(1);
    for (let i = dims.length - 2; i >= 0; i -= 1) {
      stride[i] = stride[i + 1] * Math.max(1, dims[i + 1]);
    }
    return stride;
  }

  async createZarrArray(root, path, shape, dtype, attributes = {}) {
    const dims = shape.map((v) => Math.max(1, Number(v)));
    const optsA = {
      shape: dims,
      chunks: dims,
      dtype,
      attributes
    };
    const optsB = {
      shape: dims,
      chunk_shape: dims,
      data_type: dtype,
      attributes
    };
    try {
      return await zarr.create(root.resolve(path), optsA);
    } catch {
      return await zarr.create(root.resolve(path), optsB);
    }
  }

  async writeZarrArray(arr, shape, data) {
    const makeSel = () => shape.map(() => {
      if (typeof zarr.slice === "function") return zarr.slice(null, null, null);
      return null;
    });
    const view = {
      data,
      shape: shape.map((v) => Number(v)),
      stride: this.computeStride(shape)
    };
    if (typeof zarr.set === "function") {
      try {
        await zarr.set(arr, makeSel(), view);
        return;
      } catch {
        // fallback below
      }
    }
    if (typeof arr.set === "function") {
      try {
        await arr.set(makeSel(), view);
        return;
      } catch {
        // fallback below
      }
    }
    if (typeof arr.setChunk === "function") {
      try {
        await arr.setChunk(new Array(shape.length).fill(0), view);
      } catch {
        await arr.setChunk(new Array(shape.length).fill(0), data);
      }
      return;
    }
    throw new Error("zarrita write API not found (set/arr.set/arr.setChunk missing)");
  }

  decodePixelValue(qRaw, sampleBits, bitsTarget, bounds, opts, hasBitmap) {
    let q = Number(qRaw);
    if (!Number.isFinite(q)) q = 0;
    if (sampleBits < bitsTarget) q <<= (bitsTarget - sampleBits);
    else if (sampleBits > bitsTarget) q >>= (sampleBits - bitsTarget);

    const storageShift = Number(opts?.storage_shift || 0);
    if (storageShift > 0) q >>= storageShift;
    if (hasBitmap) q = Math.max(0, q - 1);

    const qmax = (1 << Math.min(30, bitsTarget)) - 1;
    const qvalid = hasBitmap ? Math.max(1, qmax - 1) : Math.max(1, qmax);
    const norm = clamp(q / qvalid, 0, 1);
    let value = Number(bounds.min) + norm * (Number(bounds.max) - Number(bounds.min));
    if (opts?.apply_log2) {
      const logOffset = safeNum(opts.log_offset, 1);
      value = Math.pow(2, value) - logOffset;
    }
    return Number(value);
  }

  async extractSelectionFloat32() {
    const gi = this.model.getCurrentGroupInfo();
    if (!gi || !this.roi) throw new Error("No active group/selection");

    const ni0 = this.roi.ni0;
    const ni1 = this.roi.ni1;
    const nj0 = this.roi.nj0;
    const nj1 = this.roi.nj1;
    const width = ni1 - ni0 + 1;
    const height = nj1 - nj0 + 1;
    const frameCount = this.endFrame - this.startFrame + 1;
    const frameSize = width * height;
    const total = frameSize * frameCount;
    if (total <= 0) throw new Error("Empty extraction selection");

    const values = new Float32Array(total);
    const opts = this.model.currentGroup?.options || {};
    const bitsTarget = Number(opts.bits || 8);

    for (let t = 0; t < frameCount; t += 1) {
      const frameIndex = this.startFrame + t;
      const plane = await this.model._ensurePlaneData(frameIndex);
      const sampleBits = Number(plane.bits || bitsTarget || 8);
      const yStride = Number(plane.yStride || 0);
      const y = plane.y;
      const rec = this.model._getFrameRecord(frameIndex);
      const hasBitmap = Boolean(rec?.bitmapPresent);
      const bounds = this.model._getFrameQuantBounds(frameIndex);
      const bpp = sampleBits > 8 ? 2 : 1;

      const base = t * frameSize;
      for (let jj = 0; jj < height; jj += 1) {
        const srcJ = nj0 + jj;
        const rowBase = base + jj * width;
        for (let ii = 0; ii < width; ii += 1) {
          const srcI = ni0 + ii;
          const off = srcJ * yStride + srcI * bpp;
          let qRaw = 0;
          if (bpp === 2) qRaw = y[off] | (y[off + 1] << 8);
          else qRaw = y[off];
          values[rowBase + ii] = this.decodePixelValue(qRaw, sampleBits, bitsTarget, bounds, opts, hasBitmap);
        }
      }
      const pct = ((t + 1) * 100) / frameCount;
      this.statusText.textContent = `Extracting selection values: frame ${t + 1}/${frameCount}`;
      this.setDecodeProgress(pct, true);
      if (t % 4 === 0) await new Promise((r) => requestAnimationFrame(r));
    }

    const lat = new Float64Array(height);
    const lon = new Float64Array(width);
    const niMid = Math.floor((ni0 + ni1) / 2);
    const njMid = Math.floor((nj0 + nj1) / 2);
    for (let j = 0; j < height; j += 1) {
      const ll = this.model.getLatLonAt(niMid, nj0 + j);
      lat[j] = Number(ll?.lat);
    }
    for (let i = 0; i < width; i += 1) {
      const ll = this.model.getLatLonAt(ni0 + i, njMid);
      lon[i] = Number(ll?.lon);
    }

    const frameIndex = new Int32Array(frameCount);
    const timeEpochMs = new Float64Array(frameCount);
    const timeLabel = new Array(frameCount);
    for (let t = 0; t < frameCount; t += 1) {
      const fi = this.startFrame + t;
      frameIndex[t] = fi;
      const label = this.getFrameLabel(fi);
      timeLabel[t] = label;
      const ms = Date.parse(label);
      timeEpochMs[t] = Number.isFinite(ms) ? ms : NaN;
    }

    return {
      values,
      width,
      height,
      frameCount,
      frameIndex,
      timeEpochMs,
      timeLabel,
      lat,
      lon
    };
  }

  async extractSelectionToZarr() {
    const gi = this.model.getCurrentGroupInfo();
    if (!gi || !this.roi) throw new Error("No active stream/selection");

    this.pause();
    this.statusText.textContent = "Preparing selection extraction...";
    this.setDecodeProgress(0, true);
    const subset = await this.extractSelectionFloat32();

    this.statusText.textContent = "Building Zarr store with zarrita.js...";
    const store = new Map();
    const root = typeof zarr.root === "function" ? zarr.root(store) : null;
    if (!root) {
      throw new Error("zarrita root() is unavailable in current browser build");
    }

    const groupName = sanitizeName(`${gi.shortName}_${gi.typeOfLevel}_${gi.level}`, "group");
    const datasetName = sanitizeName(this.currentDatasetUrl.split("/").pop() || "dataset", "dataset");

    const subsetInfo = {
      source_dataset: this.currentDatasetUrl || "",
      stream_index: gi.streamIndex,
      variable: gi.shortName,
      type_of_level: gi.typeOfLevel,
      level: gi.level,
      unit: gi.unit || "",
      roi: {
        ni_start: this.roi.ni0,
        ni_end: this.roi.ni1,
        nj_start: this.roi.nj0,
        nj_end: this.roi.nj1
      },
      frame_range: {
        start: this.startFrame,
        end: this.endFrame
      }
    };
    try {
      await zarr.create(root, { attributes: subsetInfo });
    } catch {
      store.set("/subset_info.json", strToU8(JSON.stringify(subsetInfo, null, 2)));
    }

    const dataArr = await this.createZarrArray(
      root,
      `${groupName}/data`,
      [subset.frameCount, subset.height, subset.width],
      "float32",
      {
        dimensions: ["time", "y", "x"],
        unit: gi.unit || ""
      }
    );
    await this.writeZarrArray(
      dataArr,
      [subset.frameCount, subset.height, subset.width],
      subset.values
    );
    this.setDecodeProgress(85, true);

    const latArr = await this.createZarrArray(
      root,
      `${groupName}/latitude`,
      [subset.height],
      "float64",
      { dimensions: ["y"], units: "degrees_north" }
    );
    await this.writeZarrArray(latArr, [subset.height], subset.lat);

    const lonArr = await this.createZarrArray(
      root,
      `${groupName}/longitude`,
      [subset.width],
      "float64",
      { dimensions: ["x"], units: "degrees_east" }
    );
    await this.writeZarrArray(lonArr, [subset.width], subset.lon);

    const frameArr = await this.createZarrArray(
      root,
      `${groupName}/frame_index`,
      [subset.frameCount],
      "int32",
      { dimensions: ["time"] }
    );
    await this.writeZarrArray(frameArr, [subset.frameCount], subset.frameIndex);

    const timeArr = await this.createZarrArray(
      root,
      `${groupName}/time_epoch_ms`,
      [subset.frameCount],
      "float64",
      { dimensions: ["time"], units: "milliseconds since 1970-01-01T00:00:00Z" }
    );
    await this.writeZarrArray(timeArr, [subset.frameCount], subset.timeEpochMs);

    const infoKey = `/${groupName}/time_labels.json`;
    store.set(infoKey, strToU8(JSON.stringify(subset.timeLabel, null, 2)));

    const files = {};
    for (const [key, value] of store.entries()) {
      const clean = String(key).replace(/^\/+/, "");
      if (value instanceof Uint8Array) files[`${datasetName}.zarr/${clean}`] = value;
      else if (value instanceof ArrayBuffer) files[`${datasetName}.zarr/${clean}`] = new Uint8Array(value);
      else if (ArrayBuffer.isView(value)) files[`${datasetName}.zarr/${clean}`] = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      else files[`${datasetName}.zarr/${clean}`] = strToU8(String(value));
    }
    const zip = zipSync(files, { level: 6 });
    const outName = `${datasetName}_${groupName}_${this.startFrame}-${this.endFrame}.zarr.zip`;
    triggerDownload(new Blob([zip], { type: "application/zip" }), outName);
    this.setDecodeProgress(100, false);
    this.statusText.textContent = `Zarr export done: ${outName}`;
  }
}

const extractor = new AvencExtractor();
extractor.initialize().catch((err) => {
  const status = document.getElementById("statusText");
  status.textContent = `Initialization error: ${err.message || err}`;
});
