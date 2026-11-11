'use strict';
'require view';
'require rpc';

const callRadios = rpc.declare({
	object: 'spectral',
	method: 'radios',
	expect: { radios: [] },
	reject: true
});

const callStatus = rpc.declare({
	object: 'spectral',
	method: 'status',
	reject: true
});

const callStart = rpc.declare({
	object: 'spectral',
	method: 'start',
	params: { phy: '', mode: '', band: '', all: false, freq: 0, width: 0, bins: 0, dwell: 0 },
	reject: true
});

const callStop = rpc.declare({
	object: 'spectral',
	method: 'stop',
	reject: true
});

const callFrames = rpc.declare({
	object: 'spectral',
	method: 'frames',
	params: [ 'since' ],
	reject: true
});

const DBM_OFFSET = 160;
const OCC_NONE = 255;
const POLL_MS = 100;
const PREFS_KEY = 'luci-app-spectral';
const LVL_MIN = -130;
const LVL_N = 110;
const M = { l: 46, r: 12, t: 12, b: 30 };

const C = {
	bg: '#0d1117',
	grid: 'rgba(255,255,255,0.08)',
	text: '#9aa4b2',
	avg: '#e6edf3',
	peak: '#ff5c7a',
	occ: 'rgba(63,185,80,0.35)',
	cursor: 'rgba(255,255,255,0.55)',
	ap: 'rgba(210,168,255,0.10)',
	apText: '#d2a8ff'
};

function lut(stops) {
	const out = new Uint8Array(256 * 3);

	for (let i = 0; i < 256; i++) {
		const t = i / 255;
		let k = 1;

		while (k < stops.length - 1 && stops[k][0] < t)
			k++;

		const a = stops[k - 1], b = stops[k];
		const u = (t - a[0]) / (b[0] - a[0]);

		for (let c = 0; c < 3; c++)
			out[i * 3 + c] = Math.round(a[1][c] + (b[1][c] - a[1][c]) * u);
	}

	return out;
}

/* waterfall: power; density: how often a level shows up */
const WF_LUT = lut([
	[ 0.00, [ 6, 8, 28 ] ], [ 0.25, [ 28, 48, 150 ] ], [ 0.45, [ 20, 150, 200 ] ],
	[ 0.62, [ 70, 200, 90 ] ], [ 0.78, [ 245, 210, 45 ] ], [ 0.90, [ 240, 80, 35 ] ],
	[ 1.00, [ 255, 235, 225 ] ]
]);
const DENS_LUT = lut([
	[ 0.00, [ 20, 30, 110 ] ], [ 0.30, [ 20, 120, 210 ] ], [ 0.55, [ 40, 200, 120 ] ],
	[ 0.75, [ 240, 220, 50 ] ], [ 1.00, [ 240, 50, 40 ] ]
]);

function decode(s) {
	const bin = atob(s || ''), out = new Uint8Array(bin.length);

	for (let i = 0; i < bin.length; i++)
		out[i] = bin.charCodeAt(i);

	return out;
}

function chanNum(f) {
	if (f == 2484)
		return 14;
	if (f >= 2407 && f < 2484)
		return (f - 2407) / 5;
	if (f > 5000 && f < 5950)
		return (f - 5000) / 5;
	return 0;
}

/* Standard 20 MHz channel centres that fit in [lo, hi] */
function channelsIn(lo, hi) {
	const out = [];

	for (let c = 1; c <= 13; c++)
		out.push(2407 + 5 * c);
	out.push(2484);
	for (let c = 36; c <= 144; c += 4)
		out.push(5000 + 5 * c);
	for (let c = 149; c <= 177; c += 4)
		out.push(5000 + 5 * c);

	return out.filter(f => f - 8 >= lo && f + 8 <= hi);
}

function bandLabel(b) {
	return b == '5g' ? _('5 GHz') : b == '2g' ? _('2.4 GHz') : _('Both bands');
}

function loadPrefs() {
	try {
		return JSON.parse(localStorage.getItem(PREFS_KEY)) || {};
	}
	catch (e) {
		return {};
	}
}

function savePrefs(p) {
	try {
		localStorage.setItem(PREFS_KEY, JSON.stringify(p));
	}
	catch (e) {}
}

function option(value, label, selected, disabled) {
	return E('option', {
		value: value,
		selected: selected ? '' : null,
		disabled: disabled ? '' : null
	}, label);
}

function field(label, el) {
	return E('label', { 'class': 'spx-field' }, [ E('span', {}, label), el ]);
}

function makeCanvas(h) {
	const el = E('canvas', { style: 'height:' + h + 'px' });

	return { el: el, h: h, w: 0, ctx: el.getContext('2d') };
}

function fitCanvas(cv) {
	const dpr = window.devicePixelRatio || 1, w = cv.el.clientWidth;

	if (!w)
		return false;

	cv.el.width = Math.round(w * dpr);
	cv.el.height = Math.round(cv.h * dpr);
	cv.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
	cv.w = w;
	return true;
}

function plotBox(cv) {
	return { l: M.l, r: cv.w - M.r, t: M.t, b: cv.h - M.b };
}

function drawEmpty(cv, text) {
	const ctx = cv.ctx;

	ctx.fillStyle = C.bg;
	ctx.fillRect(0, 0, cv.w, cv.h);
	ctx.fillStyle = C.text;
	ctx.font = '13px sans-serif';
	ctx.textAlign = 'center';
	ctx.textBaseline = 'middle';
	ctx.fillText(text, cv.w / 2, cv.h / 2);
}

