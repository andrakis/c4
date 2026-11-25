// libjs/hostlock.mjs -- on a host that asks for it, builds, tests and
// measurements run only while holding a build slot.
//
// A host asks by having ~/.c4-require-lock. Helios does: it runs its jobs
// each on processors of its own, so that a test's timings and a
// benchmark's numbers are its own and not whatever else runs beside it.
// Holding a slot is having one of the slot's lock files open, as a job
// under ~/bin/c4-lock or `flock /tmp/c4-build.lock CMD` does. A shell or
// make passes the open file on to what it starts; node does not (its
// children get only their stdin, stdout and stderr), so a node program
// that holds the slot names itself in C4_SLOT_HELD, and what it starts
// passes when that process still has the lock open. C4_NO_LOCK_CHECK=1
// skips the check, for a run that times nothing and is over in seconds.
import { existsSync, readdirSync, readlinkSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

function hasLock(pid) {   // pid's open files include a slot's lock file
  let fds;
  try { fds = readdirSync(`/proc/${pid}/fd`); } catch { return false; }
  for (const fd of fds) {
    try { if (/\/c4-build\.lock(\.\d+)?$/.test(readlinkSync(`/proc/${pid}/fd/${fd}`))) return true; } catch {}
  }
  return false;
}

export function holdsSlot() {
  if (!existsSync('/proc/self/fd')) return true;    // no /proc: nothing to check
  if (hasLock('self')) return true;
  const held = process.env.C4_SLOT_HELD;            // started by a node program that holds it
  return !!held && /^\d+$/.test(held) && hasLock(held);
}

export function requireSlot(what) {
  if (process.env.C4_NO_LOCK_CHECK) return;
  let asks = false;
  try { asks = existsSync(join(homedir(), '.c4-require-lock')); } catch {}
  if (!asks) return;
  if (existsSync('/proc/self/fd') && hasLock('self')) {
    process.env.C4_SLOT_HELD = String(process.pid);   // for what this process starts
    return;
  }
  if (holdsSlot()) return;
  console.error(`${what}: this host runs builds, tests and measurements in build slots: run it under ` +
    `~/bin/c4-lock (the host asks: ~/.c4-require-lock; C4_NO_LOCK_CHECK=1 for a run that times nothing)`);
  process.exit(2);
}
