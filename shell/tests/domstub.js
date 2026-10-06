/* 极简 DOM 打桩：够跑 llm_web 前端脚本用（不需要真浏览器、不用装 jsdom）
 * 只实现前端用到的那点东西：getElementById / createElement / appendChild /
 * classList / textContent / innerHTML / value / addEventListener / click
 */
class El {
  constructor(tag) {
    this.tagName = tag;
    this.children = [];
    this.style = {};
    this._listeners = {};
    this._text = '';
    this._html = '';
    this._value = '';
    this.checked = false;
    this.scrollTop = 0;
    this.scrollHeight = 100;
    this._set = new Set();
    const s = this._set;
    this.classList = {
      add:      c => s.add(c),
      remove:   c => s.delete(c),
      contains: c => s.has(c),
      toggle:   (c, f) => { if (f === undefined) f = !s.has(c); f ? s.add(c) : s.delete(c); return f; }
    };
  }
  set className(v) { this._set.clear(); String(v).split(/\s+/).filter(Boolean).forEach(c => this._set.add(c)); }
  get className() { return [...this._set].join(' '); }
  set innerHTML(v) { this._html = String(v); this.children = []; }
  get innerHTML() { return this._html; }
  set textContent(v) { this._text = String(v); }
  get textContent() { return this._text; }
  set value(v) { this._value = String(v); }          // 真 DOM 里 value 是 DOMString
  get value() { return this._value; }
  appendChild(c) { this.children.push(c); return c; }
  removeChild(c) { this.children = this.children.filter(x => x !== c); }
  remove() { this._removed = true; }
  addEventListener(t, f) { (this._listeners[t] = this._listeners[t] || []).push(f); }
  hasListener(t) { return !!(this._listeners[t] || []).length; }
  click() { (this._listeners['click'] || []).forEach(f => f()); }
  // 手动派发事件：勾选框这类"改了 checked 才算点过"的元素用得上
  // （真 DOM 里点勾选框 = 翻转 checked + 派发 click/change）
  dispatch(type) { (this._listeners[type] || []).forEach(f => f()); }
  focus() {}
  querySelector() { return new El('div'); }
}

// 把 HTML 里所有 id="..." 的元素建出来（顺带带上 class / value / checked）
function buildDom(html) {
  const byId = {};
  for (const m of html.matchAll(/<[^>]*\bid="([^"]+)"[^>]*>/g)) {
    const tagTxt = m[0];
    const e = new El('div');
    e.id = m[1];
    const cm = tagTxt.match(/class="([^"]*)"/);
    if (cm) e.className = cm[1];
    const vm = tagTxt.match(/value="([^"]*)"/);
    if (vm) e.value = vm[1];
    if (/\bchecked\b/.test(tagTxt)) e.checked = true;
    byId[e.id] = e;
  }
  return byId;
}

function installGlobals(byId, BASE, realFetch) {
  const store = {};
  global.localStorage = {
    getItem: k => (k in store ? store[k] : null),
    setItem: (k, v) => { store[k] = String(v); },
    removeItem: k => { delete store[k]; }
  };
  global.document = { getElementById: id => byId[id] || null, createElement: t => new El(t) };
  global.confirm = () => true;
  global.prompt  = () => global.__promptAnswer;
  global.fetch = (url, opt) => realFetch(url.startsWith('http') ? url : BASE + url, opt);
}

const sleep = ms => new Promise(r => setTimeout(r, ms));

// 轮询等待条件成立（这块板子慢，固定 sleep 不可靠）
async function waitFor(fn, timeoutMs = 5000, stepMs = 100) {
  const t0 = Date.now();
  for (;;) {
    let v;
    try { v = fn(); } catch (e) { v = false; }
    if (v) return v;
    if (Date.now() - t0 > timeoutMs) return false;
    await sleep(stepMs);
  }
}

// 递归收集满足条件的元素（用来找 .tools 轨迹块之类）
function collect(el, pred, acc = []) {
  if (pred(el)) acc.push(el);
  for (const c of el.children) collect(c, pred, acc);
  return acc;
}

module.exports = { El, buildDom, installGlobals, sleep, waitFor, collect };
