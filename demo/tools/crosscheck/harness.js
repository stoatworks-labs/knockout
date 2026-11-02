// The browser half of demo/tools/crosscheck/crosscheck.py, injected into the
// served page BEFORE its own scripts (Page.addScriptToEvaluateOnNewDocument),
// so what is measured is the page itself: its plugin.js, its kit, its shim and
// its copy of the shaders, driven through its own controls. Not served to
// visitors: demo/.assetsignore keeps tools/ off the site.
//
// It changes three things about the page, none of which reaches the plugin:
//   - requestAnimationFrame is gated: the first frame runs (the kit's clock
//     starts at 0 on it, so every load draws the clip at time 0), and each
//     later one only when release() is called;
//   - the WebGL2 context is made with preserveDrawingBuffer, so the drawing
//     buffer can be read back after the frame;
//   - read() reads it, top row first, as RGBA8.
(() => {
  const realFrame = window.requestAnimationFrame.bind(window);
  const ko = { allow: 1, queue: [], gl: null };
  window.__knockoutCrosscheck = ko;

  window.requestAnimationFrame = (callback) => {
    if (ko.allow > 0) {
      ko.allow -= 1;
      return realFrame(callback);
    }
    ko.queue.push(callback);
    return 0;
  };
  ko.release = () => {
    const queued = ko.queue.splice(0);
    if (queued.length === 0) ko.allow += 1;
    for (const callback of queued) realFrame(callback);
  };

  const getContext = HTMLCanvasElement.prototype.getContext;
  HTMLCanvasElement.prototype.getContext = function crosscheckContext(type, attributes) {
    if (type !== 'webgl2') return getContext.call(this, type, attributes);
    const gl = getContext.call(this, type, { ...(attributes ?? {}), preserveDrawingBuffer: true });
    if (gl) ko.gl = gl;
    return gl;
  };

  ko.frames = () => Number(ko.gl?.canvas.dataset.frames ?? 0);
  ko.waitFrame = async (after) => {
    for (let i = 0; i < 1000; i += 1) {
      const error = document.querySelector('.stage__status.is-error')?.textContent;
      if (error) throw new Error(error);
      if (ko.frames() > after) return ko.frames();
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
    throw new Error(`no frame after ${after}`);
  };
  ko.read = () => {
    const gl = ko.gl;
    const w = gl.drawingBufferWidth;
    const h = gl.drawingBufferHeight;
    const pixels = new Uint8Array(w * h * 4);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    const row = w * 4;
    const flipped = new Uint8Array(pixels.length);
    for (let y = 0; y < h; y += 1) flipped.set(pixels.subarray((h - 1 - y) * row, (h - y) * row), y * row);
    let binary = '';
    for (let i = 0; i < flipped.length; i += 0x8000) binary += String.fromCharCode(...flipped.subarray(i, i + 0x8000));
    return { width: w, height: h, rgba: btoa(binary) };
  };
  ko.renderer = () => {
    const info = ko.gl.getExtension('WEBGL_debug_renderer_info');
    return ko.gl.getParameter(info ? info.UNMASKED_RENDERER_WEBGL : ko.gl.RENDERER);
  };
  ko.rowOf = (name) => [...document.querySelectorAll('.prow')].find((r) => r.querySelector('.prow__name')?.textContent === name);
  ko.status = () => document.getElementById('knockout-frame')?.textContent ?? '';
})();
