# TJpgDec R0.03 — ChaN's Tiny JPEG Decompressor

Upstream: http://elm-chan.org/fsw/tjpgd/ (license in the header of `tjpgd.c`:
free for any use, keep the notice).

Taken from the copy in Bodmer/TJpg_Decoder, with that port's two small
Arduino additions reverted (a `swap` flag in `JDEC` and its use in the RGB565
output path), so these files match upstream R0.03. `tjpgdcnf.h` is ours —
see the comments in it for why each option is set the way it is.

Compiled through `src/core/tjpgd_impl.c`, following the same pattern as
dr_flac/dr_mp3: CubeIDE only builds `firmware/src`, and the host Makefile
builds vendored code with warnings relaxed rather than editing it.
