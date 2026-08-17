# =============================================================================
#  video_redac — ტექსტიდან ვიდეოს რენდერერი (C11 host + CUDA device)
# =============================================================================
#
#  ბილდის მოდელი:
#    .c   ფაილებს  → gcc  (სუფთა C11, host ლოგიკა)
#    .cu  ფაილებს  → nvcc (device kernels + host-glue)
#    ლინკვა        → nvcc, რომ CUDA runtime და device კოდი თავად შეაერთოს
#
#  სამიზნეები:
#    make            — ბილდი
#    make run        — ბილდი + video.json-ის რენდერი
#    make debug      — დებაგ ბილდი (-g -G, sanitizer-ებით)
#    make info       — გარემოს დიაგნოსტიკა
#    make clean      — ობიექტების და ბინარის წაშლა
# =============================================================================

TARGET   := video_redac
BUILD    := build

# --- ინსტრუმენტები ----------------------------------------------------------
CC       := gcc
NVCC     := nvcc
CUDA_HOME ?= /opt/cuda

# nvcc-ს სჭირდება C++ host კომპილატორი, რომელსაც ის *ცნობს*. CUDA 13.x-ს
# ჭერი gcc 15-ია, Arch-ზე კი ნაგულისხმევი gcc უკვე 16-ია → პირდაპირ ვუთითებთ.
# შეცვალე, თუ სისტემაზე სხვა ვერსიაა (make NVCC_CCBIN=g++-14).
NVCC_CCBIN ?= $(shell command -v g++-15 || command -v g++-14 || command -v g++)

# --- GPU არქიტექტურა --------------------------------------------------------
#
# დავალებაში მოთხოვნილი იყო Ada Lovelace (sm_89), მაგრამ ამ მანქანის GPU
# RTX 5070-ია — Blackwell, sm_120. sm_89-ისთვის აგებული cubin *არ* გაეშვება
# Blackwell-ზე ("no kernel image is available for execution on the device").
#
# ამიტომ ვაგებთ fat binary-ს:
#   sm_89       — ნამდვილი Ada ბარათებისთვის (RTX 4090 და მისთ.)
#   sm_120      — ამ მანქანის Blackwell-ისთვის
#   compute_120 — PTX, რომელსაც დრაივერი მომავალ არქიტექტურებზე JIT-ით ააგებს
#
# ერთ არქიტექტურაზე შესაზღუდად: make GENCODE="-arch=sm_120"
GENCODE ?= -gencode arch=compute_89,code=sm_89 \
           -gencode arch=compute_120,code=sm_120 \
           -gencode arch=compute_120,code=compute_120

# --- გარე დამოკიდებულებები --------------------------------------------------
CAIRO_CFLAGS := $(shell pkg-config --cflags cairo)
CAIRO_LIBS   := $(shell pkg-config --libs cairo)

# --- ფლაგები ----------------------------------------------------------------
INCLUDES := -Iinclude

# _POSIX_C_SOURCE — popen/pclose/clock_gettime-ისთვის -std=c11-ის მკაცრ რეჟიმში.
# CPPFLAGS_EXTRA — ბრძანების ხაზიდან დამატებითი დეფინიციებისთვის, მაგ:
# make CPPFLAGS_EXTRA=-DVR_PIPELINE_DEPTH=1
CPPFLAGS := $(INCLUDES) $(CAIRO_CFLAGS) -D_POSIX_C_SOURCE=200809L $(CPPFLAGS_EXTRA)

WARNINGS := -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align \
            -Wstrict-prototypes -Wwrite-strings -Wvla

OPT      := -O3 -march=native -fno-strict-aliasing

CFLAGS   := -std=c11 $(WARNINGS) $(OPT) -MMD -MP

# -Xcompiler-ით host-ის ფლაგები nvcc-ს გავყავართ; -O3 ორივე მხარეს (host+device).
NVCCFLAGS := -std=c++17 -O3 $(GENCODE) -ccbin $(NVCC_CCBIN) \
             --expt-relaxed-constexpr -lineinfo \
             -Xcompiler "-Wall,-Wextra,-O3,-fno-strict-aliasing"

LDLIBS   := $(CAIRO_LIBS) -lm
LDFLAGS  := -L$(CUDA_HOME)/lib64

