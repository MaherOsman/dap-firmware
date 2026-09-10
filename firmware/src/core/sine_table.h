#ifndef SINE_TABLE_H
#define SINE_TABLE_H
#include <stdint.h>
#define SINE_TABLE_LEN 100  // frames (L+R pairs); 441 Hz @ 44.1kHz
extern const int32_t sine_table[SINE_TABLE_LEN * 2];
#endif
