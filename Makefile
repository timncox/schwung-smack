CC ?= cc
CFLAGS = -O2 -g -Wall -Wextra -Iinclude

.PHONY: test test-ring arm clean

test: build/host_sim build/ring_stall
	python3 test/validate_manifests.py
	./build/host_sim
	./build/ring_stall

test-ring: build/ring_stall
	./build/ring_stall

build/host_sim: src/smack_core.c src/smack_core.h test/host_sim.c
	@mkdir -p build
	$(CC) $(CFLAGS) src/smack_core.c test/host_sim.c -o $@ -lm

build/ring_stall: src/smack_core.c src/smack_core.h test/ring_stall.c
	@mkdir -p build
	$(CC) $(CFLAGS) src/smack_core.c test/ring_stall.c -o $@ -lm

arm:
	./scripts/build.sh

clean:
	rm -rf build