const CSS = [
	'.spx-bar{display:flex;flex-wrap:wrap;gap:.6em 1.2em;align-items:flex-end;margin-bottom:.6em}',
	'.spx-field{display:flex;flex-direction:column;gap:.25em;font-size:90%}',
	'.spx-field select{width:auto;min-width:8em}',
	'.spx-actions{display:flex;flex-wrap:wrap;gap:.5em;align-items:center}',
	'.spx-bands{display:inline-flex;border-radius:4px;overflow:hidden;border:1px solid rgba(128,128,128,.5)}',
	'.spx-bands button{border:0;border-radius:0;margin:0;padding:.35em .9em;background:transparent;color:inherit;cursor:pointer}',
	'.spx-bands button.active{background:#1f6feb;color:#fff}',
	'.spx-bands button:disabled{opacity:.4;cursor:default}',
	'.spx-status{display:flex;align-items:center;gap:.5em;margin:.4em 0;min-height:1.5em}',
	'.spx-dot{flex:none;width:.65em;height:.65em;border-radius:50%;background:#8b949e}',
	'.spx-dot.on{background:#3fb950;box-shadow:0 0 6px #3fb950}',
	'.spx-dot.busy{background:#d29922}',
	'.spx-dot.err{background:#f85149}',
	'.spx-panel{background:' + C.bg + ';border-radius:6px;overflow:hidden;margin:.3em 0}',
	'.spx-panel canvas{display:block;width:100%;cursor:crosshair}',
	'.spx-legend{display:flex;flex-wrap:wrap;gap:.4em 1.2em;font-size:85%;margin:.3em 0}',
	'.spx-legend i{display:inline-block;width:1.4em;height:.3em;vertical-align:middle;margin-right:.4em;border-radius:2px}',
	'.spx-readout{font-family:monospace;font-size:90%;min-height:1.5em;margin:.3em 0;white-space:pre-wrap}',
	'.spx-auto{display:flex;align-items:center;gap:.4em;font-size:90%}',
	'.spx-hint{display:none;margin:.5em 0}',
	'.spx-sub{font-size:85%;opacity:.75;margin:.6em 0 .2em}'
].join('\n');

/* One band: density snapshot, waterfall and channel occupancy */
class Panel {
	constructor(owner, v) {
		const n = v.grid.n;

		this.owner = owner;
		this.band = v.band;
		this.start = v.grid.start;
		this.res = v.grid.res;
		this.n = n;
		this.seq = 0;
		this.avg = new Float32Array(n).fill(NaN);
		this.max = new Float32Array(n).fill(NaN);
		this.peak = new Float32Array(n).fill(-Infinity);
		this.occ = new Float32Array(n).fill(NaN);
		this.dens = new Float32Array(n * LVL_N);
		this.hover = null;
		this.lastRange = null;
		this.rowH = 1;

		this.cv = {
			spec: makeCanvas(owner.compact ? 220 : 280),
			wf: makeCanvas(owner.compact ? 200 : 300),
			util: makeCanvas(110)
		};
		this.wfBuf = document.createElement('canvas');
		this.densBuf = document.createElement('canvas');
		this.readout = E('div', { 'class': 'spx-readout' }, _('Hover the plots to read values.'));

		this.root = E('div', { 'class': 'cbi-section' }, [
			E('h3', this.title()),
			E('div', { 'class': 'spx-panel' }, this.cv.spec.el),
			this.readout,
			E('div', { 'class': 'spx-sub' }, _('Waterfall')),
			E('div', { 'class': 'spx-panel' }, this.cv.wf.el),
			E('div', { 'class': 'spx-sub' }, _('Channel occupancy')),
			E('div', { 'class': 'spx-panel' }, this.cv.util.el)
		]);

		[ this.cv.spec.el, this.cv.wf.el ].forEach(c => {
			c.addEventListener('mousemove', ev => {
				this.hover = ev.offsetX;
				this.owner.requestDraw();
			});
			c.addEventListener('mouseleave', () => {
				this.hover = null;
				this.owner.requestDraw();
			});
		});
	}

	title() {
		const lo = this.start, hi = this.start + this.n * this.res;

		return bandLabel(this.band) + ' · ' + lo.toFixed(0) + '-' + hi.toFixed(0) + ' MHz';
	}

	span() {
		return [ this.start, this.start + this.n * this.res ];
	}

	ingest(f) {
		const mx = decode(f.max), av = decode(f.avg), oc = decode(f.occ), n = this.n;

		if (mx.length != n || f.seq <= this.seq)
			return;

		for (let i = 0; i < n; i++) {
			if (mx[i]) {
				const m = mx[i] - DBM_OFFSET, a = av[i] - DBM_OFFSET, row = i * LVL_N;

				this.max[i] = m;
				this.avg[i] = a;
				if (m > this.peak[i])
					this.peak[i] = m;

				/* decay only cells that got data, so a slow sweep keeps its picture */
				for (let l = 0; l < LVL_N; l++)
					this.dens[row + l] *= 0.96;

				const la = Math.round(a - LVL_MIN), lm = Math.round(m - LVL_MIN);
				if (la >= 0 && la < LVL_N)
					this.dens[row + la] += 1;
				if (lm >= 0 && lm < LVL_N)
					this.dens[row + lm] += 0.6;
			}

			if (oc[i] != OCC_NONE)
				this.occ[i] = isNaN(this.occ[i]) ? oc[i] : this.occ[i] * 0.9 + oc[i] * 0.1;
		}

		this.seq = f.seq;
		this.pushRow(this.owner.el.wfsrc.value == 'avg' ? this.avg : this.max);
	}

