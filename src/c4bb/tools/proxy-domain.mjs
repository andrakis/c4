// proxy-domain.mjs - the domain of a reverse proxy that maps
// https://PORT.<domain> to localhost:PORT here (code-server's port proxy
// works this way). The browser gates try it before the LAN address.
//
// It is machine-specific, so it is never written in the repo: set
// C4_PROXY_DOMAIN, or put the domain on the first line of .proxy-domain
// at the repo root (gitignored). With neither, there is no proxy and
// the gates go straight to the LAN address.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..', '..');

export function proxyDomain () {
  if (process.env.C4_PROXY_DOMAIN) return process.env.C4_PROXY_DOMAIN.trim();
  try {
    return readFileSync(join(ROOT, '.proxy-domain'), 'utf8').split('\n')[0].trim() || null;
  } catch { return null; }
}

// https://PORT.<domain>, or null when no proxy is configured.
export function proxyUrl (port) {
  const d = proxyDomain();
  return d ? `https://${port}.${d}` : null;
}
