CC ?= cc
CFLAGS = -O2 -g -Wall -Wextra -Iinclude

.PHONY: test arm clean

test: build/host_sim
	python3 test/validate_manifests.py
	./build/host_sim

build/host_sim: src/smack_core.c src/smack_core.h test/host_sim.c
	@mkdir -p build
	$(CC) $(CFLAGS) src/smack_core.c test/host_sim.c -o $@ -lm

arm:
	./scripts/build.sh

clean:
	rm -rf build