	resetPeak() {
		this.peak.fill(-Infinity);
		this.dens.fill(0);
	}

	fit() {
		const dpr = window.devicePixelRatio || 1;

		fitCanvas(this.cv.spec);
		fitCanvas(this.cv.util);
		if (fitCanvas(this.cv.wf)) {
			const w = Math.max(1, Math.round((this.cv.wf.w - M.l - M.r) * dpr));
			const h = Math.max(1, Math.round((this.cv.wf.h - M.t - M.b) * dpr));

			if (w != this.wfBuf.width || h != this.wfBuf.height) {
				const old = this.wfBuf;

				this.wfBuf = document.createElement('canvas');
				this.wfBuf.width = w;
				this.wfBuf.height = h;
				const ctx = this.wfBuf.getContext('2d');
				ctx.fillStyle = C.bg;
				ctx.fillRect(0, 0, w, h);
				if (old.width > 1 && old.height > 1)
					ctx.drawImage(old, 0, 0, w, h);
			}

			this.rowH = Math.max(1, Math.round(dpr));
		}
	}

	range() {
		const v = this.owner.el.range.value;

		if (v != 'auto')
			return v.split(':').map(Number);

		const vals = [];
		let hi = -Infinity;

		for (let i = 0; i < this.n; i++) {
			if (!isNaN(this.avg[i]))
				vals.push(this.avg[i]);
			if (this.peak[i] > hi)
				hi = this.peak[i];
		}

		if (!vals.length)
			return this.lastRange || [ -110, -30 ];

		/* low percentile so a few dead bins don't stretch the scale */
		vals.sort((a, b) => a - b);
		const tlo = Math.floor((vals[Math.floor(vals.length * 0.02)] - 5) / 10) * 10;
		let thi = Math.ceil((hi + 5) / 10) * 10;

		if (thi - tlo < 40)
			thi = tlo + 40;

		const cur = this.lastRange;
		if (cur && tlo >= cur[0] && tlo <= cur[0] + 10 && thi <= cur[1] && thi >= cur[1] - 10)
			return cur;

		this.lastRange = [ tlo, thi ];
		return this.lastRange;
	}

	pushRow(src) {
		const buf = this.wfBuf, rh = this.rowH, w = buf.width, h = buf.height, n = this.n;

		if (w < 2 || h <= rh)
			return;

		const ctx = buf.getContext('2d');
		ctx.drawImage(buf, 0, 0, w, h - rh, 0, rh, w, h - rh);

		const img = ctx.createImageData(w, rh), d = img.data;
		const r = this.range(), k = 255 / (r[1] - r[0]);

		for (let x = 0; x < w; x++) {
			const c0 = Math.floor(x * n / w), c1 = Math.max(c0 + 1, Math.floor((x + 1) * n / w));
			let v = -Infinity, cr = 13, cg = 17, cb = 23;

			for (let c = c0; c < c1; c++)
				if (src[c] > v)
					v = src[c];

			if (v > -Infinity) {
				const idx = Math.max(0, Math.min(255, Math.round((v - r[0]) * k))) * 3;

				cr = WF_LUT[idx];
				cg = WF_LUT[idx + 1];
				cb = WF_LUT[idx + 2];
			}

			for (let y = 0; y < rh; y++) {
				const o = (y * w + x) * 4;

				d[o] = cr;
				d[o + 1] = cg;
				d[o + 2] = cb;
				d[o + 3] = 255;
			}
		}

		ctx.putImageData(img, 0, 0);
	}

	draw() {
		this.drawSpectrum();
		this.drawWaterfall();
		this.drawUtil();
		this.updateReadout();
	}

	drawFreqAxis(ctx, box) {
		const span = this.span(), fx = f => box.l + (f - span[0]) / (span[1] - span[0]) * (box.r - box.l);
		let lastX = -Infinity;

		ctx.font = '11px sans-serif';
		ctx.textAlign = 'center';
		ctx.textBaseline = 'top';
		ctx.fillStyle = C.text;

		channelsIn(span[0], span[1]).forEach(f => {
			const x = fx(f);

			if (x - lastX >= 22) {
				ctx.fillText(chanNum(f), x, box.b + 4);
				lastX = x;
			}
		});

		ctx.textAlign = 'left';
		ctx.fillText(span[0].toFixed(0) + ' MHz', box.l, box.b + 17);
		ctx.textAlign = 'right';
		ctx.fillText(span[1].toFixed(0) + ' MHz', box.r, box.b + 17);
	}

	drawCursor(ctx, box) {
		if (this.hover == null || this.hover < box.l || this.hover > box.r)
			return;

		ctx.strokeStyle = C.cursor;
		ctx.lineWidth = 1;
		ctx.beginPath();
		ctx.moveTo(Math.round(this.hover) + 0.5, box.t);
		ctx.lineTo(Math.round(this.hover) + 0.5, box.b);
		ctx.stroke();
	}

