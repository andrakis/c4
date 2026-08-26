@ECHO OFF
ECHO ----------------------------------------------
ECHO  c4fc -- the C compiler, running in the machine
ECHO ----------------------------------------------
ECHO Loading the compiler (about 19s on c4bb)...
RUN c4th.c4r core.f ext.f locals.f dos.f dsl.f lex.f pp.f ast.f types.f emit.f tree.f gen.f parse.f opt.f c4fc.f cc.f
