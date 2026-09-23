// cdp.mjs - just enough of the Chrome DevTools Protocol to drive a page
// in one of the user's own browsers, with no Playwright.
//
//   const b = await Browser.connect('http://127.0.0.1:9224');
//   const tab = await b.newTab();            // a tab of our own
//   await tab.goto(url);
//   await tab.eval('document.title');
//   await tab.type('ps'); await tab.key('Enter'); await tab.key('c', { ctrl: true });
//   await tab.close(); b.disconnect();
//
// CARE: the browser is the user's. Only the tab this opens is ever
// closed, and disconnecting kills nothing.

export class Browser {
  static async connect(http) {
    const v = await (await fetch(`${http}/json/version`, { signal: AbortSignal.timeout(5000) })).json();
    const b = new Browser();
    b.version = v.Browser;
    await b.open(v.webSocketDebuggerUrl);
    return b;
  }

  open(url) {
    this.ws = new WebSocket(url);
    this.nextId = 1;
    this.pending = new Map();
    this.handlers = new Set();
    this.ws.onmessage = e => {
      const m = JSON.parse(typeof e.data === 'string' ? e.data : Buffer.from(e.data).toString());
      if (m.id && this.pending.has(m.id)) {
        const { res, rej } = this.pending.get(m.id);
        this.pending.delete(m.id);
        if (m.error) rej(new Error(`${m.error.message} (${m.error.code})`)); else res(m.result);
      } else if (m.method) for (const h of this.handlers) h(m);
    };
    return new Promise((res, rej) => { this.ws.onopen = res; this.ws.onerror = () => rej(new Error('CDP socket failed')); });
  }

  send(method, params = {}, sessionId) {
    const id = this.nextId++;
    const msg = { id, method, params };
    if (sessionId) msg.sessionId = sessionId;
    this.ws.send(JSON.stringify(msg));
    return new Promise((res, rej) => {
      this.pending.set(id, { res, rej });
      setTimeout(() => { if (this.pending.has(id)) { this.pending.delete(id); rej(new Error(`${method} timed out`)); } }, 30000);
    });
  }

  async newTab(background = true) {
    const { targetId } = await this.send('Target.createTarget', { url: 'about:blank', background });
    const { sessionId } = await this.send('Target.attachToTarget', { targetId, flatten: true });
    const tab = new Tab(this, targetId, sessionId);
    await tab.send('Page.enable');
    await tab.send('Runtime.enable');
    return tab;
  }

  disconnect() { try { this.ws.close(); } catch { /* already gone */ } }
}

export class Tab {
  constructor(browser, targetId, sessionId) {
    this.b = browser;
    this.targetId = targetId;
    this.sessionId = sessionId;
    this.errors = [];
    this.console = [];
    browser.handlers.add(m => {
      if (m.sessionId !== sessionId) return;
      if (m.method === 'Runtime.exceptionThrown') {
        const d = m.params.exceptionDetails;
        this.errors.push((d.exception && d.exception.description) || d.text);
      } else if (m.method === 'Runtime.consoleAPICalled') {
        this.console.push(m.params.args.map(a => a.value ?? a.description).join(' '));
      }
    });
  }

  send(method, params) { return this.b.send(method, params, this.sessionId); }

  async goto(url) {
    await this.send('Page.navigate', { url });
    await this.waitFor('document.readyState === "complete"', 30000);
    // Chrome delivers Input.dispatch* events only to the tab in front, so
    // a gate that types has to be looking at its own tab.
    await this.send('Page.bringToFront');
  }

  async eval(expression) {
    const r = await this.send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
    return r.result.value;
  }

  async waitFor(expression, timeout = 30000, every = 200) {
    const end = Date.now() + timeout;
    let last;
    while (Date.now() < end) {
      try { last = await this.eval(expression); if (last) return last; } catch { /* page mid-navigation */ }
      await new Promise(r => setTimeout(r, every));
    }
    return false;
  }

  // Keys go through Input.dispatchKeyEvent, so the page's own keydown
  // listeners see them exactly as they would a person's.
  async key(key, { ctrl = false, shift = false } = {}) {
    const named = {
      Enter: { code: 'Enter', vk: 13, text: '\r' }, Backspace: { code: 'Backspace', vk: 8 },
      Tab: { code: 'Tab', vk: 9 }, Escape: { code: 'Escape', vk: 27 },
    }[key];
    const modifiers = (ctrl ? 2 : 0) | (shift ? 8 : 0);
    let p;
    if (named) p = { key, code: named.code, windowsVirtualKeyCode: named.vk, text: ctrl ? undefined : named.text };
    else {
      const up = key.toUpperCase();
      const vk = /[a-z]/i.test(key) ? up.charCodeAt(0) : /[0-9]/.test(key) ? key.charCodeAt(0) : 0;
      p = { key, code: /[a-z]/i.test(key) ? `Key${up}` : undefined, windowsVirtualKeyCode: vk, text: ctrl ? undefined : key };
    }
    await this.send('Input.dispatchKeyEvent', { type: p.text ? 'keyDown' : 'rawKeyDown', modifiers, ...p });
    await this.send('Input.dispatchKeyEvent', { type: 'keyUp', modifiers, key: p.key, code: p.code, windowsVirtualKeyCode: p.windowsVirtualKeyCode });
  }

  async type(s) { for (const ch of s) await this.key(ch === '\n' ? 'Enter' : ch); }

  async mouse(type, x, y, button = 'none', buttons = 0) {
    await this.send('Input.dispatchMouseEvent', { type, x, y, button, buttons, clickCount: type === 'mouseMoved' ? 0 : 1 });
  }

  async screenshot() {
    const r = await this.send('Page.captureScreenshot', { format: 'png' });
    return Buffer.from(r.data, 'base64');
  }

  async close() {
    try { await this.b.send('Target.closeTarget', { targetId: this.targetId }); } catch { /* gone */ }
  }
}
