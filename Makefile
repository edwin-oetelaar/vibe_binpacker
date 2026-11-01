# Layout: headers in include, the solver in src, programs in apps, tests in
# tests, the viewer in gui. Everything the build produces goes under build and
# nothing else does, so the source tree stays readable.

CC ?= cc
CXX ?= c++
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
CPPFLAGS ?= -Iinclude
LDLIBS ?= -lm

BUILD = build
OBJ = $(BUILD)/obj
GUI_OBJ = $(BUILD)/gui

# -MMD -MP writes a dependency file beside each object, so touching a header
# rebuilds exactly what included it and nothing more.
DEPFLAGS = -MMD -MP

CORE_SOURCES = $(sort $(wildcard src/*.c))
CORE_OBJECTS = $(patsubst src/%.c,$(OBJ)/%.o,$(CORE_SOURCES))

APP_SOURCES = $(sort $(wildcard apps/*.c))
APP_BINARIES = $(patsubst apps/%.c,$(BUILD)/%,$(APP_SOURCES))

TEST_SOURCES = $(sort $(wildcard tests/test_*.c))
TEST_BINARIES = $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SOURCES))

.PHONY: all test sanitize demo benchmark gui clean

all: $(APP_BINARIES) $(TEST_BINARIES)

$(BUILD) $(OBJ) $(GUI_OBJ):
	@mkdir -p $@

$(OBJ)/%.o: src/%.c | $(OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) -c $< -o $@

$(APP_BINARIES): $(BUILD)/%: apps/%.c $(CORE_OBJECTS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) $< $(CORE_OBJECTS) -o $@ $(LDLIBS)

$(TEST_BINARIES): $(BUILD)/%: tests/%.c $(CORE_OBJECTS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Itests $(DEPFLAGS) $< $(CORE_OBJECTS) \
		-o $@ $(LDLIBS)

# The cut list checker is Python and must stay independent of the C model,
# so its own tests run here too.
test: $(TEST_BINARIES)
	@for binary in $(TEST_BINARIES); do echo "  $$binary"; $$binary || exit 1; done
	@echo "  tools/test_check_cutplan.py"
	@python3 tools/test_check_cutplan.py -q

# Sanitizers are mandatory for changes to indexing, shifting or masking.
sanitize:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=0 $(MAKE) \
		CFLAGS="$(CFLAGS) -g -fno-omit-frame-pointer \
			-fsanitize=address,undefined" \
		test

benchmark: $(BUILD)/bp_bench
	$(BUILD)/bp_bench jobs/demo.job

# The Python check re-verifies each layout from the SVG alone, so it does not
# depend on the solver's own assertions being right. It is what caught the
# clearance being measured between cell centres instead of cell squares.
demo: $(BUILD)/bp_pack
	$(BUILD)/bp_pack --svg $(BUILD)/demo.svg jobs/demo.job
	python3 tools/check_layout.py $(BUILD)/demo.svg --clearance 1.0
	$(BUILD)/bp_pack --svg $(BUILD)/shapes.svg jobs/shapes.job
	python3 tools/check_layout.py $(BUILD)/shapes.svg --clearance 1.0
	$(BUILD)/bp_pack --svg $(BUILD)/nested_hole.svg jobs/nested_hole.job
	python3 tools/check_layout.py $(BUILD)/nested_hole.svg --clearance 1.0
	$(BUILD)/bp_pack --time 6 --svg $(BUILD)/searched.svg jobs/dense.job || true
	python3 tools/check_layout.py $(BUILD)/searched.svg --clearance 1.0
	$(BUILD)/bp_pack --time 6 --svg $(BUILD)/squeezed.svg jobs/shapes.job
	python3 tools/check_layout.py $(BUILD)/squeezed.svg --clearance 1.0

# The viewer is the only part of this project with a third-party dependency,
# and it is fenced off: all, test, demo and benchmark never touch it. Run
# tools/fetch_gui_deps.sh first, then `make gui`.
CIMGUI_DIR = third_party/cimgui
IMGUI_DIR = $(CIMGUI_DIR)/imgui
GUI_CXXFLAGS = -std=c++11 -O2 -fno-exceptions -fno-rtti \
	-DIMGUI_USER_CONFIG='"../cimconfig.h"' -DIMGUI_IMPL_API='extern "C"' \
	-I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(CIMGUI_DIR)
GUI_CPPFLAGS = -DCIMGUI_USE_GLFW -DCIMGUI_USE_OPENGL3 -I$(CIMGUI_DIR)
GUI_LIBS = -lglfw -lGL -ldl -lpthread -lm

vpath %.cpp $(CIMGUI_DIR) $(IMGUI_DIR) $(IMGUI_DIR)/backends
GUI_CXX_NAMES = cimgui cimgui_impl imgui imgui_draw imgui_demo imgui_tables \
	imgui_widgets imgui_impl_glfw imgui_impl_opengl3
GUI_CXX_OBJECTS = $(addprefix $(GUI_OBJ)/,$(addsuffix .o,$(GUI_CXX_NAMES)))

gui: $(BUILD)/bp_gui

$(GUI_OBJ)/%.o: %.cpp | $(GUI_OBJ)
	@test -d $(IMGUI_DIR) || { \
		echo "third_party missing: run sh tools/fetch_gui_deps.sh"; \
		exit 1; }
	$(CXX) $(GUI_CXXFLAGS) -c $< -o $@

$(GUI_OBJ)/bp_gui.o: gui/bp_gui.c | $(GUI_OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(GUI_CPPFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/bp_gui: $(GUI_OBJ)/bp_gui.o $(CORE_OBJECTS) $(GUI_CXX_OBJECTS)
	$(CXX) $^ -o $@ $(GUI_LIBS)

clean:
	rm -rf $(BUILD)

-include $(wildcard $(OBJ)/*.d) $(wildcard $(BUILD)/*.d) \
	$(wildcard $(GUI_OBJ)/*.d)
