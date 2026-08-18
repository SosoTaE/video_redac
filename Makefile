# =============================================================================
#  video_redac — a text-to-video renderer (C11 host + CUDA device)
# =============================================================================
#
#  Build model:
#    .c  files → gcc  (plain C11, host logic)
#    .cu files → nvcc (device kernels + host glue)
#    linking   → nvcc, so it adds the CUDA runtime and does device linking
#
#  Two backends, one of which is linked in:
#    default   src/renderer.cu      CUDA kernels + NVENC     (needs nvcc + an NVIDIA GPU)
#    CPU=1     src/renderer_cpu.c   OpenMP loops + libx264   (needs neither)
#
#  The CPU build touches nvcc nowhere — not for compiling and not for linking —
#  so it works on a machine with no CUDA toolkit installed at all. That is the
#  whole point of it; if it ever starts requiring nvcc, the target is broken.
#
#  Targets:
#    make            — build (CUDA)
#    make CPU=1      — build (CPU only, no CUDA required)
#    make run        — build + render showcase.json
#    make debug      — debug build (-g -G, with sanitizers)
#    make info       — environment diagnostics
#    make clean      — remove objects and the binary
# =============================================================================

TARGET   := video_redac

#
# One object directory per backend.
#
# They used to share `build`, and because the two backends also share the
# binary's name, switching between them did nothing at all: `make CPU=1` after a
# CUDA build found the target newer than every object it cared about and
# reported "nothing to be done", leaving the CUDA binary in place. A CPU/GPU
# comparison run that way compares the GPU against itself and reports perfect
# agreement, which is the most convincing way possible to measure nothing.
#
# Separate directories make the switch visible to make in one direction: under
# CPU=1 the objects are simply absent, so they get built. It is not enough on
# its own — going *back* to the CUDA build, its objects are all present and the
# binary the CPU build just wrote is newer than every one of them, so make would
# again say "nothing to be done" and leave the wrong backend in place.
#
# What no timestamp can express is *which* backend produced ./video_redac, so
# the answer is written down: see BACKEND_STAMP below.
BACKEND  := $(if $(CPU),cpu,gpu)
BUILD    := build/$(BACKEND)

# --- Tools ------------------------------------------------------------------
CC       := gcc
NVCC     := nvcc
CUDA_HOME ?= /opt/cuda

# nvcc needs a C++ host compiler it actually *recognises*. CUDA 13.x tops out
# at gcc 15, while the default on Arch is already gcc 16 → pin it explicitly.
# Override if your system differs (make NVCC_CCBIN=g++-14).
NVCC_CCBIN ?= $(shell command -v g++-15 || command -v g++-14 || command -v g++)

# --- GPU architecture -------------------------------------------------------
#
# The original brief asked for Ada Lovelace (sm_89), but this machine's GPU is
# an RTX 5070 — Blackwell, sm_120. A cubin built only for sm_89 will *not* run
# on Blackwell ("no kernel image is available for execution on the device").
#
# So we build a fat binary:
#   sm_89       — for real Ada cards (RTX 4090 and friends)
#   sm_120      — for this machine's Blackwell
#   compute_120 — PTX the driver can JIT for future architectures
#
# To restrict it to one architecture: make GENCODE="-arch=sm_120"
GENCODE ?= -gencode arch=compute_89,code=sm_89 \
           -gencode arch=compute_120,code=sm_120 \
           -gencode arch=compute_120,code=compute_120

# --- External dependencies --------------------------------------------------
CAIRO_CFLAGS := $(shell pkg-config --cflags cairo fontconfig)
# fontconfig is already a transitive dependency of cairo; naming it explicitly
# is what lets --list fonts enumerate the installed families.
CAIRO_LIBS   := $(shell pkg-config --libs cairo fontconfig)

# --- Flags ------------------------------------------------------------------
INCLUDES := -Iinclude

