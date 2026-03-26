all: earthbound

.SUFFIXES:
%: %,v
%: RCS/%,v
%: RCS/%
%: s.%
%: SCCS/s.%

SRCDIR = asm/bankconfig
BUILDDIR = build

CA65FLAGS = -t none --cpu 65816 --bin-include-dir asm --include-dir asm --include-dir include --bin-include-dir $(BUILDDIR)
LD65FLAGS = -C snes.cfg

JPID = JP
USID = US
USPROTOID = US19950327

.PHONY: earthbound proto19950327 mother2 depsjp depsusa depsusaproto extract extractproto extractjp

earthbound: depsusa build/earthbound.sfc
proto19950327: depsusaproto build/earthbound-1995-03-27.sfc
mother2: depsjp build/mother2.sfc

ifeq ($(MAKECMDGOALS),mother2)
CA65FLAGS += -D JPN
include $(wildcard $(BUILDDIR)/$(JPID)/*.dep)
else ifeq ($(MAKECMDGOALS),proto19950327)
CA65FLAGS += -D USA -D PROTOTYPE19950327
include $(wildcard $(BUILDDIR)/$(USPROTOID)/*.dep)
else ifneq ($(if $(MAKECMDGOALS),$(filter all earthbound,$(MAKECMDGOALS)),itsthedefaulttarget),)
# The above ifneq condition will trigger if either
# - MAKECMDGOALS is empty (there is no specified make target, so build the default target), or
# - the target is all/earthbound
CA65FLAGS += -D USA
include $(wildcard $(BUILDDIR)/$(USID)/*.dep)
endif

# windows/posix mkdir differences, yay!
ifeq ($(shell echo "check_quotes"),"check_quotes")
   mkdir = mkdir $(subst /,\,$(1)) > nul 2>&1 || (exit 0)
else
   mkdir = mkdir -p $(1)
endif

JPSRCS = $(wildcard $(SRCDIR)/$(JPID)/*.asm)
JPOBJS = $(subst $(SRCDIR), $(BUILDDIR), $(patsubst %.asm, %.o, $(JPSRCS)))
USSRCS = $(wildcard $(SRCDIR)/$(USID)/*.asm)
USOBJS = $(subst $(SRCDIR), $(BUILDDIR), $(patsubst %.asm, %.o, $(USSRCS)))
USPROTOSRCS = $(wildcard $(SRCDIR)/$(USPROTOID)/*.asm)
USPROTOOBJS = $(subst $(SRCDIR), $(BUILDDIR), $(patsubst %.asm, %.o, $(USPROTOSRCS)))

$(BUILDDIR)/%.dep: $(SRCDIR)/%.asm
	@$(call mkdir, $(@D))
	ca65 $(CA65FLAGS) --listing "$(strip $(subst $(SRCDIR), $(BUILDDIR), $(patsubst %.dep,%.lst,$@)))" --create-dep "$(strip $(subst $(SRCDIR), $(BUILDDIR), $@))" -o "$(patsubst %.dep,%.o,$@)" "$<"

build/mother2.sfc: $(JPOBJS)
	ld65 $(LD65FLAGS) --mapfile "$(patsubst %.sfc,%.map,$@)" -o "$@" $^

build/earthbound.sfc: $(USOBJS)
	ld65 $(LD65FLAGS) --mapfile "$(patsubst %.sfc,%.map,$@)" -o "$@" $^

build/earthbound-1995-03-27.sfc: $(USPROTOOBJS)
	ld65 $(LD65FLAGS) --mapfile "$(patsubst %.sfc,%.map,$@)" -o "$@" $^

build/mother2.dbg: $(JPOBJS)
	ld65 $(LD65FLAGS) --dbgfile "$@" $^

build/earthbound.dbg: $(USOBJS)
	ld65 $(LD65FLAGS) --dbgfile "$@" $^

build/earthbound-1995-03-27.dbg: $(USPROTOOBJS)
	ld65 $(LD65FLAGS) --dbgfile "$@" $^

# ca65 requires all bin files to be present for generating .dep files, so make sure they're present first
depsjp: $(BUILDDIR)/main.spc700.bin $(subst $(SRCDIR), $(BUILDDIR), $(JPSRCS:.asm=.dep))
depsusa: $(BUILDDIR)/main.spc700.bin $(subst $(SRCDIR), $(BUILDDIR), $(USSRCS:.asm=.dep))
depsusaproto: $(BUILDDIR)/main.spc700.bin $(subst $(SRCDIR), $(BUILDDIR), $(USPROTOSRCS:.asm=.dep))

extract:
	ebtools extract "earthbound.yml" "donor.sfc"

extractproto:
	ebtools extract "earthbound-1995-03-27.yml" "donor-1995-03-27.sfc"

extractjp:
	ebtools extract "mother2.yml" "donorm2.sfc"

$(BUILDDIR)/%.o: $(SRCDIR)/%.asm
	@$(call mkdir, $(@D))
	ca65 $(CA65FLAGS) --listing "$(strip $(subst $(SRCDIR), $(BUILDDIR), $(patsubst %.o,%.lst,$@)))" -o "$@" "$<"

$(BUILDDIR)/%.spc700.bin: asm/spc700/%.spc700.s
	@$(call mkdir, $(@D))
	spcasm -f plain "$<" "$@"

%.bin: %.uncompressed
	inhal -n $< $@

# ── Unix C port (native PC build) ──────────────────────────────────
# Usage: make unix
#
# Prerequisites:
#   macOS:  brew install cmake sdl2
#   Ubuntu: sudo apt install cmake libsdl2-dev build-essential
#   Fedora: sudo dnf install cmake SDL2-devel gcc
#
# You also need Python 3.10+ and your EarthBound ROM as "earthbound.sfc"
# in this directory. The rest is handled automatically.

UNIX_BUILD_DIR = port/unix/build
UNIX_BINARY = $(UNIX_BUILD_DIR)/earthbound
NPROC = $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

.PHONY: unix unix-check-deps unix-check-rom unix-extract unix-venv

unix: unix-check-deps unix-check-rom unix-venv unix-extract
	@echo ""
	@echo "=== Configuring C port ==="
	@cmake -S port/unix -B $(UNIX_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	@echo ""
	@echo "=== Building C port ==="
	@cmake --build $(UNIX_BUILD_DIR) -j$(NPROC)
	@echo ""
	@echo "============================================"
	@echo "  Build complete!"
	@echo "  Run the game with:  ./$(UNIX_BINARY)"
	@echo "============================================"

unix-check-deps:
	@echo "=== Checking dependencies ==="
	@missing=""; \
	if ! command -v cmake >/dev/null 2>&1; then missing="$$missing cmake"; fi; \
	if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then missing="$$missing C-compiler"; fi; \
	if ! command -v python3 >/dev/null 2>&1; then missing="$$missing python3"; fi; \
	if ! command -v pkg-config >/dev/null 2>&1; then missing="$$missing pkg-config"; fi; \
	if [ -n "$$missing" ]; then \
		echo ""; \
		echo "ERROR: Missing required tools:$$missing"; \
		echo ""; \
		echo "Install them with:"; \
		echo "  macOS:  brew install cmake sdl2 pkg-config"; \
		echo "  Ubuntu: sudo apt install cmake libsdl2-dev build-essential pkg-config"; \
		echo "  Fedora: sudo dnf install cmake SDL2-devel gcc pkg-config"; \
		echo ""; \
		exit 1; \
	fi; \
	if ! pkg-config --exists sdl2 2>/dev/null; then \
		echo ""; \
		echo "ERROR: SDL2 development library not found."; \
		echo ""; \
		echo "Install it with:"; \
		echo "  macOS:  brew install sdl2"; \
		echo "  Ubuntu: sudo apt install libsdl2-dev"; \
		echo "  Fedora: sudo dnf install SDL2-devel"; \
		echo ""; \
		exit 1; \
	fi; \
	echo "  All dependencies found."

unix-check-rom:
	@if [ ! -f earthbound.sfc ]; then \
		echo ""; \
		echo "ERROR: EarthBound ROM not found."; \
		echo ""; \
		echo "Place your US EarthBound ROM in this directory as:"; \
		echo "  earthbound.sfc"; \
		echo ""; \
		exit 1; \
	fi

unix-extract: unix-venv
	@if [ ! -d asm/bin ] || [ ! -f asm/bin/assets.manifest ]; then \
		echo "=== Extracting game assets from ROM ==="; \
		. .venv/bin/activate && ebtools extract; \
	else \
		echo "=== Assets already extracted (skipping) ==="; \
	fi

unix-venv:
	@if [ ! -d .venv ]; then \
		echo "=== Setting up Python environment ==="; \
		python3 -m venv .venv; \
	fi
	@. .venv/bin/activate && pip install -q -e .
