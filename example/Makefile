CC       = mpicc
CFLAGS   = -O2 -fopenmp -Wall
LDFLAGS  =

# Build with profiling enabled
profile: CFLAGS  += -DPROFILE_WSS -I..
profile: LDFLAGS += -lpapi
profile: bench

# Build without profiling (macros compile away)
bench: bench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Run (2 ranks); capture WSS output from stderr
run: bench
	mpirun -np 2 ./bench 2>&1

clean:
	rm -f bench

.PHONY: profile run clean
