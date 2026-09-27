/* Tiny DOM / canvas helpers + formatting. */
(function () {
  const RC = window.RC;

  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  function div(cls, text) {
    return el('div', cls, text);
  }
  function canvas(w, h, cls) {
    const c = el('canvas', cls || 'pixcanvas');
    c.width = w;
    c.height = h;
    c.style.width = w + 'px';
    c.style.height = h + 'px';
    const x = c.getContext('2d');
    x.imageSmoothingEnabled = false;
    return c;
  }

  /* seconds -> mm:ss.mmm */
  function fmtTime(t) {
    if (t == null || !isFinite(t)) return '--:--.---';
    const neg = t < 0;
    t = Math.abs(t);
    const m = Math.floor(t / 60);
    const s = t - m * 60;
    const sStr = (s < 10 ? '0' : '') + s.toFixed(3);
    return (neg ? '-' : '') + String(m).padStart(2, '0') + ':' + sStr;
  }

  /* signed seconds -> +d.ddd */
  function fmtDelta(d, digits) {
    if (d == null || !isFinite(d)) return '+0.000';
    const s = d >= 0 ? '+' : '-';
    return s + Math.abs(d).toFixed(digits == null ? 3 : digits);
  }

  function clamp(v, a, b) {
    return v < a ? a : v > b ? b : v;
  }

  RC.DOM = { el, div, canvas, fmtTime, fmtDelta, clamp };
})();
