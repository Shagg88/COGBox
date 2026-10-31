# xextool (ours, MIT)

Reads an Xbox 360 XEX2, extracts files from a LIVE/CON title-update package, applies a .xexp delta
patch, and writes the loaded image (a flat copy of guest memory from the load address). Written from the
published formats with Xenia's loader read as a reference; no code copied.

Build the LZX helper (libmspack's lzxd.c is used as a library, LGPL-2.1; Gears 3's no-pad change applied,
it made no difference for Judgment's TU4):

    gcc -O2 -shared -fPIC -I<libmspack>/mspack -DHAVE_STRING_H -DHAVE_STDLIB_H -DHAVE_LIMITS_H \
        -o libgjlzx.so lzx_helper.c <libmspack>/mspack/lzxd.c <libmspack>/mspack/system.c

Use:

    python3 xextool.py info  default.xex
    python3 xextool.py image default.xex base.bin
    python3 xextool.py stfs  <TU package> list
    python3 xextool.py stfs  <TU package> extract default.xexp default.xexp
    python3 xextool.py patch default.xex default.xexp tu4.bin --header-out tu4-header.bin

Output images are game data: keep them out of git.
