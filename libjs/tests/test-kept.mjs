// test-kept.mjs - the page's Uploads folders and Saved Files, without a
// machine: uploads.js lays a folder's uploads on drive 0 for each system
// to find, and kept.js lists a store and archives it as the page does.
// (The machines' side, with C4IX: test-desktop.mjs; the games' lookups:
// their own tests and READMEs.)
//
//   node libjs/tests/test-kept.mjs

import { execFileSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, readFileSync, rmSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { layUploads, FOLDERS, uploadName, uploadParts, NEEDS, needsStatus, HomeFiles, uploadsReadme } from '../uploads.js';
import { untar, tar, keptFiles, keptFolders, folderTar } from '../kept.js';

let fail = 0;
const check = (cond, what, extra = '') => {
  console.log(`test-kept: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(0, 1500) : ''}`);
  if (!cond) fail = 1;
};
const t = s => new TextEncoder().encode(s), d = b => new TextDecoder().decode(b);
const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

// ---- the Uploads' folders ----
check(FOLDERS[0][0] === '' && ['wolf3d', 'duke3d', 'quake', 'doom', 'rott'].every(f => FOLDERS.some(([k]) => k === f)),
  'the folders: /home, and one each for wolf3d, duke3d, quake, doom and rott');
check(uploadName('doom', 'DOOM2.WAD') === 'doom/doom2.wad' && uploadName('', 'X.WL6') === 'x.wl6' && uploadName('nope', 'a') === 'a',
  'an upload\'s name: its folder\'s (one of them only) and the file\'s, lower case');
check(uploadParts('doom/doom2.wad').join('|') === 'doom|doom2.wad' && uploadParts('a.txt').join('|') === '|a.txt', 'and back');
// what each game wants, for the Uploads' list: in its folder or in /home
{
  check(NEEDS.every(n => FOLDERS.some(([f]) => f === n.folder)) && NEEDS.length === FOLDERS.length - 1,
    "the Uploads' list says what each game of a folder wants");
  const st = Object.fromEntries(needsStatus(['duke3d/duke3d.grp', 'pak1.pak', 'doom/tnt.wad', 'rott/darkwar.wad', 'wolf3d/vswap.wl6'])
    .map(s => [s.need.folder, s]));
  check(st.duke3d.ready && st.quake.ready && st.doom.ready, 'a game with its files, in its folder or in /home, is ready (Duke, Quake, Doom)');
  check(!st.rott.ready && st.rott.missing.join() === 'darkwar.rtl,darkwar.rtc' && !st.wolf3d.ready && st.wolf3d.missing.length === 7,
    "one without them says which are still to add (ROTT's .RTL and .RTC, Wolfenstein's seven)", JSON.stringify([st.rott.missing, st.wolf3d.missing]));
  check(!needsStatus([]).find(s => s.need.folder === 'doom').ready, 'Doom wants one of its IWADs');
}

// under C4DOS the uploads are on B: (HomeFiles over drive 1's files), not on A:
{
  const disk = new Map([['c4dos.dir', t('GAMES/\nautoexec.bat\n')]]);
  layUploads(disk, [['duke3d/duke3d.grp', t('grp')], ['x.wad', t('w')]], true);
  check(!disk.has('duke3d/duke3d.grp') && !disk.has('x.wad') && d(disk.get('c4dos.dir')) === 'GAMES/\nautoexec.bat\n',
    'C4DOS: A: keeps only the system (no A:\\DUKE3D, its listing as it was)', JSON.stringify([...disk.keys()]));
  const base = new Map([['c4dos.dir', t('NOTE.TXT\n')], ['NOTE.TXT', t('mine')], ['x.wad', t('my own')]]);
  const kept = [];
  const b = new HomeFiles(base, [['duke3d/duke3d.grp', t('grp')], ['x.wad', t('w')]], (n, v) => kept.push([n, d(v)]));
  check(d(b.get('DUKE3D/DUKE3D.GRP')) === 'grp' && d(b.get('duke3d/duke3d.grp')) === 'grp' && b.has('Duke3D/Duke3D.grp'),
    'B:\\DUKE3D\\DUKE3D.GRP is the upload, found in any case');
  check(d(b.get('x.wad')) === 'w' && d(base.get('x.wad')) === 'my own', "an upload hides the person's own file of its name, which stays theirs");
  check(d(b.get('c4dos.dir')) === 'NOTE.TXT\nduke3d/\nx.wad\n' && d(b.get('duke3d/c4dos.dir')) === 'duke3d.grp\n',
    "B:'s listings are the person's with the uploads added: the root's, and the folder's", d(b.get('c4dos.dir')));
  check(b.locked('DUKE3D/duke3d.grp') && !b.delete('duke3d/duke3d.grp') && b.has('duke3d/duke3d.grp') && !b.locked('NOTE.TXT'),
    'an upload cannot be deleted from the machine (DEL refuses it); a file of the person\'s can');
  b.set('DUKE3D/c4dos.dir', t('duke3d.grp\nduke3d.cfg\n'));
  b.set('DUKE3D/duke3d.cfg', t('cfg'));
  check(d(base.get('DUKE3D/c4dos.dir')) === 'duke3d.cfg\n' && d(base.get('DUKE3D/duke3d.cfg')) === 'cfg' && !base.has('duke3d/duke3d.grp'),
    "a listing C4DOS writes is kept without the uploads, a file it saves is kept, and no upload is kept", JSON.stringify([...base.keys()]));
  check(d(base.get('c4dos.dir')) === 'NOTE.TXT\nDUKE3D/\n' && kept.length === 1 && kept[0][0] === 'c4dos.dir',
    "... and a folder saved into is in the root's kept listing too, which the page is told of", JSON.stringify([d(base.get('c4dos.dir')), kept]));
  check(d(b.kept('c4dos.dir', t('NOTE.TXT\nduke3d/\nDUKE3D/\nx.wad\n'))) === 'NOTE.TXT\nduke3d/\nDUKE3D/\nx.wad\n',
    "... keeping a folder or a file of the person's own of an upload's name");
  const forgot = new HomeFiles(base, [], null);
  check(d(forgot.get('c4dos.dir')) === 'NOTE.TXT\nDUKE3D/\n' && !forgot.has('duke3d/duke3d.grp') && d(forgot.get('DUKE3D/c4dos.dir')) === 'duke3d.cfg\n',
    'the upload forgotten: B: has what the person saved, and nothing of the upload');
}

// A:\GAMES\README.TXT, from NEEDS: each game's files, where, and how to start it
{
  const r = uploadsReadme('B'), a = uploadsReadme('A');
  check(NEEDS.every(n => [...(n.all || []), ...(n.any || [])].every(f => r.includes(f.toUpperCase())) && r.includes(n.game) && r.includes('B:' + n.dos.slice(2))),
    "README.TXT names every game's files, its folder on B: and how to start it", r);
  check(r.includes('Click Uploads') && r.split('\r\n').every(l => l.length <= 70) && /\r\n$/.test(r),
    "... says how to upload them, in lines Notepad's window holds, with DOS's line ends");
  check(a.includes('A:\\DUKE3D') && !a.includes('B:\\'), "... and the Build Ladder's says A:, where its C4DOS has them");
}

{
  const files = new Map([['c4dos.dir', t('BIN/\nGAMES/\nDOOM/\nautoexec.bat\n')], ['c4ix.sizes', t('5 x\n')]]);
  layUploads(files, [['vswap.wl6', t('a')], ['wolf3d/vswap.wl6', t('bb')], ['doom/doom2.wad', t('ccc')]]);
  check(d(files.get('wolf3d/vswap.wl6')) === 'bb' && d(files.get('vswap.wl6')) === 'a',
    'drive 0 has each upload by its name, a folder\'s with the folder (the bare games open "wolf3d/vswap.wl6")');
  check(d(files.get('c4dos.dir')) === 'BIN/\nGAMES/\nDOOM/\nautoexec.bat\nvswap.wl6\nwolf3d/\n',
    'C4DOS: A:\\ lists the /home upload and the new folder, not the files in folders', d(files.get('c4dos.dir')));
  check(d(files.get('wolf3d/c4dos.dir')) === 'vswap.wl6\n', 'and the folder has a listing of its own', d(files.get('wolf3d/c4dos.dir') || t('')));
  check(d(files.get('DOOM/doom2.wad')) === 'ccc' && d(files.get('DOOM/c4dos.dir')) === 'doom2.wad\n',
    'a folder the disk has already (DOOM/) gets its files in its own spelling, and its listing');
  check(d(files.get('c4ix.sizes')) === '5 x\n1 home/vswap.wl6\n2 home/wolf3d/vswap.wl6\n3 home/doom/doom2.wad\n' &&
        d(files.get('home/wolf3d/vswap.wl6')) === 'bb',
    'C4IX: each is home/NAME too, with a line in c4ix.sizes (vfsload makes /home/wolf3d for it)', d(files.get('c4ix.sizes')));
}

// ---- tar and untar, against GNU tar both ways ----
// (tar is run in the folder, with names relative to it: on Windows a
// path node makes is C:\..., which GNU tar takes for a remote host)
const dir = mkdtempSync(join(tmpdir(), 'test-kept-'));
try {
  const big = new Uint8Array(70000).map((_, i) => (i * 7) & 255);
  const long = 'a'.repeat(60) + '/' + 'b'.repeat(60) + '/' + 'c'.repeat(50) + '.txt';
  const archive = tar([['wolf3d/savegam0.wl6', t('save 0\n')], ['wolf3d/sub/big.bin', big], [long, t('long name\n')]]);
  writeFileSync(join(dir, 'a.tar'), archive);
  const listed = execFileSync('tar', ['tf', 'a.tar'], { cwd: dir, encoding: 'utf8' }).trim().split('\n');
  check(['wolf3d/', 'wolf3d/savegam0.wl6', 'wolf3d/sub/', 'wolf3d/sub/big.bin', long].every(n => listed.includes(n)),
    'tar: GNU tar lists its files and their folders, a name over 100 bytes too', listed.join(' '));
  mkdirSync(join(dir, 'x'));
  execFileSync('tar', ['xf', 'a.tar', '-C', 'x'], { cwd: dir });
  check(same(new Uint8Array(readFileSync(join(dir, 'x/wolf3d/sub/big.bin'))), big) &&
        readFileSync(join(dir, 'x', long), 'utf8') === 'long name\n', 'and takes out the same bytes');
  const back = untar(archive);
  check(back.files.length === 3 && same(back.files[1][1], big) && back.files[2][0] === long && back.dirs.includes('wolf3d/sub'),
    'untar reads it back, names, bytes and folders');
  writeFileSync(join(dir, 'x/plain.txt'), 'gnu\n');
  execFileSync('tar', ['--format=ustar', '-cf', 'g.tar', '-C', 'x', 'plain.txt', 'wolf3d'], { cwd: dir });
  const g = untar(new Uint8Array(readFileSync(join(dir, 'g.tar'))));
  check(g.files.some(([n, b]) => n === 'plain.txt' && d(b) === 'gnu\n') && g.files.some(([n, b]) => n === 'wolf3d/sub/big.bin' && same(b, big)),
    'and reads what GNU tar writes (ustar, as C4IX\'s persist writes)');
} finally { rmSync(dir, { recursive: true, force: true }); }

// ---- a store, as the page lists it ----
{
  // a bare game's: files by name; C4DOS's listings left out
  const bare = keptFiles(new Map([['savegam0.wl6', t('s')], ['config.wl6', t('c')], ['DOOM/c4dos.dir', t('x')], ['c4dos.dir', t('y')], ['DOOM/doomsav0.dsg', t('d')]]));
  check(bare.map(([n]) => n).join(' ') === 'DOOM/doomsav0.dsg config.wl6 savegam0.wl6', 'a bare game\'s or C4DOS\'s store: its files by name, not c4dos.dir',
    bare.map(([n]) => n).join(' '));
  const folders = keptFolders(bare).map(([f, fs]) => `${f || '(top)'}:${fs.length}`).join(' ');
  check(folders === '(top):3 DOOM:1', 'its folders, each with all under it', folders);
  // C4IX's: the current tar's files, not persist's own
  const old = tar([['usr/games/wolf3d/savegam0.wl6', t('old')]]);
  const now = tar([['usr/games/wolf3d/savegam0.wl6', t('new')], ['home/notes.txt', t('n')], ['var/persist/whiteouts', t('w')]]);
  const c4ix = new Map([['c4ix-base.tar', tar([['bin/x', t('x')]])], ['c4ix-base.id', t('1')], ['c4ix-persist.tar', old],
                        ['c4ix-persist-b.tar', now], ['c4ix-persist.cur', t('b\n')]]);
  const kept = keptFiles(c4ix);
  check(kept.map(([n, b]) => `${n}=${d(b)}`).join(' ') === '/home/notes.txt=n /usr/games/wolf3d/savegam0.wl6=new',
    'C4IX\'s store: the files of the tar c4ix-persist.cur names, by their paths, not the system\'s or persist\'s own',
    kept.map(([n]) => n).join(' '));
  c4ix.set('c4ix-persist.cur', t('a'));
  check(d(keptFiles(c4ix).find(([n]) => n.endsWith('.wl6'))[1]) === 'old', 'and the other tar when it names that one');
  const ft = folderTar(kept, '/usr/games/wolf3d', 'c4ix');
  const inside = untar(ft.bytes);
  check(ft.name === 'wolf3d.tar' && inside.files.map(([n]) => n).join(' ') === 'wolf3d/savegam0.wl6',
    'a folder downloads as NAME.tar, its files under NAME/', ft.name + ' ' + inside.files.map(([n]) => n).join(' '));
  const all = folderTar(kept, '', 'c4ix');
  check(all.name === 'c4ix.tar' && untar(all.bytes).files.map(([n]) => n).join(' ') === 'c4ix/home/notes.txt c4ix/usr/games/wolf3d/savegam0.wl6',
    'and all of it as the store\'s NAME.tar', untar(all.bytes).files.map(([n]) => n).join(' '));
}

console.log(fail ? 'test-kept: FAILED' : 'test-kept: OK');
process.exit(fail);
