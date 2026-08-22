CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
LDLIBS ?=

.PHONY: all test sanitize demo benchmark gui clean

CORE = bp_bitmap.c bp_mask.c bp_polygon.c bp_raster.c bp_place.c \
	bp_nester.c bp_penetration.c bp_separate.c bp_score.c \
	bp_search.c bp_svg.c bp_job.c

# The viewer is the only part of this project with a third-party dependency,
# and it is deliberately fenced off: `make`, `make test` and `make benchmark`
# never touch it. Run tools/fetch_gui_deps.sh first, then `make gui`.
CIMGUI_DIR = third_party/cimgui
IMGUI_DIR = $(CIMGUI_DIR)/imgui
GUI_CXXFLAGS = -std=c++11 -O2 -fno-exceptions -fno-rtti \
	-DIMGUI_USER_CONFIG='"../cimconfig.h"' -DIMGUI_IMPL_API='extern "C"' \
	-I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(CIMGUI_DIR)
GUI_CFLAGS = -std=c11 -O2 -Wall -Wextra -Werror -pedantic \
	-DCIMGUI_USE_GLFW -DCIMGUI_USE_OPENGL3 -I$(CIMGUI_DIR) -I.
GUI_SOURCES_CXX = $(CIMGUI_DIR)/cimgui.cpp $(CIMGUI_DIR)/cimgui_impl.cpp \
	$(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp \
	$(IMGUI_DIR)/imgui_demo.cpp \
	$(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp \
	$(IMGUI_DIR)/backends/imgui_impl_glfw.cpp \
	$(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp
GUI_LIBS = -lglfw -lGL -ldl -lpthread -lm
GUI_OBJ_DIR = gui_build
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

gui: bp_gui

# Objects go in their own directory: a failed build must not leave a stray
# .o in the tree for the next link to pick up.
bp_gui: $(CORE) $(CORE_HEADERS) gui/bp_gui.c
	@test -d $(IMGUI_DIR) || { \
		echo "third_party missing: run sh tools/fetch_gui_deps.sh"; \
		exit 1; }
	@rm -rf $(GUI_OBJ_DIR) && mkdir -p $(GUI_OBJ_DIR)
	@for source in $(GUI_SOURCES_CXX); do \
		echo "  CXX $$source"; \
		$(CXX) $(GUI_CXXFLAGS) -c $$source \
			-o $(GUI_OBJ_DIR)/`basename $$source .cpp`.o || exit 1; \
	done
	@for source in $(CORE) gui/bp_gui.c; do \
		echo "  CC  $$source"; \
		$(CC) $(GUI_CFLAGS) -c $$source \
			-o $(GUI_OBJ_DIR)/`basename $$source .c`.o || exit 1; \
	done
	$(CXX) $(GUI_OBJ_DIR)/*.o -o $@ $(GUI_LIBS)

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
	rm -rf $(GUI_OBJ_DIR)
	rm -f test_bp_bitmap test_bp_mask test_bp_polygon test_bp_raster \
		test_bp_place test_bp_nester test_bp_score test_bp_search \
		test_bp_penetration test_bp_separate \
	test_bp_penetration test_bp_separate \
		bp_pack bp_bench bp_gui demo.svg shapes.svg nested_hole.svg searched.svg squeezed.svg
