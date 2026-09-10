import math

N = 100
AMPLITUDE = (1 << 30) - 1  # leave headroom below full-scale int32

print("#include \"sine_table.h\"\n")
print(f"const int32_t sine_table[SINE_TABLE_LEN * 2] = {{")
for i in range(N):
    s = int(AMPLITUDE * math.sin(2 * math.pi * i / N))
    print(f"    {s}, {s},  // frame {i}: L, R")
print("};")
