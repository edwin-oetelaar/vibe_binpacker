CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
LDLIBS ?=

.PHONY: all test sanitize demo benchmark clean

CORE = bp_bitmap.c bp_mask.c bp_place.c bp_nester.c bp_svg.c bp_job.c
CORE_HEADERS = bp_bitmap.h bp_mask.h bp_place.h bp_nester.h \
	bp_svg.h bp_job.h bp_contract.h

all: test_bp_bitmap test_bp_mask test_bp_place test_bp_nester bp_pack bp_bench

test_bp_bitmap: $(CORE) $(CORE_HEADERS) test_bp_bitmap.c
	$(CC) $(CFLAGS) $(CORE) test_bp_bitmap.c -o $@ $(LDLIBS)

test_bp_mask: $(CORE) $(CORE_HEADERS) test_bp_mask.c
	$(CC) $(CFLAGS) $(CORE) test_bp_mask.c -o $@ $(LDLIBS)

test_bp_place: $(CORE) $(CORE_HEADERS) test_bp_place.c
	$(CC) $(CFLAGS) $(CORE) test_bp_place.c -o $@ $(LDLIBS)

test_bp_nester: $(CORE) $(CORE_HEADERS) test_bp_nester.c
	$(CC) $(CFLAGS) $(CORE) test_bp_nester.c -o $@ $(LDLIBS)

bp_pack: $(CORE) $(CORE_HEADERS) bp_pack.c
	$(CC) $(CFLAGS) $(CORE) bp_pack.c -o $@ $(LDLIBS) -lm

bp_bench: $(CORE) $(CORE_HEADERS) bp_bench.c
	$(CC) $(CFLAGS) $(CORE) bp_bench.c -o $@ $(LDLIBS) -lm

benchmark: bp_bench
	./bp_bench jobs/demo.job

# The Python check re-verifies the layout from the SVG alone, so it does not
# depend on the solver's own assertions being right.
demo: bp_pack
	./bp_pack --svg demo.svg jobs/demo.job
	python3 tools/check_layout.py demo.svg --clearance 1.0

test: all
	./test_bp_bitmap
	./test_bp_mask
	./test_bp_place
	./test_bp_nester

# Sanitizers are mandatory for changes to indexing, shifting or masking.
sanitize:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=0 $(MAKE) \
		CFLAGS="$(CFLAGS) -g -fno-omit-frame-pointer \
			-fsanitize=address,undefined" \
		test

clean:
	rm -f test_bp_bitmap test_bp_mask test_bp_place test_bp_nester \
		bp_pack bp_bench demo.svg
