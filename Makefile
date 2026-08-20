CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
LDLIBS ?=

.PHONY: all test sanitize demo benchmark clean

CORE = bp_bitmap.c bp_mask.c bp_polygon.c bp_raster.c bp_place.c \
	bp_nester.c bp_penetration.c bp_separate.c bp_score.c \
	bp_search.c bp_svg.c bp_job.c
CORE_HEADERS = bp_bitmap.h bp_mask.h bp_polygon.h bp_raster.h \
	bp_place.h bp_nester.h bp_penetration.h bp_separate.h \
	bp_score.h bp_search.h \
	bp_svg.h bp_job.h bp_contract.h

all: test_bp_bitmap test_bp_mask test_bp_polygon test_bp_raster \
	test_bp_place test_bp_nester test_bp_score test_bp_search \
	test_bp_penetration test_bp_separate \
	bp_pack bp_bench

test_bp_bitmap: $(CORE) $(CORE_HEADERS) test_bp_bitmap.c
	$(CC) $(CFLAGS) $(CORE) test_bp_bitmap.c -o $@ $(LDLIBS) -lm

test_bp_mask: $(CORE) $(CORE_HEADERS) test_bp_mask.c
	$(CC) $(CFLAGS) $(CORE) test_bp_mask.c -o $@ $(LDLIBS) -lm

test_bp_polygon: $(CORE) $(CORE_HEADERS) test_bp_polygon.c
	$(CC) $(CFLAGS) $(CORE) test_bp_polygon.c -o $@ $(LDLIBS) -lm

test_bp_raster: $(CORE) $(CORE_HEADERS) test_bp_raster.c
	$(CC) $(CFLAGS) $(CORE) test_bp_raster.c -o $@ $(LDLIBS) -lm

test_bp_place: $(CORE) $(CORE_HEADERS) test_bp_place.c
	$(CC) $(CFLAGS) $(CORE) test_bp_place.c -o $@ $(LDLIBS) -lm

test_bp_nester: $(CORE) $(CORE_HEADERS) test_bp_nester.c
	$(CC) $(CFLAGS) $(CORE) test_bp_nester.c -o $@ $(LDLIBS) -lm

test_bp_penetration: $(CORE) $(CORE_HEADERS) test_bp_penetration.c
	$(CC) $(CFLAGS) $(CORE) test_bp_penetration.c -o $@ $(LDLIBS) -lm

test_bp_separate: $(CORE) $(CORE_HEADERS) test_bp_separate.c
	$(CC) $(CFLAGS) $(CORE) test_bp_separate.c -o $@ $(LDLIBS) -lm

test_bp_score: $(CORE) $(CORE_HEADERS) test_bp_score.c
	$(CC) $(CFLAGS) $(CORE) test_bp_score.c -o $@ $(LDLIBS) -lm

test_bp_search: $(CORE) $(CORE_HEADERS) test_bp_search.c
	$(CC) $(CFLAGS) $(CORE) test_bp_search.c -o $@ $(LDLIBS) -lm

bp_pack: $(CORE) $(CORE_HEADERS) bp_pack.c
	$(CC) $(CFLAGS) $(CORE) bp_pack.c -o $@ $(LDLIBS) -lm

bp_bench: $(CORE) $(CORE_HEADERS) bp_bench.c
	$(CC) $(CFLAGS) $(CORE) bp_bench.c -o $@ $(LDLIBS) -lm

benchmark: bp_bench
	./bp_bench jobs/demo.job

# The Python check re-verifies each layout from the SVG alone, so it does not
# depend on the solver's own assertions being right. It is what caught the
# clearance being measured between cell centres instead of cell squares.
demo: bp_pack
	./bp_pack --svg demo.svg jobs/demo.job
	python3 tools/check_layout.py demo.svg --clearance 1.0
	./bp_pack --svg shapes.svg jobs/shapes.job
	python3 tools/check_layout.py shapes.svg --clearance 1.0
	./bp_pack --svg nested_hole.svg jobs/nested_hole.job
	python3 tools/check_layout.py nested_hole.svg --clearance 1.0
	./bp_pack --time 6 --svg searched.svg jobs/dense.job || true
	python3 tools/check_layout.py searched.svg --clearance 1.0
	./bp_pack --time 6 --svg squeezed.svg jobs/shapes.job
	python3 tools/check_layout.py squeezed.svg --clearance 1.0

test: all
	./test_bp_bitmap
	./test_bp_mask
	./test_bp_polygon
	./test_bp_raster
	./test_bp_place
	./test_bp_nester
	./test_bp_penetration
	./test_bp_separate
	./test_bp_score
	./test_bp_search

# Sanitizers are mandatory for changes to indexing, shifting or masking.
sanitize:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=0 $(MAKE) \
		CFLAGS="$(CFLAGS) -g -fno-omit-frame-pointer \
			-fsanitize=address,undefined" \
		test

clean:
	rm -f test_bp_bitmap test_bp_mask test_bp_polygon test_bp_raster \
		test_bp_place test_bp_nester test_bp_score test_bp_search \
		test_bp_penetration test_bp_separate \
	test_bp_penetration test_bp_separate \
		bp_pack bp_bench demo.svg shapes.svg nested_hole.svg searched.svg squeezed.svg
