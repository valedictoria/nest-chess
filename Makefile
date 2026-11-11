# Nest - build system
#
#   make -j build ARCH=<arch> [COMP=gcc|clang] [CXX=...] [EXE=name]
#   make -j profile-build ARCH=<arch> ...     PGO build, trained on "bench"
#   make net                                  download + verify the networks
#   make debug | tsan                         sanitizer builds (nest-debug / nest-tsan)
#   make test                                 perft + protocol tests
#   make help
#
# A plain "make" (as run by OpenBench: make EXE=... CXX=...) does a native
# build. If the two network files are present they are embedded in the binary.

### Configuration ---------------------------------------------------------------

EXE  ?= nest
ARCH ?= native
COMP ?= gcc
VERSION ?= 0.1

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(origin CXX),default)
  ifeq ($(COMP),clang)
    CXX := clang++
  else ifeq ($(UNAME_S),Darwin)
    CXX := clang++
  else
    CXX := g++
  endif
endif

# Which compiler family is CXX really? (Apple's g++ is clang.)
CXX_IS_CLANG := $(shell $(CXX) --version 2>/dev/null | grep -ci clang)

BIG_NET   := nn-1c0000000000.nnue
SMALL_NET := nn-37f18f62d772.nnue
BIG_SHA   := 1c0000000000
SMALL_SHA := 37f18f62d772
NET_URL   := https://raw.githubusercontent.com/official-stockfish/networks/master

### Architecture flags ------------------------------------------------------------

ARCH_FLAGS :=
PEXT := no
X86_V2 := -march=x86-64 -msse3 -mssse3 -msse4.1 -mpopcnt
X86_V3 := $(X86_V2) -msse4.2 -mavx -mavx2 -mbmi -mfma
X86_V4 := $(X86_V3) -mbmi2 -mavx512f -mavx512bw -mavx512dq -mavx512vl

ifeq ($(ARCH),native)
  ifeq ($(UNAME_M),arm64)
    ARCH_FLAGS := -mcpu=apple-m1
  else ifeq ($(UNAME_M),aarch64)
    ARCH_FLAGS := -mcpu=native
  else
    ARCH_FLAGS := -march=native
    # PEXT only where it is fast: BMI2 present and not AMD Zen 1/2 (family 23).
    HAS_BMI2 := $(shell $(CXX) -march=native -dM -E - < /dev/null 2>/dev/null | grep -c __BMI2__)
    IS_AMD   := $(shell grep -m1 -c AuthenticAMD /proc/cpuinfo 2>/dev/null)
    IS_FAM23 := $(shell grep -m1 'cpu family' /proc/cpuinfo 2>/dev/null | grep -c ': 23')
    ifeq ($(HAS_BMI2),1)
      ifneq ($(IS_AMD)$(IS_FAM23),11)
        PEXT := yes
      endif
    endif
  endif
else ifeq ($(ARCH),x86-64)
  ARCH_FLAGS := -march=x86-64
else ifeq ($(ARCH),x86-64-sse41-popcnt)
  ARCH_FLAGS := $(X86_V2)
else ifeq ($(ARCH),x86-64-avx2)
  ARCH_FLAGS := $(X86_V3)
else ifeq ($(ARCH),x86-64-bmi2)
  ARCH_FLAGS := $(X86_V3) -mbmi2
  PEXT := yes
else ifeq ($(ARCH),x86-64-avx512)
  ARCH_FLAGS := $(X86_V4)
  PEXT := yes
else ifeq ($(ARCH),x86-64-vnni)
  ARCH_FLAGS := $(X86_V4) -mavx512vnni
  PEXT := yes
else ifeq ($(ARCH),ivybridge)
  ARCH_FLAGS := -march=ivybridge
else ifeq ($(ARCH),armv8)
  ARCH_FLAGS := -march=armv8-a
else ifeq ($(ARCH),armv8-dotprod)
  ARCH_FLAGS := -march=armv8.2-a+dotprod
else ifeq ($(ARCH),apple-silicon)
  ARCH_FLAGS := -mcpu=apple-m1
else ifeq ($(ARCH),scalar)
  # Reference build without any vector code, for bit-exactness checks.
  ARCH_FLAGS := -DNEST_NO_SIMD
else
  $(error Unknown ARCH '$(ARCH)'. Run "make help" for the list)
endif

ifeq ($(PEXT),yes)
  ARCH_FLAGS += -DUSE_PEXT
endif

### Common flags ---------------------------------------------------------------------