# _POSIX_C_SOURCE — for popen/pclose/clock_gettime under strict -std=c11.
# CPPFLAGS_EXTRA — extra defines from the command line, e.g:
# make CPPFLAGS_EXTRA=-DVR_PIPELINE_DEPTH=1
CPPFLAGS := $(INCLUDES) $(CAIRO_CFLAGS) -D_POSIX_C_SOURCE=200809L $(CPPFLAGS_EXTRA)

WARNINGS := -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align \
            -Wstrict-prototypes -Wwrite-strings -Wvla

OPT      := -O3 -march=native -fno-strict-aliasing

CFLAGS   := -std=c11 $(WARNINGS) $(OPT) -MMD -MP

# -Xcompiler passes host flags through nvcc; -O3 on both sides (host+device).
# -MMD -MP through to the host compiler, exactly as the .c rule does. Without
# it a change to pixel_ops.h rebuilt renderer_cpu.o and left renderer.o stale —
# and since the two backends are meant to be bit-identical, the resulting
# mismatch looks like a bug in the code rather than in the build.
NVCCFLAGS := -std=c++17 -O3 $(GENCODE) -ccbin $(NVCC_CCBIN) \
             --expt-relaxed-constexpr -lineinfo -MMD -MP \
             -Xcompiler "-Wall,-Wextra,-O3,-fno-strict-aliasing"

LDLIBS   := $(CAIRO_LIBS) -lm
# LDFLAGS_EXTRA — extra link flags from the command line (see the sanitize target).
LDFLAGS  := $(LDFLAGS_EXTRA)

