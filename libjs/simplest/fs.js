/*
 * File system for C4 JS
 *
 * In the simplest possible way
 */

'use strict';

var fs = require('fs');

var handles = {};
var handle_counter = 0;

function FileHandle (fd, pathname, mode) {
	this.fd = fd;
	this.pathname = pathname;
	// TODO: mode ignored
	this.mode     = mode;
	if (this.pathname)
		this.contents = fs.readFileSync(this.pathname, { encoding: 'binary', flag: 'r' });
	this.cursor   = 0;
}

exports.FileHandle = FileHandle;

function dothrow (e) { if (0) console.log(handles); throw e; }

exports.read = function(fd, bytes) {
	var handle   = handles[fd] || dothrow("Invalid file handle: " + fd.toString());
	var content  = handle.contents.substr(handle.cursor, bytes);
	handle.cursor += bytes;
	return content;
};

exports.fread = function(size, nmemb, fd) {
	var count  = size * nmemb;
	return exports.read(fd, count);
}

exports.fopen = function(pathname, mode) {
	var fd = handle_counter++;
	try {
		//console.log("fopen pathname:", pathname);
		handles[fd] = new FileHandle(fd, pathname, mode);
		//console.log("File open success");
	} catch (e) {
		//console.log("Error opening file:", e);
		--handle_counter;
		fd = -1;
	} finally {
		return fd;
	}
};

exports.open = exports.fopen; // Equivelent for now

exports.close = function(fd) {
	var handle   = handles[fd] || dothrow("Invalid file handle: " + fd.toString());
	delete handles[fd];
};
