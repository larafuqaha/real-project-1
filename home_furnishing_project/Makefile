# =====================================================================
# Makefile — Home Furnishing Competition (ENCS4330 Project #1)
# =====================================================================
# Targets:
#   make             — build with OpenGL + OpenMP
#   make headless    — build without OpenGL (no -lGL etc.)
#   make debug       — build with -O0 and asan-friendly flags
#   make run         — build and run with config/default.txt
#   make clean
# =====================================================================

CC      ?= gcc
SRCDIR   = src
BUILDDIR = build
BIN      = $(BUILDDIR)/furnishing

SRC = $(SRCDIR)/main.c \
      $(SRCDIR)/config.c \
      $(SRCDIR)/ipc.c \
      $(SRCDIR)/member.c \
      $(SRCDIR)/referee.c \
      $(SRCDIR)/gui.c

OBJ = $(SRC:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)

# Default flags
WARN     = -Wall -Wextra -Wno-unused-parameter
CFLAGS  ?= -O2 -g $(WARN) -fopenmp -DHAVE_GL
LDFLAGS ?= -fopenmp -lGL -lGLU -lglut -lm

# Detect if OpenGL/GLUT is missing — fall back to headless without erroring.
HAVE_GL := $(shell echo 'int main(){return 0;}' | $(CC) -x c - -lGL -lGLU -lglut -o /tmp/.glcheck 2>/dev/null && echo yes ; rm -f /tmp/.glcheck)

ifneq ($(HAVE_GL),yes)
  $(warning OpenGL/GLUT not found — building in headless mode. Install freeglut3-dev to enable the GUI.)
  CFLAGS  := -O2 -g $(WARN) -fopenmp
  LDFLAGS := -fopenmp -lm
endif

all: $(BIN)

$(BIN): $(OBJ) | $(BUILDDIR)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)
	@echo
	@echo "Built $(BIN). Run with:  ./$(BIN) config/default.txt"
	@echo

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Force a headless build (no OpenGL link), regardless of detection.
headless:
	$(MAKE) CFLAGS="-O2 -g $(WARN) -fopenmp" LDFLAGS="-fopenmp -lm"

# Debug build: no optimization, more checks.
debug:
	$(MAKE) CFLAGS="-O0 -g3 $(WARN) -fopenmp -DHAVE_GL -fno-omit-frame-pointer" \
	        LDFLAGS="-fopenmp -lGL -lGLU -lglut -lm"

run: $(BIN)
	./$(BIN) config/default.txt

clean:
	rm -rf $(BUILDDIR)

.PHONY: all clean run debug headless

# ---- test targets --------------------------------------------------------

CC_TEST = cc -O0 -g -Wall -fopenmp

build/test_ipc: src/ipc.c tests/test_ipc.c | $(BUILDDIR)
	$(CC_TEST) src/ipc.c tests/test_ipc.c -o build/test_ipc -lm

build/test_config: src/config.c tests/test_config.c | $(BUILDDIR)
	$(CC_TEST) src/config.c tests/test_config.c -o build/test_config -lm

build/test_member: src/ipc.c src/config.c tests/test_member.c | $(BUILDDIR)
	$(CC_TEST) src/ipc.c src/config.c tests/test_member.c -o build/test_member -lm

build/test_referee: src/ipc.c src/config.c src/member.c src/referee.c tests/test_referee.c | $(BUILDDIR)
	$(CC_TEST) src/ipc.c src/config.c src/member.c src/referee.c tests/test_referee.c -o build/test_referee -lm

.PHONY: test test-unit test-scenarios test-stress

test: test-unit test-scenarios test-stress

test-unit: build/test_ipc build/test_config build/test_member build/test_referee
	@echo "=== Unit Tests ==="
	@./build/test_ipc
	@./build/test_config
	@./build/test_member
	@./build/test_referee

test-scenarios: $(BIN)
	@echo "=== Scenario Tests ==="
	@bash tests/test_scenarios.sh

test-stress: $(BIN)
	@echo "=== Stress Tests ==="
	@bash tests/test_stress.sh
