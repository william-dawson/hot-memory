CC      = mpicc
CFLAGS  = -O2 -fopenmp -Wall
LDFLAGS =

# Build with profiling enabled.
# wss_profiler.h is installed at /usr/local/include inside the container,
# so no -I flag is needed.
profile: CFLAGS  += -DPROFILE_WSS
profile: LDFLAGS += -lpapi
profile: bench

# Build without profiling (macros compile away to nothing)
bench: bench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run: bench
	mpirun --allow-run-as-root -np 2 ./bench 2>&1

clean:
	rm -f bench

.PHONY: profile run clean
