# =====================================================================
# Makefile — Home Furnishing Competition (ENCS4330 Project #1)
# =====================================================================
# Targets:
#   make             — build with OpenGL + OpenMP
#   make headless    — build without OpenGL (no -lGL etc.)
#   make debug       — build with -O0 and asan-friendly flags
#   make run         — build and run with config/default.cfg
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
