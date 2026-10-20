import { AvencModel } from "./avenc_model.js";

function formatBytes(bytes) {
  const n = Number(bytes || 0);
  if (!Number.isFinite(n) || n <= 0) return "0 B";
  const units = ["B", "KB", "MB", "GB", "TB"];
  let v = n;
  let u = 0;
  while (v >= 1024 && u < units.length - 1) {
    v /= 1024;
    u += 1;
  }
  const d = v >= 100 ? 0 : (v >= 10 ? 1 : 2);
  return `${v.toFixed(d)} ${units[u]}`;
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
  if (x < 0.33) {
    return mix(c0, c1, x / 0.33);
  } else if (x < 0.66) {
    return mix(c1, c2, (x - 0.33) / 0.33);
  }
  return mix(c2, c3, (x - 0.66) / 0.34);
}

void main() {
  vec2 tc = vec2(v_uv.x * u_cropScale.x, v_uv.y * u_cropScale.y);
  float y = texture2D(u_video, tc).r;
  float qv = clamp(y, 0.0, 1.0);
  float value = mix(u_quantRange.x, u_quantRange.y, qv);
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

    const quad = new Float32Array([
      -1, -1,
      1, -1,
      -1, 1,
      1, 1
    ]);
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

class AvencViewer {
  constructor() {
    this.model = new AvencModel([]);
    this.groupSelect = document.getElementById("groupSelect");
    this.colormapSelect = document.getElementById("colormapSelect");
    this.rateSelect = document.getElementById("rateSelect");
    this.timeline = document.getElementById("timeline");
    this.playBtn = document.getElementById("playBtn");
    this.statusText = document.getElementById("statusText");
    this.decodeProgressWrap = document.getElementById("decodeProgressWrap");
    this.decodeProgressFill = document.getElementById("decodeProgressFill");
    this.frameLabel = document.getElementById("frameLabel");
    this.timeLabel = document.getElementById("timeLabel");
    this.legendCanvas = document.getElementById("legendCanvas");
    this.legendMin = document.getElementById("legendMin");
    this.legendMax = document.getElementById("legendMax");
    this.scaleLockBtn = document.getElementById("scaleLockBtn");
    this.rescaleBtn = document.getElementById("rescaleBtn");
    this.datasetInfo = document.getElementById("datasetInfo");
    this.cursorInfo = document.getElementById("cursorInfo");
    this.tooltip = document.getElementById("hoverTooltip");
    this.canvas = document.getElementById("glCanvas");
    this.markerLayer = document.getElementById("markerLayer");
    this.crossV = document.getElementById("crossV");
    this.crossH = document.getElementById("crossH");
    this.crossLonLabel = document.getElementById("crossLonLabel");
    this.crossLatLabel = document.getElementById("crossLatLabel");
    this.crossValueLabel = document.getElementById("crossValueLabel");
    this.renderer = new AvencWebGLRenderer(this.canvas);

    this.currentFrame = 0;
    this.playing = false;
    this.lastTick = 0;
    this.playbackHoursPerSecond = Number(this.rateSelect?.value || 1);
    this.scaleLocked = false;
    this.lockedRange = null;
    this.lastRenderRange = null;
    this.zoomScale = 1;
    this.zoomOriginX = 50;
    this.zoomOriginY = 50;
    this.hoverBusy = false;
    this.pendingHover = null;
    this.lastPointer = null;
    this.markers = [];
    this.nextMarkerId = 1;
    this.markerUpdateToken = 0;
  }

  getRequestedDatasetFromUrl() {
    try {
      const u = new URL(window.location.href);
      const ds = (u.searchParams.get("dataset") || "").trim();
      return ds || "";
    } catch {
      return "";
    }
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
    this.model.addEventListener("loaded", () => this.refreshGroupMenu());

    this.groupSelect.addEventListener("change", async () => {
      this.pause();
      this.setDecodeProgress(0, false);
      this.resetZoom();
      this.clearMarkers();
      await this.model.selectGroup(Number(this.groupSelect.value));
      this.currentFrame = 0;
      this.updateTimeline();
      await this.renderCurrentFrame();
      this.updateDatasetInfo();
    });
    this.colormapSelect.addEventListener("change", () => this.renderCurrentFrame());
    this.rateSelect.addEventListener("change", () => {
      this.playbackHoursPerSecond = Number(this.rateSelect.value || 1);
    });
    this.timeline.addEventListener("input", async () => {
      this.currentFrame = Number(this.timeline.value);
      await this.renderCurrentFrame();
    });
    this.playBtn.addEventListener("click", () => this.togglePlay());
    this.scaleLockBtn.addEventListener("click", () => this.toggleScaleLock());
    this.rescaleBtn.addEventListener("click", () => this.rescaleFromCurrentFrame());

    this.canvas.addEventListener("mousemove", (ev) => {
      this.lastPointer = { clientX: ev.clientX, clientY: ev.clientY };
      this.pendingHover = ev;
      this.scheduleHoverUpdate();
    });
    this.canvas.addEventListener("click", async (ev) => {
      await this.addMarkerAtEvent(ev);
    });
    this.canvas.addEventListener("wheel", (ev) => this.onWheelZoom(ev), { passive: false });
    this.canvas.addEventListener("dblclick", () => this.resetZoom());
    this.canvas.addEventListener("mouseleave", () => {
      this.lastPointer = null;
      this.tooltip.style.display = "none";
      this.hideCrosshair();
      if (this.cursorInfo) this.cursorInfo.textContent = "Move mouse over map.";
    });
  }

  setDecodeProgress(percent, active) {
    const pct = Math.max(0, Math.min(100, Number(percent || 0)));
    this.decodeProgressFill.style.width = `${pct.toFixed(1)}%`;
    if (active) this.decodeProgressWrap.classList.add("active");
    else this.decodeProgressWrap.classList.remove("active");
  }

  async initialize() {
    this.bindEvents();
    this.colormapSelect.value = "gray";
    const url = this.getRequestedDatasetFromUrl();
    if (!url) {
      this.statusText.textContent =
        "Missing dataset parameter. Open this viewer via index.php or add ?dataset=./datasets/<file>.mp4";
      return;
    }
    await this.loadDatasetByUrl(url);
  }

  async loadDatasetByUrl(url) {
    if (!url) return;
    this.pause();
    this.setDecodeProgress(0, false);
    this.resetZoom();
    this.clearMarkers();
    try {
      await this.model.loadDatasetByUrl(url);
      this.currentFrame = 0;
      this.updateTimeline();
      await this.renderCurrentFrame();
      this.updateDatasetInfo();
    } catch (err) {
      this.statusText.textContent = `Load error: ${err.message || err}`;
    }
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

  updateTimeline() {
    const n = this.model.getCurrentFrameCount();
    this.timeline.min = "0";
    this.timeline.max = String(Math.max(0, n - 1));
    this.timeline.value = String(Math.min(this.currentFrame, Math.max(0, n - 1)));
  }

  updateDatasetInfo() {
    const gi = this.model.getCurrentGroupInfo();
    if (!gi) {
      this.datasetInfo.textContent = "No group selected.";
      return;
    }
    const stats = this.model.getCompressionStats();
    const ratio = Number(stats?.compressionRatioFull || 0);
    const ratioTxt = Number.isFinite(ratio) && ratio > 0 ? `${ratio.toFixed(2)}x` : "--";
    const unitTxt = gi.unit ? gi.unit : "(unit not provided)";
    this.datasetInfo.textContent =
      `group_id: ${gi.id}\n` +
      `stream_index: ${gi.streamIndex}\n` +
      `short_name: ${gi.shortName}\n` +
      `level_type: ${gi.typeOfLevel}\n` +
      `level: ${gi.level}\n` +
      `unit: ${unitTxt}\n` +
      `frames: ${gi.frameCount}\n` +
      `bits: ${gi.bits}\n` +
      `quantization: ${gi.quantMode}\n` +
      `crop: ${gi.cropWidth} x ${gi.cropHeight}\n` +
      `encoded: ${gi.encodedWidth} x ${gi.encodedHeight}\n` +
      `group_minmax: [${gi.valueMin.toFixed(6)}, ${gi.valueMax.toFixed(6)}]\n` +
      `\n` +
      `mp4_size: ${formatBytes(stats?.fileSizeBytes || 0)}\n` +
      `raw_f32_full: ${formatBytes(stats?.rawFullFloat32Bytes || 0)}\n` +
      `raw_f32_group: ${formatBytes(stats?.rawGroupFloat32Bytes || 0)}\n` +
      `compression(raw_f32/mp4): ${ratioTxt}`;
  }

  async renderCurrentFrame() {
    const renderRequest = this._renderRequest = (this._renderRequest || 0) + 1;
    const n = this.model.getCurrentFrameCount();
    if (!n) return;
    if (this.currentFrame < 0) this.currentFrame = 0;
    if (this.currentFrame >= n) this.currentFrame = n - 1;

    try {
      const frameVis = await this.model.getFrameVisual(this.currentFrame, "frame");
      if (renderRequest !== this._renderRequest) return;
      this.renderer.resizeTo(frameVis.cropWidth, frameVis.cropHeight);
      this.applyZoomTransform();
      const displayMin = this.scaleLocked && this.lockedRange ? this.lockedRange.min : frameVis.displayMin;
      const displayMax = this.scaleLocked && this.lockedRange ? this.lockedRange.max : frameVis.displayMax;
      this.lastRenderRange = { min: displayMin, max: displayMax };
      this.renderer.render(frameVis.frame, {
        cropScaleX: frameVis.cropWidth / Math.max(1, frameVis.encodedWidth),
        cropScaleY: frameVis.cropHeight / Math.max(1, frameVis.encodedHeight),
        quantMin: frameVis.quantMin,
        quantMax: frameVis.quantMax,
        displayMin,
        displayMax,
        colormap: this.colormapSelect.value
      });

      this.frameLabel.textContent = `frame=${this.currentFrame}`;
      this.timeLabel.textContent = `time=${this.model.getFrameTimestampLabel(this.currentFrame)}`;
      this.timeline.value = String(this.currentFrame);
      this.updateLegend(displayMin, displayMax, this.colormapSelect.value);
      this.positionMarkers();
      this.updateMarkersForCurrentFrame().catch(() => {});
      if (this.lastPointer) {
        this.pendingHover = { ...this.lastPointer };
        this.scheduleHoverUpdate();
      }
    } catch (err) {
      if (err.name === "AbortError" || renderRequest !== this._renderRequest) return;
      this.statusText.textContent = `Render error: ${err.message || err}`;
    }
  }

  updateLegend(minValue, maxValue, colormap) {
    const ctx = this.legendCanvas.getContext("2d");
    const w = this.legendCanvas.width;
    const h = this.legendCanvas.height;
    for (let x = 0; x < w; x += 1) {
      const t = x / Math.max(1, w - 1);
      let rgb;
      if (colormap === "gray") rgb = this.cmGray(t);
      else if (colormap === "jet") rgb = this.cmJet(t);
      else rgb = this.cmViridis(t);
      const [r, g, b] = rgb;
      ctx.fillStyle = `rgb(${r}, ${g}, ${b})`;
      ctx.fillRect(x, 0, 1, h);
    }
    this.legendMin.textContent = minValue.toFixed(6);
    this.legendMax.textContent = maxValue.toFixed(6);
  }

  cmJet(t) {
    const clamp = (x) => Math.max(0, Math.min(1, x));
    const r = clamp(1.5 - Math.abs(4 * t - 3));
    const g = clamp(1.5 - Math.abs(4 * t - 2));
    const b = clamp(1.5 - Math.abs(4 * t - 1));
    return [Math.round(r * 255), Math.round(g * 255), Math.round(b * 255)];
  }

  cmGray(t) {
    const g = Math.round(Math.max(0, Math.min(1, t)) * 255);
    return [g, g, g];
  }

  cmViridis(t) {
    const c0 = [72, 42, 121];
    const c1 = [31, 121, 180];
    const c2 = [94, 201, 98];
    const c3 = [253, 231, 37];
    let a; let b; let f;
    if (t < 0.33) {
      a = c0; b = c1; f = t / 0.33;
    } else if (t < 0.66) {
      a = c1; b = c2; f = (t - 0.33) / 0.33;
    } else {
      a = c2; b = c3; f = (t - 0.66) / 0.34;
    }
    return [
      Math.round(a[0] + (b[0] - a[0]) * f),
      Math.round(a[1] + (b[1] - a[1]) * f),
      Math.round(a[2] + (b[2] - a[2]) * f)
    ];
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
        const n = this.model.getCurrentFrameCount();
        this.currentFrame = (this.currentFrame + 1) % Math.max(1, n);
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

  toggleScaleLock() {
    if (this.scaleLocked) {
      this.scaleLocked = false;
      this.lockedRange = null;
      this.scaleLockBtn.textContent = "Lock Scale";
      this.renderCurrentFrame();
      return;
    }
    if (this.lastRenderRange) {
      this.scaleLocked = true;
      this.lockedRange = { min: this.lastRenderRange.min, max: this.lastRenderRange.max };
      this.scaleLockBtn.textContent = "Unlock Scale";
      this.renderCurrentFrame();
    }
  }

  async rescaleFromCurrentFrame() {
    const frameVis = await this.model.getFrameVisual(this.currentFrame, "frame");
    this.scaleLocked = true;
    this.lockedRange = { min: frameVis.displayMin, max: frameVis.displayMax };
    this.scaleLockBtn.textContent = "Unlock Scale";
    await this.renderCurrentFrame();
  }

  onWheelZoom(ev) {
    ev.preventDefault();
    const rect = this.canvas.getBoundingClientRect();
    if (rect.width <= 0 || rect.height <= 0) return;
    const px = ((ev.clientX - rect.left) / rect.width) * 100;
    const py = ((ev.clientY - rect.top) / rect.height) * 100;
    this.zoomOriginX = Math.max(0, Math.min(100, px));
    this.zoomOriginY = Math.max(0, Math.min(100, py));
    const factor = ev.deltaY < 0 ? 1.12 : (1 / 1.12);
    this.zoomScale = Math.max(1, Math.min(20, this.zoomScale * factor));
    this.applyZoomTransform();
    this.positionMarkers();
  }

  resetZoom() {
    this.zoomScale = 1;
    this.zoomOriginX = 50;
    this.zoomOriginY = 50;
    this.applyZoomTransform();
    this.positionMarkers();
  }

  applyZoomTransform() {
    this.canvas.style.transformOrigin = `${this.zoomOriginX}% ${this.zoomOriginY}%`;
    this.canvas.style.transform = `scale(${this.zoomScale})`;
  }

  clearMarkers() {
    for (const m of this.markers) {
      try {
        m.dot.remove();
        m.label.remove();
      } catch {
        // ignore
      }
    }
    this.markers = [];
    this.nextMarkerId = 1;
  }

  async addMarkerAtEvent(ev) {
    const gi = this.model.getCurrentGroupInfo();
    if (!gi) return;
    const rect = this.canvas.getBoundingClientRect();
    const x = (ev.clientX - rect.left) / Math.max(1, rect.width);
    const y = (ev.clientY - rect.top) / Math.max(1, rect.height);
    const i = Math.max(0, Math.min(gi.cropWidth - 1, Math.floor(x * gi.cropWidth)));
    const j = Math.max(0, Math.min(gi.cropHeight - 1, Math.floor(y * gi.cropHeight)));

    const dot = document.createElement("div");
    dot.className = "marker-dot";
    const label = document.createElement("div");
    label.className = "marker-label";
    label.textContent = "--";
    this.markerLayer.appendChild(dot);
    this.markerLayer.appendChild(label);

    const marker = {
      id: this.nextMarkerId,
      i,
      j,
      dot,
      label,
      value: NaN,
      unit: "",
      visible: true
    };
    this.nextMarkerId += 1;
    this.markers.push(marker);
    this.positionMarker(marker);
    await this.updateSingleMarker(marker, this.currentFrame);
  }

  positionMarker(marker) {
    if (!marker?.visible) return;
    const gi = this.model.getCurrentGroupInfo();
    if (!gi) return;
    const wrapRect = this.canvas.parentElement.getBoundingClientRect();
    const canvasRect = this.canvas.getBoundingClientRect();
    const x = ((marker.i + 0.5) / Math.max(1, gi.cropWidth)) * canvasRect.width;
    const y = ((marker.j + 0.5) / Math.max(1, gi.cropHeight)) * canvasRect.height;
    const xInWrap = canvasRect.left - wrapRect.left + x;
    const yInWrap = canvasRect.top - wrapRect.top + y;
    marker.dot.style.left = `${xInWrap}px`;
    marker.dot.style.top = `${yInWrap}px`;
    marker.label.style.left = `${xInWrap}px`;
    marker.label.style.top = `${yInWrap}px`;
  }

  positionMarkers() {
    for (const marker of this.markers) this.positionMarker(marker);
  }

  async updateSingleMarker(marker, frameIndex) {
    try {
      const sample = await this.model.getHoverValue(frameIndex, marker.i, marker.j);
      const showValue = this.colormapSelect.value === "gray";
      marker.value = showValue ? Number(sample.value) : NaN;
      marker.unit = sample.unit || "";
      if (showValue && Number.isFinite(marker.value)) {
        marker.label.textContent = `${marker.value.toPrecision(6)}${marker.unit ? ` ${marker.unit}` : ""}`;
      } else {
        marker.label.textContent = "--";
      }
    } catch {
      marker.label.textContent = "--";
    }
  }

  async updateMarkersForCurrentFrame() {
    if (!this.markers.length) return;
    const token = ++this.markerUpdateToken;
    const frameIndex = this.currentFrame;
    await Promise.all(this.markers.map(async (marker) => this.updateSingleMarker(marker, frameIndex)));
    if (token !== this.markerUpdateToken) return;
    this.positionMarkers();
  }

  scheduleHoverUpdate() {
    if (this.hoverBusy) return;
    this.hoverBusy = true;
    requestAnimationFrame(async () => {
      try {
        if (!this.pendingHover) return;
        await this.updateHoverTooltip(this.pendingHover);
      } finally {
        this.hoverBusy = false;
      }
    });
  }

  async updateHoverTooltip(ev) {
    const gi = this.model.getCurrentGroupInfo();
    if (!gi) return;
    const rect = this.canvas.getBoundingClientRect();
    const x = (ev.clientX - rect.left) / Math.max(1, rect.width);
    const y = (ev.clientY - rect.top) / Math.max(1, rect.height);
    const i = Math.max(0, Math.min(gi.cropWidth - 1, Math.floor(x * gi.cropWidth)));
    const j = Math.max(0, Math.min(gi.cropHeight - 1, Math.floor(y * gi.cropHeight)));
    const ll = this.model.getLatLonAt(i, j);
    this.updateCrosshair(ev, {
      lat: ll?.lat,
      lon: ll?.lon,
      unit: "",
      value: NaN
    });

    try {
      const sample = await this.model.getHoverValue(this.currentFrame, i, j);
      const showValue = this.colormapSelect.value === "gray";
      const valTxt = Number(sample.value).toPrecision(8);
      const unitTxt = sample.unit ? ` ${sample.unit}` : "";
      const latTxt = Number.isFinite(sample.lat) ? `${sample.lat.toFixed(5)} ${sample.latUnits}` : "n/a";
      const lonTxt = Number.isFinite(sample.lon) ? `${sample.lon.toFixed(5)} ${sample.lonUnits}` : "n/a";
      const lumaTxt = Number(sample.luma01).toFixed(6);
      this.tooltip.style.display = "block";
      this.tooltip.style.left = `${ev.clientX - rect.left}px`;
      this.tooltip.style.top = `${ev.clientY - rect.top}px`;
      this.tooltip.textContent = showValue ? `${valTxt}${unitTxt}` : "--";
      if (this.cursorInfo) {
        this.cursorInfo.textContent =
          `i=${sample.i}, j=${sample.j}\n` +
          `lat=${latTxt}\n` +
          `lon=${lonTxt}\n` +
          `Y=${sample.y} U=${sample.u} V=${sample.v}\n` +
          `luma01=${lumaTxt}\n` +
          `q=${sample.quantizedY}\n` +
          (showValue ? `value=${valTxt}${unitTxt}\n` : `value=hidden (grayscale only)\n`) +
          `source=${sample.source}\n` +
          `time=${sample.timestamp}`;
      }
      this.updateCrosshair(ev, { ...sample, value: showValue ? sample.value : NaN });
    } catch (err) {
      const latTxt = Number.isFinite(ll?.lat) ? `${ll.lat.toFixed(5)} ${ll.latUnits || "degrees_north"}` : "n/a";
      const lonTxt = Number.isFinite(ll?.lon) ? `${ll.lon.toFixed(5)} ${ll.lonUnits || "degrees_east"}` : "n/a";
      const errMsg = String(err?.message || err || "").split("\n")[0];
      this.tooltip.style.display = "block";
      this.tooltip.style.left = `${ev.clientX - rect.left}px`;
      this.tooltip.style.top = `${ev.clientY - rect.top}px`;
      this.tooltip.textContent = "--";
      if (this.cursorInfo) {
        this.cursorInfo.textContent =
          `i=${i}, j=${j}\n` +
          `lat=${latTxt}\n` +
          `lon=${lonTxt}\n` +
          `value=decode unavailable\n` +
          `${errMsg ? `reason=${errMsg}` : ""}`;
      }
      if (errMsg) {
        this.statusText.textContent = `Hover decode issue: ${errMsg}`;
      }
    }
  }

  updateCrosshair(ev, sample) {
    const wrapRect = this.canvas.parentElement.getBoundingClientRect();
    const canvasRect = this.canvas.getBoundingClientRect();
    const xInWrap = ev.clientX - wrapRect.left;
    const yInWrap = ev.clientY - wrapRect.top;
    const xInCanvas = ev.clientX - canvasRect.left;
    const yInCanvas = ev.clientY - canvasRect.top;
    if (xInCanvas < 0 || yInCanvas < 0 || xInCanvas > canvasRect.width || yInCanvas > canvasRect.height) {
      this.hideCrosshair();
      return;
    }

    this.crossV.style.display = "block";
    this.crossH.style.display = "block";
    this.crossV.style.left = `${xInWrap}px`;
    this.crossH.style.top = `${yInWrap}px`;

    const lonTxt = Number.isFinite(sample.lon)
      ? `${sample.lon.toFixed(4)}`
      : "lon=n/a";
    const latTxt = Number.isFinite(sample.lat)
      ? `${sample.lat.toFixed(4)}`
      : "lat=n/a";
    const hasValue = Number.isFinite(sample.value);
    const unitTxt = sample.unit ? ` ${sample.unit}` : "";
    const valueTxt = hasValue ? `${Number(sample.value).toPrecision(7)}${unitTxt}` : "value --";

    this.crossLonLabel.style.display = "block";
    this.crossLatLabel.style.display = "block";
    this.crossValueLabel.style.display = "none";

    this.crossLonLabel.textContent = `lon ${lonTxt}`;
    this.crossLatLabel.textContent = `lat ${latTxt}`;
    this.crossValueLabel.textContent = "";

    this.crossLonLabel.style.left = `${Math.max(6, xInWrap + 6)}px`;
    this.crossLonLabel.style.top = `6px`;
    this.crossLatLabel.style.left = `6px`;
    this.crossLatLabel.style.top = `${Math.max(6, yInWrap - 10)}px`;
    this.crossValueLabel.style.left = `${xInWrap + 8}px`;
    this.crossValueLabel.style.top = `${Math.max(24, yInWrap + 8)}px`;
  }

  hideCrosshair() {
    this.crossV.style.display = "none";
    this.crossH.style.display = "none";
    this.crossLonLabel.style.display = "none";
    this.crossLatLabel.style.display = "none";
    this.crossValueLabel.style.display = "none";
  }
}

const viewer = new AvencViewer();
viewer.initialize().catch((err) => {
  const status = document.getElementById("statusText");
  status.textContent = `Initialization error: ${err.message || err}`;
});
