CC          = mpicc
CFLAGS      = -O2 -fopenmp -Wall
LDFLAGS     =
EXTRA_CFLAGS ?=

# Build with profiling enabled.
# wss_profiler.h is installed at /usr/local/include inside the container,
# so no -I flag is needed.
# Pass extra compiler defines with: make profile EXTRA_CFLAGS="-DFOO=bar"
profile: CFLAGS  += -DPROFILE_WSS -g -fno-omit-frame-pointer $(EXTRA_CFLAGS)
profile: LDFLAGS += -lwss_profiler -lpapi
profile: bench

# Build without profiling (macros compile away to nothing)
bench: bench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run: bench
	mpirun -np 4 ./bench 2>&1

clean:
	rm -f bench

.PHONY: profile run clean
