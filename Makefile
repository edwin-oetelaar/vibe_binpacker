CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
LDLIBS ?=

.PHONY: all test sanitize clean

all: test_bp_bitmap

test_bp_bitmap: bp_bitmap.c bp_bitmap.h bp_contract.h test_bp_bitmap.c
	$(CC) $(CFLAGS) bp_bitmap.c test_bp_bitmap.c -o $@ $(LDLIBS)

test: all
	./test_bp_bitmap

# Sanitizers are mandatory for changes to indexing, shifting or masking.
sanitize:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=0 $(MAKE) \
		CFLAGS="$(CFLAGS) -g -fno-omit-frame-pointer \
			-fsanitize=address,undefined" \
		test

clean:
	rm -f test_bp_bitmap