	drawDensity(ctx, box, r) {
		const pw = Math.max(1, Math.round(box.r - box.l)), ph = Math.max(1, Math.round(box.b - box.t));
		const buf = this.densBuf, n = this.n;

		if (buf.width != pw || buf.height != ph) {
			buf.width = pw;
			buf.height = ph;
		}

		const bctx = buf.getContext('2d'), img = bctx.createImageData(pw, ph), d = img.data;
		const col = new Float32Array(LVL_N);

		for (let x = 0; x < pw; x++) {
			const c0 = Math.floor(x * n / pw), c1 = Math.max(c0 + 1, Math.floor((x + 1) * n / pw));
			let top = 0;

			col.fill(0);
			for (let c = c0; c < c1; c++)
				for (let l = 0; l < LVL_N; l++) {
					const v = this.dens[c * LVL_N + l];

					if (v > col[l])
						col[l] = v;
					if (v > top)
						top = v;
				}

			if (!top)
				continue;

			for (let y = 0; y < ph; y++) {
				const lvl = r[1] - (y + 0.5) / ph * (r[1] - r[0]);
				const l = Math.round(lvl - LVL_MIN);

				if (l < 0 || l >= LVL_N || !col[l])
					continue;

				const t = Math.sqrt(col[l] / top);
				if (t < 0.05)
					continue;

				const idx = Math.min(255, Math.round(t * 255)) * 3, o = (y * pw + x) * 4;
				d[o] = DENS_LUT[idx];
				d[o + 1] = DENS_LUT[idx + 1];
				d[o + 2] = DENS_LUT[idx + 2];
				d[o + 3] = Math.round(120 + 135 * t);
			}
		}

		bctx.putImageData(img, 0, 0);
		ctx.drawImage(buf, box.l, box.t, box.r - box.l, box.b - box.t);
	}

	drawSpectrum() {
		const cv = this.cv.spec, ctx = cv.ctx;

		if (!cv.w)
			return;

		const box = plotBox(cv), r = this.range(), span = this.span(), n = this.n;
		const fx = f => box.l + (f - span[0]) / (span[1] - span[0]) * (box.r - box.l);
		const cx = i => box.l + (i + 0.5) / n * (box.r - box.l);
		const y = v => box.b - (v - r[0]) / (r[1] - r[0]) * (box.b - box.t);

		ctx.fillStyle = C.bg;
		ctx.fillRect(0, 0, cv.w, cv.h);

		/* operating channels of every radio, for context */
		ctx.font = '10px sans-serif';
		ctx.textBaseline = 'top';
		ctx.textAlign = 'left';
		this.owner.radios.forEach(rd => (rd.ifaces || []).forEach(i => {
			if (!i.up || i.own || !i.freq)
				return;

			const c = i.center1 || i.freq, w = i.width || 20;
			const x0 = Math.max(box.l, fx(c - w / 2)), x1 = Math.min(box.r, fx(c + w / 2));

			if (x1 <= x0)
				return;

			ctx.fillStyle = C.ap;
			ctx.fillRect(x0, box.t, x1 - x0, box.b - box.t);
			ctx.fillStyle = C.apText;
			ctx.fillText(i.name, x0 + 3, box.t + 2);
		}));

		ctx.lineWidth = 1;
		ctx.font = '11px sans-serif';
		ctx.textAlign = 'right';
		ctx.textBaseline = 'middle';
		for (let v = r[0]; v <= r[1]; v += 10) {
			ctx.strokeStyle = C.grid;
			ctx.beginPath();
			ctx.moveTo(box.l, Math.round(y(v)) + 0.5);
			ctx.lineTo(box.r, Math.round(y(v)) + 0.5);
			ctx.stroke();
			ctx.fillStyle = C.text;
			ctx.fillText(v, box.l - 6, y(v));
		}

		this.drawFreqAxis(ctx, box);

		const show = this.owner.el.display.value;
		if (show != 'lines')
			this.drawDensity(ctx, box, r);

		ctx.save();
		ctx.beginPath();
		ctx.rect(box.l, box.t, box.r - box.l, box.b - box.t);
		ctx.clip();

		const line = (arr, color, dash, width) => {
			let pen = false;

			ctx.strokeStyle = color;
			ctx.lineWidth = width || 1;
			ctx.setLineDash(dash || []);
			ctx.beginPath();
			for (let i = 0; i < n; i++) {
				const v = arr[i];

				if (isNaN(v) || v == -Infinity) {
					pen = false;
					continue;
				}

				if (pen)
					ctx.lineTo(cx(i), y(v));
				else
					ctx.moveTo(cx(i), y(v));
				pen = true;
			}
			ctx.stroke();
		};

		if (show == 'lines') {
			const bw = (box.r - box.l) / n + 0.5, oh = (box.b - box.t) * 0.25;

			ctx.fillStyle = C.occ;
			for (let i = 0; i < n; i++)
				if (this.occ[i] > 0)
					ctx.fillRect(cx(i) - bw / 2, box.b - this.occ[i] / 100 * oh, bw, this.occ[i] / 100 * oh);
			line(this.max, '#f0883e');
		}

		line(this.peak, C.peak, [ 2, 2 ]);
		line(this.avg, C.avg, null, 1.2);
		ctx.setLineDash([]);
		ctx.restore();

		this.drawCursor(ctx, box);
	}

	drawWaterfall() {
		const cv = this.cv.wf, ctx = cv.ctx;

		if (!cv.w)
			return;

		const box = plotBox(cv), dpr = window.devicePixelRatio || 1;
		const st = this.owner.status, interval = (st && st.interval) || POLL_MS;
		const rowCss = this.rowH / dpr;

		ctx.fillStyle = C.bg;
		ctx.fillRect(0, 0, cv.w, cv.h);
		ctx.imageSmoothingEnabled = false;
		ctx.drawImage(this.wfBuf, box.l, box.t, box.r - box.l, box.b - box.t);

		ctx.font = '11px sans-serif';
		ctx.textAlign = 'right';
		ctx.textBaseline = 'middle';
		ctx.fillStyle = C.text;
		for (let s = 0; ; s += 5) {
			const yy = box.t + s * 1000 / interval * rowCss;

			if (yy > box.b)
				break;
			ctx.fillText(s ? '-' + s + 's' : _('now'), box.l - 6, yy);
		}

		this.drawFreqAxis(ctx, box);
		this.drawCursor(ctx, box);
	}

