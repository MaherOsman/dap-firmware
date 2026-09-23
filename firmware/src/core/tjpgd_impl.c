/*
 * The single translation unit that compiles TJpgDec. It lives under
 * third_party/ untouched; this file exists so CubeIDE (which only builds
 * firmware/src) and the host Makefile both pick it up.
 */
#include "../../third_party/tjpgd/tjpgd.c"