SRCS := $(wildcard src/*.cpp) $(wildcard src/nnue/*.cpp)
HDRS := $(wildcard src/*.h) $(wildcard src/nnue/*.h) src/magics.inc

WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wcast-qual
CXXSTD   := -std=c++20
BASE     := $(CXXSTD) $(WARN) -fno-exceptions -fno-rtti -DNEST_VERSION=\"$(VERSION)\" $(ARCH_FLAGS)
OPT      := -O3 -DNDEBUG -flto=auto
LDFLAGS  += -pthread
ifneq ($(findstring MINGW,$(UNAME_S)),)
  LDFLAGS += -static
endif

ifeq ($(TUNE),yes)
  BASE += -DTUNE
endif

# Embed the networks when both files are available.
NETDIR ?= .
ifneq ($(wildcard $(NETDIR)/$(BIG_NET)),)
ifneq ($(wildcard $(NETDIR)/$(SMALL_NET)),)
  BASE += -DNEST_EMBED_BIG=\"$(abspath $(NETDIR)/$(BIG_NET))\" -DNEST_EMBED_SMALL=\"$(abspath $(NETDIR)/$(SMALL_NET))\"
endif
endif

VARIANT ?= release
OBJROOT := .obj
OBJDIR  := $(OBJROOT)/$(ARCH)-$(notdir $(CXX))-$(VARIANT)
PGO_DIR := $(abspath $(OBJROOT)/pgo-$(ARCH)-$(notdir $(CXX)))

### Targets ---------------------------------------------------------------------------

.PHONY: all build profile-build debug tsan net test clean help

all: build

help:
	@echo "make -j build ARCH=<arch> [COMP=gcc|clang] [CXX=...] [EXE=name]"
	@echo "make -j profile-build ARCH=<arch> ..."
	@echo "ARCH: native (default), x86-64, x86-64-sse41-popcnt, x86-64-avx2, x86-64-bmi2,"
	@echo "      x86-64-avx512, x86-64-vnni, ivybridge, armv8, armv8-dotprod, apple-silicon, scalar"
	@echo "Intel Mac (2012, Ivy Bridge): ARCH=ivybridge locally, x86-64-sse41-popcnt to distribute"
	@echo "Apple Silicon:                ARCH=apple-silicon"
	@echo "Other: net, debug, tsan, test, clean.  TUNE=yes exposes search constants as UCI options."

build:
	@$(MAKE) --no-print-directory $(EXE) EXTRA="$(OPT)" VARIANT=release

OBJS := $(patsubst src/%.cpp,$(OBJDIR)/%.o,$(SRCS))

$(OBJDIR)/%.o: src/%.cpp $(HDRS) $(wildcard $(NETDIR)/*.nnue)
	@mkdir -p $(dir $@)
	$(CXX) $(BASE) $(EXTRA) -c $< -o $@

$(EXE): $(OBJS)
	$(CXX) $(BASE) $(EXTRA) $(OBJS) -o $@ $(LDFLAGS)

# Profile-guided build: instrument, run bench, rebuild with the profile.
ifeq ($(CXX_IS_CLANG),0)
  PGO_GEN   := -fprofile-generate=$(PGO_DIR) -fprofile-update=prefer-atomic
  PGO_USE   := -fprofile-use=$(PGO_DIR) -fprofile-correction -fno-peel-loops -fno-tracer -Wno-missing-profile
  PGO_MERGE := true
else
  ifeq ($(UNAME_S),Darwin)
    PROFDATA := xcrun llvm-profdata
  else
    PROFDATA := $(shell command -v llvm-profdata || ls /usr/bin/llvm-profdata-* 2>/dev/null | sort -V | tail -1)
  endif
  PGO_GEN   := -fprofile-instr-generate=$(PGO_DIR)/nest-%p.profraw
  PGO_USE   := -fprofile-instr-use=$(PGO_DIR)/nest.profdata
  PGO_MERGE := $(PROFDATA) merge -output=$(PGO_DIR)/nest.profdata $(PGO_DIR)/*.profraw
endif

profile-build:
	@mkdir -p $(PGO_DIR)
	@find $(PGO_DIR) -type f \( -name '*.gcda' -o -name '*.profraw' -o -name '*.profdata' \) -delete
	@$(MAKE) --no-print-directory $(EXE)-pgo-gen EXE=$(EXE)-pgo-gen EXTRA="$(OPT) $(PGO_GEN)" VARIANT=pgo-gen
	./$(EXE)-pgo-gen bench > /dev/null
	@$(PGO_MERGE)
	@$(MAKE) --no-print-directory -B $(EXE) EXTRA="$(OPT) $(PGO_USE)" VARIANT=pgo-use
	@rm -f ./$(EXE)-pgo-gen

debug:
	@$(MAKE) --no-print-directory $(EXE)-debug EXE=$(EXE)-debug VARIANT=debug \
	  EXTRA="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"

tsan:
	@$(MAKE) --no-print-directory $(EXE)-tsan EXE=$(EXE)-tsan VARIANT=tsan \
	  EXTRA="-O2 -g -DNDEBUG -fsanitize=thread"

net:
	@for pair in $(BIG_NET):$(BIG_SHA) $(SMALL_NET):$(SMALL_SHA); do \
	  f=$${pair%%:*}; want=$${pair##*:}; \
	  if [ ! -f $$f ]; then echo "Downloading $$f"; curl -sSfL -o $$f $(NET_URL)/$$f || exit 1; fi; \
	  if command -v sha256sum >/dev/null; then got=$$(sha256sum $$f | cut -c1-12); else got=$$(shasum -a 256 $$f | cut -c1-12); fi; \
	  if [ "$$got" != "$$want" ]; then echo "$$f: SHA-256 prefix $$got, expected $$want. Delete the file and retry."; exit 1; fi; \
	  echo "$$f ok"; \
	done

test: build
	./tests/run_tests.sh ./$(EXE)

clean:
	rm -rf ./.obj
	rm -f ./$(EXE) ./$(EXE)-debug ./$(EXE)-tsan ./$(EXE)-pgo-gen
