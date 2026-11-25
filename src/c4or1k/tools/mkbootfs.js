#!/usr/bin/env node
// mkbootfs.js -- offline converter, basefs.json -> bootfs.idx + bootfs.blob.
//
// jor1k builds its 9p root filesystem's inode tree at runtime from
// basefs.json (jorconsole/jor1k/js/worker/filesystem/fsloader.js's
// HandleDirContents), fetching each file's real bytes over the
// network as it goes. c4or1k has no JSON parser and no network --
// this tool does that walk once, offline, in Node, and writes two
// files bootfs.c loads directly at startup with plain read() calls:
//
//   bootfs.idx   4-byte inode count, then N fixed 64-byte records:
//                name[32] (utf8, NUL-padded), mode/uid/gid/parentid/
//                firstid/nextid/size/bloboff (8 x int32, all LE).
//                parentid/firstid/nextid are -1 when absent.
//   bootfs.blob  every file's content and every symlink's target
//                string, concatenated; bloboff/size above slice into it.
//
// Inode 0 is always the root directory, matching fsloader's
// `this.CreateDirectory("", -1)` (in this tool it's just index 0 with
// parentid -1). firstid/nextid replicate FS.prototype.PushInode's
// singly-linked, newest-first child list exactly (each new child is
// *prepended*: `inode.nextid = parent.firstid; parent.firstid = idx`)
// -- not because c4or1k's directory-listing order needs to match
// jor1k's byte for byte, but because it's the simplest faithful port
// and there's no reason to invent a different order.
//
// Compressed files (basefs.json's "c":1, currently just bin/busybox)
// are decompressed here via the host's bunzip2, not jor1k's own
// bzip2.js -- this is a Node build tool with a real filesystem and
// shell, not the emulator itself, so reaching for the standard
// system tool is simpler and more robust than re-hosting jor1k's JS
// decompressor in a script that never runs inside c4m.
//
// MULTIPLE MANIFESTS / MERGE: jor1k boots by loading basefs.json, then
// overlaying the much larger fs.json (jorconsole loads the second over
// the first -- see filesystem.js's OnLoaded). Both are walked into one
// shared inode tree, and a directory that already exists is *merged
// into*, not duplicated. This tool replicates that: pass any number of
// (manifest, src-dir) pairs and they are processed in order into one
// combined idx/blob, using fsloader.js's HandleDirContents merge rule
// (a re-seen directory reuses the existing inode; a re-seen file is
// prepended and shadows the earlier one, exactly as jor1k's newest-
// first child list resolves it). This is how the extended filesystem
// reaches c4or1k -- everything resident up front, since there is no
// network to lazy-load over (see bootfs.h).
//
// Usage: node mkbootfs.js out.idx out.blob manifest1.json srcdir1 \
//                                          [manifest2.json srcdir2 ...]

"use strict";

const fs = require("fs");
const path = require("path");
const { execFileSync } = require("child_process");

const S_IFLNK = 0xA000;
const S_IFREG = 0x8000;
const S_IFDIR = 0x4000;
const S_IRWXUGO = 0x1FF;

const NAME_LEN = 64;   // must match bootfs.h's BOOTFS_NAME_LEN. Raised from
                       // 32 for the extended fs, whose longest name is 54.
const RECORD_LEN = NAME_LEN + 32; // name[NAME_LEN] + 8 x int32

// Usage: mkbootfs.js OUT_IDX OUT_BLOB (MANIFEST SRCDIR)...
const args = process.argv.slice(2);
if (args.length < 4 || (args.length % 2) !== 0) {
    console.error("usage: mkbootfs.js out.idx out.blob (manifest.json src-dir)...");
    process.exit(1);
}
const outIdxPath = args[0];
const outBlobPath = args[1];
const manifestPairs = []; // [{jsonPath, srcDir}, ...] processed in order
for (let i = 2; i < args.length; i += 2) manifestPairs.push({ jsonPath: args[i], srcDir: args[i + 1] });

// inode[i] = { name, mode, uid, gid, parentid, firstid, nextid, size, data }
// data is a Buffer (file content or symlink target bytes) or null (dir).
const inodes = [];

function pushInode(inode) {
    const idx = inodes.length;
    inodes.push(inode);
    if (inode.parentid != -1) {
        inode.nextid = inodes[inode.parentid].firstid;
        inodes[inode.parentid].firstid = idx;
    }
    return idx;
}

function fullPath(idx) {
    let p = "";
    while (idx != 0) {
        p = "/" + inodes[idx].name + p;
        idx = inodes[idx].parentid;
    }
    return p.substring(1);
}

// Mirrors FS.prototype.Search: the newest child of parentid whose name
// matches (children are prepended, so a linear scan newest-first would
// do; parentid+name equality over the flat array is equivalent and
// simpler here). -1 if absent.
function search(parentid, name) {
    for (let i = inodes.length - 1; i >= 0; --i) {
        if (inodes[i].parentid === parentid && inodes[i].name === name) return i;
    }
    return -1;
}

