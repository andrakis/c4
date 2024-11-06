C4 under JavaScript
===================

An incomplete port of C4 to JavaScript. It is very slow running under 64bit mode, but runs at a decent speed under 32bit mode.

However, `C4R` files are architecture-specific, requiring 32bit cross-compilation for decent speed.

This interpreter requires files assembled by `C4CC` with the `asm-js` component:

	./c4 c4m.c c4cc.c asm-js.c -- -s hello.c | node -
	./c4 c4m.c c4cc.c asm-js.c -- -s factorial.c > factorial.js && node factorial.js

Requires a node module for the JavaScript emulator to work, ensure you run:

	npm install

Note: The 64bit version is fairly slow. If you compile c4 for 32bit, the resulting Node application is also much faster, as it uses Int32 rather than BigNum for its word type.

