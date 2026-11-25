// drives.js -- what is in each drive, and how it gets there.
//
// Two halves, deliberately separated. DriveSet is ordinary logic with
// no DOM in it: which medium is in which drive, what happens when one
// is taken out, and what the machine's Devices layer should be handed.
// DrivePanel is the view, and does nothing but read that and draw it.
//
// The split is not tidiness. Ejecting a disk and restarting is the
// whole climb (docs/c4bb-storage.md M11/M12), so the rule that a soft
// reset must NOT put back a medium somebody took out is a rule worth
// testing -- and it is testable exactly because it does not live in a
// click handler. src/c4bb/tests/test-drives.mjs.

export const EMPTY = '';               // what a drive with nothing in it holds

export class DriveSet {
  // media: id -> { id, label, writable, files, sink }
  //   files is a Map name -> Uint8Array; sink is called with a written
  //   file, or null for a medium that cannot be written to.
  constructor (count = 3) {
    this.count = count;
    this.media = new Map();
    this.slots = new Array(count).fill(EMPTY);
    this.ejected = new Array(count).fill(false);
  }

  define (m) { this.media.set(m.id, m); return m; }
  forget (id) {
    this.media.delete(id);
    for (let d = 0; d < this.count; ++d) if (this.slots[d] === id) this.eject(d);
  }

  medium (drive) { return this.media.get(this.slots[drive]) || null; }

  // Putting a disk in. It also clears the ejected mark: the mark says
  // "somebody took the medium out of this drive", and the moment
  // something is in it again that is no longer the case.
  insert (drive, id) {
    if (drive < 0 || drive >= this.count) return false;
    if (id !== EMPTY && !this.media.has(id)) return false;
    // One medium, one drive. Two drives holding the same files would
    // be two views of one thing, and writing through both is a
    // corruption waiting to happen rather than a feature.
    if (id !== EMPTY) {
      for (let d = 0; d < this.count; ++d) if (d !== drive && this.slots[d] === id) this.slots[d] = EMPTY;
    }
    this.slots[drive] = id;
    this.ejected[drive] = false;
    return true;
  }

  eject (drive) {
    if (drive < 0 || drive >= this.count) return false;
    if (this.slots[drive] === EMPTY) return false;
    this.slots[drive] = EMPTY;
    this.ejected[drive] = true;
    return true;
  }

  // The machine ejected it from inside -- `reboot 0`, or a store to
  // DISK_EJECT. Same thing as the button, and it has to be, or the
  // panel and the machine disagree about what is in the drive.
  ejectedByGuest (drive) { return this.eject(drive); }

  // What Devices wants. A medium with no sink is read-only however it
  // is labelled: `writable` is a claim, a sink is the ability.
  toDevices () {
    const out = [];
    for (let d = 0; d < this.count; ++d) {
      const m = this.medium(d);
      out.push(m
        ? { files: m.files, writable: !!m.sink, sink: m.sink, id: m.id }
        : { files: new Map(), writable: false, sink: null, id: EMPTY });
    }
    return out;
  }

  // A soft reset rebuilds the machine, and the media have to come back
  // with it -- except the ones that were taken out. Restarting a
  // machine does not put a disk back in it, and the climb turns on
  // that: install a boot medium in drive 1, eject drive 0, reset, and
  // the BIOS has to find drive 1 rather than the disk it came from.
  // (cli.js:266 learned the same thing.)
  //
  // Ejected marks are cleared here: they have done their job for this
  // reset, and a drive left empty stays empty on its own.
  survivesReset () {
    for (let d = 0; d < this.count; ++d) this.ejected[d] = false;
    return this.toDevices();
  }

  // One line per drive, for whoever is drawing.
  summary (drive) {
    const m = this.medium(drive);
    if (!m) return { drive, label: null, empty: true, files: 0, bytes: 0, writable: false };
    let bytes = 0;
    for (const v of m.files.values()) bytes += v.length;
    return {
      drive, label: m.label, id: m.id, empty: false,
      files: m.files.size, bytes, writable: !!m.sink,
    };
  }
}

export function humanBytes (n) {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${Math.round(n / 1024)} KB`;
  return `${(n / 1024 / 1024).toFixed(1)} MB`;
}

// ---------------------------------------------------------------------
// The view. Constructed with a container element; `onchange` is called
// whenever the player moves a medium, so the app can rebuild the
// machine's drive list.

export class DrivePanel {
  constructor (el, set, handlers = {}) {
    this.el = el;
    this.set = set;
    this.h = handlers;
  }

  render (bootDrive) {
    if (!this.el) return;
    const s = this.set;
    this.el.textContent = '';
    for (let d = 0; d < s.count; ++d) {
      const row = document.createElement('div');
      row.className = 'drive' + (d === bootDrive ? ' booted' : '');

      const num = document.createElement('span');
      num.className = 'dnum';
      num.textContent = `${d}:`;
      row.appendChild(num);

      const sel = document.createElement('select');
      sel.className = 'dsel';
      const none = document.createElement('option');
      none.value = EMPTY;
      none.textContent = '— empty —';
      sel.appendChild(none);
      for (const m of s.media.values()) {
        const o = document.createElement('option');
        o.value = m.id;
        o.textContent = m.sink ? m.label : `${m.label} (read-only)`;
        sel.appendChild(o);
      }
      sel.value = s.slots[d];
      sel.onchange = () => {
        if (sel.value === EMPTY) s.eject(d); else s.insert(d, sel.value);
        this.h.onchange && this.h.onchange();
      };
      row.appendChild(sel);

      const info = document.createElement('span');
      info.className = 'dinfo';
      const q = s.summary(d);
      info.textContent = q.empty
        ? 'no medium'
        : `${q.files} file${q.files === 1 ? '' : 's'}, ${humanBytes(q.bytes)}${q.writable ? '' : ', RO'}`;
      row.appendChild(info);

      const out = document.createElement('button');
      out.textContent = 'Eject';
      out.disabled = q.empty;
      out.onclick = () => { s.eject(d); this.h.onchange && this.h.onchange(); };
      row.appendChild(out);

      this.el.appendChild(row);
    }
  }
}