function loadFileData(tag, idx, srcDir) {
    const srcRel = tag.src ? tag.src : fullPath(idx);
    const srcPath = path.join(srcDir, srcRel);
    if (tag.c) {
        const data = execFileSync("bunzip2", ["-c", srcPath + ".bz2"], { maxBuffer: 1 << 28 });
        if (data.length != tag.size) {
            console.error(`mkbootfs: warning: ${srcRel} decompressed to ${data.length} bytes, basefs.json says ${tag.size}`);
        }
        return data;
    }
    const data = fs.readFileSync(srcPath);
    if (data.length != tag.size) {
        console.error(`mkbootfs: warning: ${srcRel} is ${data.length} bytes, basefs.json says ${tag.size}`);
    }
    return data;
}

function handleDirContents(list, parentid, srcDir) {
    for (const tag of list) {
        // fsloader.js's merge rule: if a same-named entry already exists
        // under parentid AND this tag is a directory (no symlink path,
        // no file size), recurse into the existing inode instead of
        // creating a duplicate. This is what lets fs.json overlay
        // basefs.json -- their shared dirs (/usr, /etc, /root, ...) fuse
        // into one tree. A re-seen file/symlink is NOT merged: it's
        // created and, being newer, shadows the earlier one on lookup
        // (bootfs_search returns the newest match), exactly as jor1k's
        // newest-first child list resolves it.
        const existing = search(parentid, tag.name);
        if (existing !== -1 && !tag.path && (typeof tag.size === "undefined")) {
            if (tag.child) handleDirContents(tag.child, existing, srcDir);
            continue;
        }

        const idx = pushInode({
            name: tag.name,
            mode: 0,
            uid: tag.uid | 0,
            gid: tag.gid | 0,
            parentid: parentid,
            firstid: -1,
            nextid: -1,
            size: 0,
            data: null,
        });
        const inode = inodes[idx];

        if (tag.path) { // symlink
            inode.mode = S_IFLNK | S_IRWXUGO;
            inode.data = Buffer.from(tag.path, "utf8");
            inode.size = inode.data.length;
        } else if (typeof tag.size === "undefined") { // directory
            inode.mode = parseInt(tag.mode, 8) | S_IFDIR;
            if (tag.child) handleDirContents(tag.child, idx, srcDir);
        } else { // regular file
            inode.mode = parseInt(tag.mode, 8) | S_IFREG;
            inode.data = loadFileData(tag, idx, srcDir);
            inode.size = inode.data.length;
        }
    }
}

// Root directory, matching fsloader's this.fs.CreateDirectory("", -1).
pushInode({ name: "", mode: S_IRWXUGO | S_IFDIR, uid: 0, gid: 0, parentid: -1, firstid: -1, nextid: -1, size: 0, data: null });
// Each manifest is walked into the shared tree in order, so a later
// manifest (fs.json) overlays an earlier one (basefs.json).
for (const { jsonPath, srcDir } of manifestPairs) {
    const manifest = JSON.parse(fs.readFileSync(jsonPath, "utf8"));
    handleDirContents(manifest.fs, 0, srcDir);
}

// Lay out the blob and assign bloboff to every inode with data.
const chunks = [];
let blobLen = 0;
for (const inode of inodes) {
    if (inode.data) {
        inode.bloboff = blobLen;
        chunks.push(inode.data);
        blobLen += inode.data.length;
    } else {
        inode.bloboff = 0;
    }
}
const blob = Buffer.concat(chunks, blobLen);

// Write bootfs.idx: 4-byte count, then RECORD_LEN-byte records.
const idx = Buffer.alloc(4 + inodes.length * RECORD_LEN);
idx.writeInt32LE(inodes.length, 0);
inodes.forEach((inode, i) => {
    const off = 4 + i * RECORD_LEN;
    const nameBytes = Buffer.from(inode.name, "utf8");
    if (nameBytes.length >= NAME_LEN) {
        console.error(`mkbootfs: name too long for NAME_LEN=${NAME_LEN}: ${inode.name}`);
        process.exit(1);
    }
    nameBytes.copy(idx, off);
    idx.writeInt32LE(inode.mode, off + NAME_LEN);
    idx.writeInt32LE(inode.uid, off + NAME_LEN + 4);
    idx.writeInt32LE(inode.gid, off + NAME_LEN + 8);
    idx.writeInt32LE(inode.parentid, off + NAME_LEN + 12);
    idx.writeInt32LE(inode.firstid, off + NAME_LEN + 16);
    idx.writeInt32LE(inode.nextid, off + NAME_LEN + 20);
    idx.writeInt32LE(inode.size, off + NAME_LEN + 24);
    idx.writeInt32LE(inode.bloboff, off + NAME_LEN + 28);
});

fs.writeFileSync(outIdxPath, idx);
fs.writeFileSync(outBlobPath, blob);
console.error(`mkbootfs: wrote ${inodes.length} inodes, ${blob.length} blob bytes`);