# --- წყაროები ---------------------------------------------------------------
C_SRCS  := $(wildcard src/*.c) lib/cJSON.c
CU_SRCS := $(wildcard src/*.cu)

C_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
CU_OBJS := $(patsubst %.cu,$(BUILD)/%.o,$(CU_SRCS))
OBJS    := $(C_OBJS) $(CU_OBJS)
DEPS    := $(C_OBJS:.o=.d)

# =============================================================================
#  წესები
# =============================================================================

.PHONY: all run debug clean distclean info help
.DEFAULT_GOAL := all

all: $(TARGET)

# ლინკვას nvcc აკეთებს: ის თავად ამატებს -lcudart-ს და ასრულებს device linking-ს.
$(TARGET): $(OBJS)
	@echo "  LD      $@"
	@$(NVCC) $(GENCODE) -ccbin $(NVCC_CCBIN) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "  მზადაა → ./$@"

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

# header-ების ავტომატური დამოკიდებულებები (gcc-ს -MMD-ით აგებული).
-include $(DEPS)

# =============================================================================
#  დამხმარე სამიზნეები
# =============================================================================

# სრული დემონსტრაცია: 5 სცენა, 4 ენა, ყველა ქმედება.
# ყველა რენდერი out/-ში იწერება (იხ. .gitignore) — რეპოზიტორია სუფთა რჩება.
run: $(TARGET) | out
	./$(TARGET) showcase.json -o out/showcase.mp4 --dump

out:
	@mkdir -p out

# დებაგში: -g host-ისთვის, -G device-ისთვის (device კოდის ოპტიმიზაცია ითიშება,
# რაც cuda-gdb-ს და compute-sanitizer-ს აძლევს ზუსტ ხაზებს — სამაგიეროდ ნელია).
debug: OPT       := -O0 -g -fsanitize=address,undefined
debug: NVCCFLAGS := -std=c++17 -g -G $(GENCODE) -ccbin $(NVCC_CCBIN) --expt-relaxed-constexpr
# ლინკვისას sanitizer-ის ფლაგები nvcc-ს კი არა, host კომპილატორს ეკუთვნის —
# nvcc მათ არ ცნობს, ამიტომ -Xcompiler-ით გადავცემთ.
#
# ყურადღება: -Xcompiler მძიმეს არგუმენტების გამყოფად კითხულობს, ამიტომ
# "-fsanitize=address,undefined" მასში ორად დაიშლებოდა და "undefined" შემავალ
# ფაილად ჩაითვლებოდა. სწორედ ამიტომ ორი ცალკე -Xcompiler-ია.
debug: LDFLAGS   += -Xcompiler=-fsanitize=address -Xcompiler=-fsanitize=undefined
debug: clean $(TARGET)

info:
	@echo "CC        : $(shell $(CC) --version | head -1)"
	@echo "NVCC      : $(shell $(NVCC) --version | tail -2 | head -1)"
	@echo "ccbin     : $(NVCC_CCBIN) ($(shell $(NVCC_CCBIN) --version | head -1))"
	@echo "GPU       : $(shell nvidia-smi --query-gpu=name,compute_cap --format=csv,noheader 2>/dev/null || echo 'ვერ მოიძებნა')"
	@echo "cairo     : $(shell pkg-config --modversion cairo)"
	@echo "ffmpeg    : $(shell ffmpeg -version 2>/dev/null | head -1 || echo 'ვერ მოიძებნა')"
	@echo "nvenc     : $(shell ffmpeg -hide_banner -encoders 2>/dev/null | grep -c nvenc) ენკოდერი"
	@echo "GENCODE   : $(GENCODE)"

clean:
	@rm -rf $(BUILD) $(TARGET)
	@echo "  გასუფთავდა"

# რენდერის შედეგების წაშლა (build-ს არ ეხება)
distclean: clean
	@rm -rf out
	@echo "  out/ წაიშალა"

help:
	@echo "make        — ბილდი"
	@echo "make run    — ბილდი + video.json-ის რენდერი"
	@echo "make debug  — დებაგ ბილდი (cuda-gdb / compute-sanitizer)"
	@echo "make info   — გარემოს დიაგნოსტიკა"
	@echo "make clean  — გასუფთავება"
	@echo "make distclean — + out/-ის წაშლა"
