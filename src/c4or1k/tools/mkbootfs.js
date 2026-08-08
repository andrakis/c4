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
// Usage: node mkbootfs.js basefs.json basefs-src-dir out.idx out.blob

"use strict";

const fs = require("fs");
const path = require("path");
const { execFileSync } = require("child_process");

const S_IFLNK = 0xA000;
const S_IFREG = 0x8000;
const S_IFDIR = 0x4000;
const S_IRWXUGO = 0x1FF;

const NAME_LEN = 32;
const RECORD_LEN = 64; // name[32] + 8 x int32

if (process.argv.length < 6) {
    console.error("usage: mkbootfs.js basefs.json basefs-src-dir out.idx out.blob");
    process.exit(1);
}
const [, , basefsJsonPath, basefsSrcDir, outIdxPath, outBlobPath] = process.argv;

const manifest = JSON.parse(fs.readFileSync(basefsJsonPath, "utf8"));

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

function loadFileData(tag, idx) {
    const srcRel = tag.src ? tag.src : fullPath(idx);
    const srcPath = path.join(basefsSrcDir, srcRel);
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

function handleDirContents(list, parentid) {
    for (const tag of list) {
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
            if (tag.child) handleDirContents(tag.child, idx);
        } else { // regular file
            inode.mode = parseInt(tag.mode, 8) | S_IFREG;
            inode.data = loadFileData(tag, idx);
            inode.size = inode.data.length;
        }
    }
}

// Root directory, matching fsloader's this.fs.CreateDirectory("", -1).
pushInode({ name: "", mode: S_IRWXUGO | S_IFDIR, uid: 0, gid: 0, parentid: -1, firstid: -1, nextid: -1, size: 0, data: null });
handleDirContents(manifest.fs, 0);

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
    idx.writeInt32LE(inode.mode, off + 32);
    idx.writeInt32LE(inode.uid, off + 36);
    idx.writeInt32LE(inode.gid, off + 40);
    idx.writeInt32LE(inode.parentid, off + 44);
    idx.writeInt32LE(inode.firstid, off + 48);
    idx.writeInt32LE(inode.nextid, off + 52);
    idx.writeInt32LE(inode.size, off + 56);
    idx.writeInt32LE(inode.bloboff, off + 60);
});

fs.writeFileSync(outIdxPath, idx);
fs.writeFileSync(outBlobPath, blob);
console.error(`mkbootfs: wrote ${inodes.length} inodes, ${blob.length} blob bytes`);
