// libjs/hostlock.mjs -- on a host that asks for it, builds, tests and
// measurements run only while holding a build slot.
//
// A host asks by having ~/.c4-require-lock. Helios does: it runs one job
// at a time, so that a test's timings and a benchmark's numbers are its
// own and not whatever else runs beside it. Holding a slot is having one
// of the slot's lock files open, as a job under ~/bin/c4-lock or
// `flock /tmp/c4-build.lock CMD` does; every process the job starts
// inherits it. C4_NO_LOCK_CHECK=1 skips the check, for a run that times
// nothing and is over in seconds.
import { existsSync, readdirSync, readlinkSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

export function holdsSlot() {
  let fds;
  try { fds = readdirSync('/proc/self/fd'); } catch { return true; }   // no /proc: nothing to check
  for (const fd of fds) {
    try { if (/\/c4-build\.lock(\.\d+)?$/.test(readlinkSync(`/proc/self/fd/${fd}`))) return true; } catch {}
  }
  return false;
}

export function requireSlot(what) {
  if (process.env.C4_NO_LOCK_CHECK) return;
  let asks = false;
  try { asks = existsSync(join(homedir(), '.c4-require-lock')); } catch {}
  if (!asks || holdsSlot()) return;
  console.error(`${what}: this host runs builds, tests and measurements one at a time: run it under ` +
    `~/bin/c4-lock (the host asks: ~/.c4-require-lock; C4_NO_LOCK_CHECK=1 for a run that times nothing)`);
  process.exit(2);
}