# --- Sources ----------------------------------------------------------------
#
# Exactly one backend is compiled: renderer.cu and renderer_cpu.c define the
# same three symbols, so linking both would fail. The wildcard picks up
# everything else automatically.
ALL_C   := $(wildcard src/*.c) lib/cJSON.c

ifdef CPU
  C_SRCS  := $(filter-out src/renderer.cu,$(ALL_C))
  CU_SRCS :=
  # OpenMP is what makes this backend usable rather than merely correct.
  # It is optional on purpose: without it the pragmas are ignored and the
  # renderer runs single-threaded, which is still a working build.
  OPENMP  ?= -fopenmp
  CFLAGS  += $(OPENMP)
  LDFLAGS += $(OPENMP)
  # Turning OpenMP off (make CPU=1 OPENMP=) is a supported configuration, so
  # the resulting "ignoring #pragma omp" flood is noise, not a finding.
  ifeq ($(strip $(OPENMP)),)
    CFLAGS += -Wno-unknown-pragmas
  endif
else
  C_SRCS  := $(filter-out src/renderer_cpu.c,$(ALL_C))
  CU_SRCS := $(wildcard src/*.cu)
endif

C_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
CU_OBJS := $(patsubst %.cu,$(BUILD)/%.o,$(CU_SRCS))
OBJS    := $(C_OBJS) $(CU_OBJS)
DEPS    := $(OBJS:.o=.d)

# The CPU build links with plain gcc and never mentions the CUDA runtime;
# the CUDA build links with nvcc so it can do device linking.
ifdef CPU
  LINK    := $(CC)
  LINKFLAGS :=
else
  LINK    := $(NVCC)
  LINKFLAGS := $(GENCODE) -ccbin $(NVCC_CCBIN)
  LDFLAGS += -L$(CUDA_HOME)/lib64
endif

# =============================================================================
#  Rules
# =============================================================================

.PHONY: all run debug sanitize clean distclean info help
.DEFAULT_GOAL := all

all: $(TARGET)

#
# Which backend last wrote ./$(TARGET), as a file whose *content* is the answer.
#
# Rewritten only when the answer changes, so its timestamp moves exactly when
# the backend does and never otherwise — the binary depends on it, so switching
# backends relinks and staying put does not.
BACKEND_STAMP := build/.backend

.PHONY: FORCE
FORCE:

$(BACKEND_STAMP): FORCE
	@mkdir -p $(dir $@)
	@echo '$(BACKEND)' | cmp -s - $@ || echo '$(BACKEND)' > $@

# CUDA build: nvcc links, adding -lcudart itself and doing device linking.
# CPU build:  gcc links; nothing here refers to CUDA at all.
$(TARGET): $(OBJS) $(BACKEND_STAMP)
	@echo "  LD      $@"
	@$(LINK) $(LINKFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)
	@echo "  ready → ./$@ ($(BACKEND))"

# --- Host C11 ---------------------------------------------------------------
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# --- Device CUDA ------------------------------------------------------------
$(BUILD)/%.o: %.cu
	@mkdir -p $(dir $@)
	@echo "  NVCC    $<"
	@$(NVCC) $(CPPFLAGS) $(NVCCFLAGS) -c $< -o $@

# Automatic header dependencies (generated by gcc's -MMD).
-include $(DEPS)

# =============================================================================
#  Auxiliary targets
# =============================================================================

# The full demo: 5 scenes, 4 languages, every action.
# All renders go into out/ (see .gitignore), keeping the repository clean.
run: $(TARGET) | out
	./$(TARGET) showcase.json -o out/showcase.mp4 --dump

out:
	@mkdir -p out

# Debug: -g for the host, -G for the device (device optimisation is disabled,
# giving cuda-gdb and compute-sanitizer exact line numbers — at the cost of speed).
debug: OPT       := -O0 -g -fsanitize=address,undefined
debug: NVCCFLAGS := -std=c++17 -g -G $(GENCODE) -ccbin $(NVCC_CCBIN) --expt-relaxed-constexpr
# At link time the sanitizer flags belong to the host compiler, not to nvcc —
# nvcc does not recognise them, so they go through -Xcompiler.
#
# Careful: -Xcompiler treats commas as argument separators, so
# "-fsanitize=address,undefined" would split in two and "undefined" would be
# taken as an input file. Hence two separate -Xcompiler flags.
debug: LDFLAGS   += -Xcompiler=-fsanitize=address -Xcompiler=-fsanitize=undefined
debug: clean $(TARGET)

# Full-program ASAN/UBSAN — only possible on the CPU backend.
#
# The CUDA runtime and ASAN do not coexist (the driver's own allocations trip
# the interceptors), so `make debug` above can only sanitize the host half of a
# GPU build. With CPU=1 there is no CUDA runtime at all, which means the
# compositor, the effect stack and the NV12 conversion finally get checked too —
# the code paths that used to be reachable only through kernels.
#
#   make sanitize && ./video_redac showcase.json --range 0:1
# A recursive make: CPU=1 has to be set before the Makefile's conditionals are
# evaluated, which a target-specific variable is far too late to do.
sanitize:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory CPU=1 \
	    OPT="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined" \
	    LDFLAGS_EXTRA="-fsanitize=address,undefined"

info:
	@echo "CC        : $(shell $(CC) --version | head -1)"
	@echo "NVCC      : $(shell $(NVCC) --version | tail -2 | head -1)"
	@echo "ccbin     : $(NVCC_CCBIN) ($(shell $(NVCC_CCBIN) --version | head -1))"
	@echo "GPU       : $(shell nvidia-smi --query-gpu=name,compute_cap --format=csv,noheader 2>/dev/null || echo 'not found')"
	@echo "cairo     : $(shell pkg-config --modversion cairo)"
	@echo "ffmpeg    : $(shell ffmpeg -version 2>/dev/null | head -1 || echo 'not found')"
	@echo "nvenc     : $(shell ffmpeg -hide_banner -encoders 2>/dev/null | grep -c nvenc) encoder(s)"
	@echo "GENCODE   : $(GENCODE)"

clean:
	@rm -rf build $(TARGET)
	@echo "  cleaned"

# Remove render output (leaves the build alone)
distclean: clean
	@rm -rf out
	@echo "  out/ removed"

help:
	@echo "make           — build (CUDA + NVENC)"
	@echo "make CPU=1     — build the CPU backend (no CUDA needed)"
	@echo "make run       — build + render showcase.json"
	@echo "make debug     — debug build (cuda-gdb / compute-sanitizer)"
	@echo "make info      — environment diagnostics"
	@echo "make clean     — remove objects and binary"
	@echo "make distclean — also remove out/"
