// uploads-readme.mjs - what grin's Full Games (Uploads) group's README.TXT
// says (A:\GAMES\README.TXT), from uploads.js's NEEDS, so that the two
// cannot part: page-disks.sh writes it onto the C4DOS disks at build time.
//
//   node libjs/uploads-readme.mjs [DRIVE] > GAMES/README.TXT
//
// DRIVE is where that disk's C4DOS has the uploads: B (the page's C4DOS,
// its home drive), or A (the Build Ladder's, which has no home drive).

import { uploadsReadme } from './uploads.js';

process.stdout.write(uploadsReadme(process.argv[2] || 'B'));
