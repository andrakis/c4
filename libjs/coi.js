// coi.js - cross-origin isolation where the server cannot give it.
//
// The page needs SharedArrayBuffer for ?parallel (a worker per CPU,
// par.js) and the shared display path (shared.js), and a browser gives
// SharedArrayBuffer only to a page that is cross-origin isolated: one
// served with the two headers serve.mjs --isolate adds,
//
//   Cross-Origin-Opener-Policy: same-origin
//   Cross-Origin-Embedder-Policy: require-corp
//
// A static host such as GitHub Pages cannot add headers. This file adds
// them instead. It is two things:
//
// - in the page (index.html loads it, a plain script before app.js), it
//   registers itself as a service worker for libjs/, when the page is not
//   isolated already and can have one (a secure context: https, or
//   localhost). The first visit has no worker yet, so it reloads the page
//   once, and sets globalThis.c4coi.reloading, which app.js waits on, so
//   that no machine boots only to be thrown away. ?coi=0 unregisters it.
// - as that service worker, it fetches everything under libjs/ (the page,
//   its workers' scripts, the images they read) and adds the two headers
//   to the response. Its scope is libjs/, its own folder, so it covers the
//   page in libjs/web/ and the workers in libjs/ (a worker's script needs
//   the headers too, or an isolated page may not start it).
//
// Behind a server that sends the headers, the page is isolated before
// this runs, and it does nothing.

if (typeof window === 'undefined') {
  // the service worker
  self.addEventListener('install', () => self.skipWaiting());
  self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));
  self.addEventListener('fetch', e => {
    const r = e.request;
    if (new URL(r.url).origin !== self.location.origin) return;
    if (r.cache === 'only-if-cached' && r.mode !== 'same-origin') return;
    e.respondWith(fetch(r).then(res => {
      if (res.status === 0) return res;
      const h = new Headers(res.headers);
      h.set('Cross-Origin-Opener-Policy', 'same-origin');
      h.set('Cross-Origin-Embedder-Policy', 'require-corp');
      return new Response(res.body, { status: res.status, statusText: res.statusText, headers: h });
    }));
  });
} else {
  // the page
  const c4coi = globalThis.c4coi = { reloading: false };
  const sw = navigator.serviceWorker;
  const script = document.currentScript.src;
  const scope = new URL('./', script).href;
  const KEY = 'c4coi-reloaded';
  const once = () => {
    try {
      if (sessionStorage.getItem(KEY)) return false;
      sessionStorage.setItem(KEY, '1');
      return true;
    } catch { return false; }
  };
  if (new URLSearchParams(location.search).get('coi') === '0') {
    if (sw) sw.getRegistrations().then(rs => rs.filter(r => r.scope === scope).forEach(r => r.unregister()));
  } else if (globalThis.crossOriginIsolated) {
    try { sessionStorage.removeItem(KEY); } catch {}
  } else if (window.isSecureContext && sw) {
    // reload once, when the worker controls the page from then on: on the
    // first visit, and after a reload that bypassed it (Shift-Reload).
    // Never twice in a session: a browser that will not isolate the page
    // so gets it without, and ?parallel falls back to one thread.
    if (once()) {
      c4coi.reloading = true;
      // sw.ready: once the worker is active, a reload goes through it
      sw.register(script, { scope }).then(() => sw.ready).then(() => location.reload(), err => {
        console.warn('coi.js: no service worker, so the page is not cross-origin isolated:', err);
        c4coi.reloading = false;
        c4coi.done?.();
      });
    } else {
      console.warn('coi.js: the page is still not cross-origin isolated; running without SharedArrayBuffer');
    }
  }
  // app.js awaits this: at once, unless the page is about to reload
  c4coi.ready = new Promise(r => { c4coi.done = r; if (!c4coi.reloading) r(); });
}
