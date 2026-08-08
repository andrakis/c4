// netlist.js - parser for hw/board.hwd (board layout description).
// Layout only: behavior comes from microcode.uc. See board.hwd header.

export function parseBoard(source) {
  const modules = [];
  const nets = [];
  const connects = [];

  for (let raw of source.split('\n')) {
    const line = raw.replace(/#.*/, '').trim();
    if (!line) continue;
    let m;
    if ((m = line.match(/^module\s+(\w+)\s+(.*)$/))) {
      const mod = { name: m[1], kind: 'reg', at: [0, 0], size: [120, 80], label: m[1], sigs: [], leds: 'hex' };
      for (const [, k, v] of m[2].matchAll(/(\w+)=("([^"]*)"|\S+)/g)) {
        const val = v.startsWith('"') ? v.slice(1, -1) : v;
        if (k === 'at' || k === 'size') mod[k] = val.split(',').map(Number);
        else if (k === 'sigs') mod.sigs = val ? val.split(',') : [];
        else mod[k] = val;
      }
      modules.push(mod);
    } else if ((m = line.match(/^net\s+(\w+)\s+(.*)$/))) {
      const net = { name: m[1], width: 1, route: [] };
      const rest = m[2];
      const wm = rest.match(/width=(\d+)/);
      if (wm) net.width = parseInt(wm[1], 10);
      const rm = rest.match(/route=(.*)$/);
      if (rm) net.route = rm[1].trim().split(/\s+/).map(p => p.split(',').map(Number));
      nets.push(net);
    } else if ((m = line.match(/^connect\s+(\w+)\s*->\s*(\w+)$/))) {
      connects.push({ from: m[1], to: m[2] });
    } else {
      throw new Error(`board.hwd: cannot parse: ${line}`);
    }
  }
  return { modules, nets, connects };
}
