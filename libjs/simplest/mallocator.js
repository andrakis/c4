/*
 * Memory allocator for C4 JS
 *
 * In the simplest possible way
 */

'use strict';

const ENTRIES_MAX = 16;

/* struct */ function Entry (id, start, length, reallength, inuse) {
	this.id = id;
	this.start = start;
	this.length = length;
	this.reallength = reallength;
	this.inuse = inuse;
	this.magic = 0; // Filled out later
}

function Mallocator (startLocation, vm) {
	this.start = startLocation;
	this.entries = [/* struct Entry */];
	this.vm = vm;

	this.zero = vm.memory.convert(0);
	this.one  = vm.memory.convert(1);

	this.word = vm.word;

	this.magic = vm.memory.convert(0xBEAF);

	this.idcounter = 0;
}

Mallocator.prototype.IsValid = function(entry) {
	return this.vm.readword(entry.start) == entry.magic;
};
Mallocator.prototype._findFree = function(szUnaligned) {
	var nextStart = this.start;
	var sz = this.vm.memory.align(szUnaligned);
	this.entries.sort((a, b) => Number(a.start - b.start));
	for (var i = 0; i < this.entries.length; i++) {
		nextStart = this.entries[i].start + this.entries[i].reallength;
		if (this.entries[i].magic != this.vm.readword(this.entries[i].start)) {
			console.log(`Heap corruption (mis)detected on index ${i}, aborting. magic 0x${this.vm.readword(this.entries[i].start)} (addr ${this.entries[i].start}) not expected value 0x${this.entries[i].magic}`);
			console.log(this.entries);
			process.exit(1);
		}
		if (this.entries[i].inuse) continue;
		if (this.entries[i].reallength >= sz)
			return this.entries[i];
		// TODO: consolidation disabled
		continue;
		// Count up next free slots
		var freeBytes = this.zero, highest = false;
		var inuse = false;
		for (x = i; x < this.entries.length; x++) {
			if ((inuse = this.entries[x].inuse)) break; // Ran into an in use block
			freeBytes += this.entries[x].reallength;
			highest = x;
			if (freeBytes >= sz) break;       // No need to continue consolidating
		}
		if (!inuse && freeBytes >= sz /*&& highest > i*/) {
			if (highest !== false)
				// Consolidate from i to highest
				var removed = this.entries.splice(i + 1, highest - i);
			//console.log(`Merging entries from ${i + 1}, count ${highest - i}:`, removed);
			this.entries[i].reallength += freeBytes;
			this.entries[i].length = this.entries[i].reallength;
			this.entries[i].magic = this._calcEntryMagic(this.entries[i]);
			return this.entries[i];
		} else {
			// Skip the items we just checked
			i = highest;
		}
	}
	// None found, add new
	if (false && this.entries.length > ENTRIES_MAX) {
		printf("Memory exhausted!\n");
		return null;
	}
	nextStart = this.vm.memory.align(nextStart);
	var entry = new Entry(this.vm.memory.convert(++this.idcounter), nextStart, szUnaligned, sz, true);
	entry.magic = this._calcEntryMagic(entry);
	this.entries.push(entry);
	return entry;
};
Mallocator.prototype._calcEntryMagic = function(entry) {
	//return this.magic ^ ((entry.id + this.one) * entry.length) ^ (entry.inuse ? this.one : this.zero);
	return entry.start - this.magic ^ (((entry.id + this.one) * (entry.reallength ^ (entry.length * (this.one + this.one) % entry.reallength)) + (entry.length * (entry.inuse ? this.one : this.zero) * (this.one + this.one) * entry.id)) % entry.start);
}
Mallocator.prototype.malloc = function(sz) {
	//console.log("malloc(", sz, ")");
	//if (sz == this.zero) return this.zero;

	// Add space for magic word
	sz = sz + this.word;
	//console.log("malloc adjusted bytes = ", sz);
	var entry = this._findFree(sz);
	if (!entry) return this.zero;

	// Update (real length may remain unchanged)
	entry.length = sz;
	entry.inuse  = true;
	entry.magic  = this._calcEntryMagic(entry);

	// Resize vm memory if required
	this.vm.ensureMemoryAvailable(entry.start + entry.length);

	// Write magic word and return location after magic word
	this.vm.writeword(entry.start, entry.magic);
	// console.log(`malloc(..), wrote magic ${entry.magic} to ${entry.start}`);
	if (this.vm.readword(entry.start) != entry.magic) {
		console.log("malloc(..) !! writeback failed no memory");
	}
	// console.log("malloc(..), start=0x" + entry.start.toString(16), ", return=0x" + (entry.start + this.word).toString(16));
	return entry.start + this.word;
};
Mallocator.prototype._findEntry = function(addr) {
	addr = this.vm.memory.convert(addr);
	var addrMinusWord = addr - this.word;
	for (var i = 0; i < this.entries.length; i++) {
		if (addrMinusWord == this.entries[i].start)
			return this.entries[i];
	}
	//console.log("simples.tmallocator._findEntry(0x" + addr.toString(16) + " failed:", this.entries);
	return null;
};
Mallocator.prototype.free = function(addr) {
	// console.log("free(" + addr.toString() + ")");
	if (addr == this.zero) return;
	var entry = this._findEntry(addr);
	if (!entry) throw "Invalid free: 0x" + addr.toString(16);

	entry.inuse = false;
	entry.magic = this._calcEntryMagic(entry);
	this.vm.writeword(entry.start, entry.magic);
};
Mallocator.prototype.atexit = function() {
	var i = 0;
	while (i < this.entries.length) {
		if (!this.entries[i].inuse) {
			this.entries.splice(i, 1);
			continue;
		}
		++i;
	}
	return;
	if (this.entries.length > 0) {
		console.log(`Still ${this.entries.length} allocated blocks at exit:`, this.entries);
	} else {
		// console.log(`(simplest.mallocator) All blocks freed`);
	}
};

exports.Mallocator = Mallocator;