	drawUtil() {
		const cv = this.cv.util, ctx = cv.ctx;

		if (!cv.w)
			return;

		const box = { l: M.l, r: cv.w - M.r, t: 18, b: cv.h - 22 };
		const span = this.span(), bars = [];

		channelsIn(span[0], span[1]).forEach(f => {
			const c0 = Math.max(0, Math.floor((f - 10 - this.start) / this.res));
			const c1 = Math.min(this.n, Math.ceil((f + 10 - this.start) / this.res));
			let sum = 0, cnt = 0;

			for (let c = c0; c < c1; c++)
				if (!isNaN(this.occ[c])) {
					sum += this.occ[c];
					cnt++;
				}

			if (cnt)
				bars.push([ f, sum / cnt ]);
		});

		ctx.fillStyle = C.bg;
		ctx.fillRect(0, 0, cv.w, cv.h);

		ctx.font = '11px sans-serif';
		ctx.textAlign = 'right';
		ctx.textBaseline = 'middle';
		[ 0, 50, 100 ].forEach(v => {
			const yy = box.b - v / 100 * (box.b - box.t);

			ctx.strokeStyle = C.grid;
			ctx.beginPath();
			ctx.moveTo(box.l, Math.round(yy) + 0.5);
			ctx.lineTo(box.r, Math.round(yy) + 0.5);
			ctx.stroke();
			ctx.fillStyle = C.text;
			ctx.fillText(v + '%', box.l - 6, yy);
		});

		if (!bars.length)
			return;

		const slot = (box.r - box.l) / bars.length, bw = Math.max(2, slot * 0.7);

		ctx.textAlign = 'center';
		bars.forEach((b, i) => {
			const x = box.l + slot * (i + 0.5), v = b[1], h = v / 100 * (box.b - box.t);

			ctx.fillStyle = 'hsl(' + Math.max(0, 120 - v * 2.4) + ',65%,48%)';
			ctx.fillRect(x - bw / 2, box.b - h, bw, h);

			ctx.fillStyle = C.text;
			ctx.textBaseline = 'top';
			ctx.fillText(chanNum(b[0]), x, box.b + 5);
			if (slot >= 24) {
				ctx.textBaseline = 'bottom';
				ctx.fillText(Math.round(v) + '%', x, box.b - h - 2);
			}
		});
	}

	updateReadout() {
		const box = plotBox(this.cv.spec);

		if (this.hover == null || this.hover < box.l || this.hover > box.r) {
			this.readout.textContent = _('Hover the plots to read values.');
			return;
		}

		const i = Math.min(this.n - 1, Math.max(0, Math.floor((this.hover - box.l) / (box.r - box.l) * this.n)));
		const f = this.start + (i + 0.5) * this.res;
		const ch = channelsIn(f - 30, f + 30).filter(c => Math.abs(c - f) <= 10).reduce((best, c) =>
			(best == null || Math.abs(c - f) < Math.abs(best - f)) ? c : best, null);
		const fmt = v => (isNaN(v) || v == -Infinity) ? '-' : v.toFixed(0) + ' dBm';

		this.readout.textContent = '%.1f MHz%s · %s %s · %s %s · %s %s · %s %s'.format(
			f, ch ? ' (ch ' + chanNum(ch) + ')' : '',
			_('avg'), fmt(this.avg[i]),
			_('max'), fmt(this.max[i]),
			_('peak'), fmt(this.peak[i]),
			_('occupancy'), isNaN(this.occ[i]) ? '-' : this.occ[i].toFixed(0) + '%');
	}
}

return view.extend({
	load() {
		return Promise.all([
			callRadios().catch(e => ({ error: e })),
			callStatus().catch(() => null)
		]);
	},

	render(data) {
		const radios = data[0], status = data[1];

		if (!Array.isArray(radios))
			return E('div', { 'class': 'alert-message warning' }, [
				E('h4', _('Spectrum analyzer service unavailable')),
				E('p', _('Could not reach the spectral ubus object: %s').format(radios.error ? radios.error.message : '?')),
				E('p', _('Check that the spectrald package is installed and running: /etc/init.d/spectrald start'))
			]);

		this.radios = radios;
		this.prefs = loadPrefs();
		this.running = false;
		this.inflight = false;
		this.panels = [];
		this.panelKey = null;
		this.status = null;
		this.compact = false;
		this.band = this.prefs.band || 'all';

		this.buildControls();
		this.el.panels = E('div', {}, E('div', { 'class': 'cbi-section' },
			E('em', {}, _('Press Start to begin.'))));

		const root = E('div', { 'class': 'spx' }, [
			E('style', {}, CSS),
			E('h2', _('Spectrum Analyzer')),
			E('div', { 'class': 'cbi-map-descr' },
				_('Live spectrum from the ath10k spectral scan, in dBm per 20 MHz. Full spectrum mode hops the scanning radio across every channel in 40/80 MHz chunks. Scanning only runs while this page is open and stops by itself about 10 seconds after you leave.')),
			E('div', { 'class': 'cbi-section' }, [
				E('div', { 'class': 'spx-bar' }, [
					field(_('Mode'), this.el.mode),
					field(_('Radio'), this.el.radio),
					this.el.fBands = field(_('Band'), this.el.bands),
					this.el.fChans = field(_('Channels'), this.el.chans),
					this.el.fDwell = field(_('Dwell'), this.el.dwell),
					this.el.fChan = field(_('Channel'), this.el.chan),
					this.el.fWidth = field(_('Width'), this.el.width),
					this.el.fBins = field(_('FFT bins'), this.el.bins),
					field(_('Display'), this.el.display),
					field(_('Waterfall'), this.el.wfsrc),
					field(_('Range'), this.el.range)
				]),
				E('div', { 'class': 'spx-actions' }, [
					this.el.start, this.el.stop, this.el.reset,
					E('label', { 'class': 'spx-auto' }, [ this.el.auto, _('Start when this page opens') ])
				]),
				E('div', { 'class': 'spx-status' }, [ this.el.dot, this.el.status ]),
				this.el.hint,
				E('div', { 'class': 'spx-legend' }, [
					E('span', {}, [ E('i', { style: 'background:linear-gradient(90deg,#141e6e,#1478d2,#28c878,#f0dc32,#f03228)' }), _('Density (how often a level is seen)') ]),
					E('span', {}, [ E('i', { style: 'background:' + C.avg }), _('Average') ]),
					E('span', {}, [ E('i', { style: 'background:' + C.peak }), _('Peak hold') ]),
					E('span', {}, [ E('i', { style: 'background:' + C.apText }), _('Operating channels') ])
				])
			]),
			this.el.panels
		]);

		if (window.ResizeObserver)
			new ResizeObserver(() => this.fitAll()).observe(root);
		window.addEventListener('resize', () => this.fitAll());
		document.addEventListener('visibilitychange', () => this.onVisibility());

		this.onModeChange(false);

		if (status && status.running) {
			this.adopt(status);
		}
		else {
			this.setStatus('', status && status.error ? _('Stopped: %s').format(status.error) : _('Stopped'));
			if (this.prefs.autostart !== false && this.radios.some(r => r.spectral))
				window.setTimeout(() => this.start(), 0);
		}

		return root;
	},

	buildControls() {
		const p = this.prefs;
		const changed = () => this.onSettingsChange();

		this.el = {};
		this.el.mode = E('select', { 'class': 'cbi-input-select', change: () => this.onModeChange(true) }, [
			option('full', _('Full spectrum'), (p.mode || 'full') == 'full'),
			option('channel', _('Single channel'), p.mode == 'channel'),
			option('sweep', _('Passive scan sweep'), p.mode == 'sweep')
		]);
		this.el.radio = E('select', { 'class': 'cbi-input-select', change: () => this.onRadioChange(true) });
		this.el.bands = E('span', { 'class': 'spx-bands' }, [ '2g', '5g', 'all' ].map(b =>
			E('button', { type: 'button', 'data-band': b, click: () => this.setBand(b) },
				b == 'all' ? _('Both') : bandLabel(b))));
		this.el.chans = E('select', { 'class': 'cbi-input-select', change: changed }, [
			option('fast', _('Covering set (faster)'), p.all !== true),
			option('all', _('Every channel'), p.all === true)
		]);
		this.el.dwell = E('select', { 'class': 'cbi-input-select', change: changed },
			[ 10, 20, 30, 50, 100, 150, 200, 250 ]
				.map(d => option(d, d + ' ms' + (d == 150 ? ' (' + _('default') + ')' : ''), (p.dwell || 150) == d)));
		this.el.chan = E('select', { 'class': 'cbi-input-select', change: () => this.onChanChange(false) });
		this.el.width = E('select', { 'class': 'cbi-input-select', change: changed });
		this.el.bins = E('select', { 'class': 'cbi-input-select', change: changed },
			[ [ 0, _('Auto') ], [ 64, '64' ], [ 128, '128' ], [ 256, '256' ] ]
				.map(b => option(b[0], b[1], (p.bins || 0) == b[0])));
		this.el.display = E('select', { 'class': 'cbi-input-select', change: () => { this.savePrefs(); this.requestDraw(); } }, [
			option('density', _('Density'), p.display != 'lines'),
			option('lines', _('Lines'), p.display == 'lines')
		]);
		this.el.wfsrc = E('select', { 'class': 'cbi-input-select', change: () => this.savePrefs() }, [
			option('max', _('Maximum'), p.wfsrc != 'avg'),
			option('avg', _('Average'), p.wfsrc == 'avg')
		]);
		this.el.range = E('select', { 'class': 'cbi-input-select', change: () => { this.savePrefs(); this.requestDraw(); } },
			[ [ 'auto', _('Auto') ], [ '-110:-30', '-110 … -30 dBm' ], [ '-110:-50', '-110 … -50 dBm' ],
			  [ '-100:-20', '-100 … -20 dBm' ], [ '-120:-40', '-120 … -40 dBm' ] ]
				.map(r => option(r[0], r[1], (p.range || 'auto') == r[0])));

		this.el.start = E('button', { 'class': 'btn cbi-button cbi-button-positive', click: () => this.start() }, _('Start'));
		this.el.stop = E('button', { 'class': 'btn cbi-button cbi-button-negative', click: () => this.stop() }, _('Stop'));
		this.el.reset = E('button', { 'class': 'btn cbi-button', click: () => this.resetPeak() }, _('Reset peak and density'));
		this.el.auto = E('input', { type: 'checkbox', checked: p.autostart !== false ? '' : null, change: () => this.savePrefs() });
		this.el.dot = E('span', { 'class': 'spx-dot' });
		this.el.status = E('span', {}, '');
		this.el.hint = E('div', { 'class': 'alert-message notice spx-hint' },
			_('No FFT samples are arriving. Check that kmod-ath and kmod-ath10k-ct were built with spectral support. Some CT firmware builds do not emit spectral reports: in that case switch to kmod-ath10k with the non-CT ath10k-firmware packages.'));
	},

	busyIface(r) {
		return r ? (r.ifaces || []).find(i => i.up && !i.own) : null;
	},

	radio() {
		return this.radios.find(r => r.phy == this.el.radio.value);
	},

	radioLabel(r) {
		const bands = Object.keys(r.bands || {}).map(bandLabel).join(' + ');
		const used = this.busyIface(r);
		let s = r.phy + ' · ' + (r.driver || '?') + ' · ' + bands;

		if (!r.spectral)
			return s + ' · ' + _('no spectral support');
		if (used)
			return s + ' · ' + _('in use by %s').format(used.name) + (used.freq ? ' (ch ' + chanNum(used.freq) + ')' : '');
		return s + ' · ' + _('idle');
	},

	onModeChange(restart) {
		const mode = this.el.mode.value, scanning = mode != 'channel', p = this.prefs;
		const prev = this.el.radio.value || p.phy || 'auto';

		this.el.radio.innerHTML = '';
		if (mode == 'full')
			this.el.radio.appendChild(option('auto', _('Auto (idle scanning radio)'), prev == 'auto'));

		this.radios.forEach(r => {
			const off = !r.spectral || (scanning && this.busyIface(r));

			this.el.radio.appendChild(option(r.phy, this.radioLabel(r), prev == r.phy && !off, off));
		});

		if (!this.radio() && mode != 'full') {
			const pick = this.radios.find(r => r.spectral && (!scanning || !this.busyIface(r)));
			if (pick)
				this.el.radio.value = pick.phy;
		}

		this.onRadioChange(restart);
	},

	onRadioChange(restart) {
		const mode = this.el.mode.value, r = this.radio(), p = this.prefs;
		const show = (el, on) => { el.style.display = on ? '' : 'none'; };
		const bands = r ? Object.keys(r.bands || {}) : [ '2g', '5g' ];

		/* the band buttons drive what the radio scans */
		this.el.bands.querySelectorAll('button').forEach(btn => {
			const b = btn.getAttribute('data-band');

			btn.disabled = b == 'all' ? (mode != 'full' || bands.length < 2) : bands.indexOf(b) < 0;
		});
		if (mode == 'sweep' && this.band == 'all')
			this.band = bands[0] || '2g';
		if (this.band != 'all' && bands.indexOf(this.band) < 0)
			this.band = mode == 'full' && bands.length > 1 ? 'all' : bands[0];
		this.markBand();

		this.el.chan.innerHTML = '';
		if (r) {
			const radar = r.radar || [];

			bands.forEach(b => (r.bands[b] || []).forEach(f => {
				const label = chanNum(f) + ' (' + f + ' MHz)' + (radar.indexOf(f) >= 0 ? ' DFS' : '');
				this.el.chan.appendChild(option(f, label, f == p.chan));
			}));
		}

		const fixed = mode == 'channel' && r && !this.busyIface(r);
		show(this.el.fBands, mode != 'channel');
		show(this.el.fChans, mode == 'sweep');
		show(this.el.fDwell, mode != 'channel');
		show(this.el.fChan, fixed);
		show(this.el.fWidth, fixed);
		show(this.el.fBins, mode != 'full');
		this.el.dwell.disabled = mode == 'sweep' && r && !r.dwell;

		this.onChanChange(true);
		if (restart)
			this.onSettingsChange();
	},

	onChanChange(quiet) {
		const f = +this.el.chan.value, prev = +(this.el.width.value || this.prefs.width || 20);
		const widths = f > 5000 ? [ 20, 40, 80 ] : [ 20, 40 ];

		this.el.width.innerHTML = '';
		widths.forEach(w => this.el.width.appendChild(option(w, w + ' MHz', w == prev)));

		if (!quiet)
			this.onSettingsChange();
	},

	setBand(b) {
		this.band = b;
		this.markBand();
		this.onSettingsChange();
	},

	markBand() {
		this.el.bands.querySelectorAll('button').forEach(btn =>
			btn.classList.toggle('active', btn.getAttribute('data-band') == this.band));
	},

	onSettingsChange() {
		this.savePrefs();
		if (!this.running)
			return;

		window.clearTimeout(this.restartTimer);
		this.restartTimer = window.setTimeout(() => this.start(), 300);
	},

	params() {
		const mode = this.el.mode.value, r = this.radio();
		const p = { mode: mode };

		if (r)
			p.phy = r.phy;

		if (mode == 'full') {
			p.band = this.band;
			p.dwell = +this.el.dwell.value;
		}
		else if (mode == 'sweep') {
			p.band = this.band == 'all' ? '2g' : this.band;
			p.all = this.el.chans.value == 'all';
			p.dwell = r && r.dwell ? +this.el.dwell.value : 0;
			p.bins = +this.el.bins.value;
		}
		else {
			p.bins = +this.el.bins.value;
			if (!this.busyIface(r)) {
				p.freq = +this.el.chan.value;
				p.width = +this.el.width.value;
			}
		}

		return p;
	},

	savePrefs() {
		savePrefs({
			phy: this.el.radio.value,
			mode: this.el.mode.value,
			band: this.band,
			all: this.el.chans.value == 'all',
			dwell: +this.el.dwell.value,
			chan: +this.el.chan.value,
			width: +this.el.width.value,
			bins: +this.el.bins.value,
			display: this.el.display.value,
			wfsrc: this.el.wfsrc.value,
			range: this.el.range.value,
			autostart: this.el.auto.checked
		});
		this.prefs = loadPrefs();
	},

	start() {
		const mode = this.el.mode.value, r = this.radio();

		if (mode != 'full' && (!r || !r.spectral))
			return this.setStatus('err', _('Pick a radio with spectral scan support.'));

		this.savePrefs();
		this.running = true;
		this.startedAt = Date.now();
		this.setStatus('busy', _('Starting…'));

		return callStart(this.params()).then(res => {
			if (!res.ok)
				throw new Error(res.error || _('unknown error'));

			this.applyStatus(res);
			this.poll();
		}).catch(e => {
			this.running = false;
			this.setStatus('err', _('Start failed: %s').format(e.message));
		});
	},

	stop() {
		this.running = false;
		window.clearTimeout(this.restartTimer);
		window.clearTimeout(this.pollTimer);
		return callStop().then(() => this.setStatus('', _('Stopped'))).catch(e => this.setStatus('err', e.message));
	},

	adopt(status) {
		this.running = true;
		this.startedAt = Date.now();
		this.el.mode.value = status.mode;
		if (status.band)
			this.band = status.band;
		this.prefs.phy = status.mode == 'full' ? 'auto' : status.phy;
		this.el.radio.value = '';
		this.onModeChange(false);
		this.applyStatus(status);
		this.poll();
	},

	onVisibility() {
		if (!document.hidden && this.running && !this.inflight)
			this.start();
	},

	poll() {
		window.clearTimeout(this.pollTimer);
		if (!this.running || this.inflight || document.hidden)
			return;

		const t0 = Date.now();
		let delay = POLL_MS;

		this.inflight = true;
		callFrames(this.seq || 0).then(res => this.onFrames(res)).catch(e => {
			delay = 2000;
			this.setStatus('err', _('Lost contact with spectrald: %s').format(e.message));
		}).finally(() => {
			this.inflight = false;
			if (this.running)
				this.pollTimer = window.setTimeout(() => this.poll(), Math.max(10, delay - (Date.now() - t0)));
		});
	},

	onFrames(res) {
		/* a reply that was in flight when Stop was pressed */
		if (!this.running)
			return;

		if (!res.running) {
			if (res.reason == 'idle' && !document.hidden) {
				this.start();
				return;
			}

			this.running = false;
			this.setStatus(res.error ? 'err' : '', res.error ? _('Stopped: %s').format(res.error) : _('Stopped'));
			return;
		}

		const views = res.views || [];
		const key = res.gen + '|' + views.map(v => v.band + ':' + v.grid.n + ':' + v.grid.start).join(',');

		if (key != this.panelKey)
			this.buildPanels(views, key);

		views.forEach((v, i) => (v.frames || []).forEach(f => this.panels[i].ingest(f)));
		this.seq = res.seq;
		this.applyStatus(res);
		this.requestDraw();
	},

	buildPanels(views, key) {
		this.panelKey = key;
		this.seq = 0;
		this.compact = views.length > 1;
		this.panels = views.map(v => new Panel(this, v));
		this.el.panels.innerHTML = '';
		this.panels.forEach(p => this.el.panels.appendChild(p.root));
		window.requestAnimationFrame(() => this.fitAll());
	},

	resetPeak() {
		this.panels.forEach(p => p.resetPeak());
		this.requestDraw();
	},

	applyStatus(s) {
		const parts = [];

		this.status = s;
		if (s.mode == 'full')
			parts.push(_('Full spectrum, %s, on %s: %d chunks, %d ms per pass').format(
				bandLabel(s.band), s.phy, (s.chunks || []).length, s.sweep_ms || 0));
		else if (s.mode == 'sweep')
			parts.push(_('Sweeping %s over %d channels via %s').format(bandLabel(s.band), (s.freqs || []).length, s.iface) +
				(s.sweeps ? ', ' + _('%d ms per sweep').format(s.sweep_ms) : ''));
		else
			parts.push(_('Channel %d, %d MHz via %s').format(chanNum(s.freq), s.width, s.iface) +
				(s.shared ? ' (' + _('shared, no disruption') + ')' : ''));

		parts.push(_('%d samples/s').format(s.rate || 0));
		if (s.mode != 'full')
			parts.push(_('%d bins').format(s.bins));

		this.setStatus('on', parts.join(' · '));
		this.el.hint.style.display = (!s.samples && Date.now() - this.startedAt > 6000) ? 'block' : 'none';
	},

	setStatus(kind, text) {
		this.el.dot.className = 'spx-dot' + (kind ? ' ' + kind : '');
		this.el.status.textContent = text;
		this.el.start.disabled = this.running && kind != 'err';
		this.el.stop.disabled = !this.running;
		if (kind != 'on')
			this.el.hint.style.display = 'none';
	},

	fitAll() {
		this.panels.forEach(p => p.fit());
		this.draw();
	},

	requestDraw() {
		if (this.raf)
			return;

		this.raf = window.requestAnimationFrame(() => {
			this.raf = null;
			this.draw();
		});
	},

	draw() {
		this.panels.forEach(p => p.draw());
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
