# Makefile for SNES port
#
# This is scaffolding.  Choose a compiler by setting COMPILER=vbcc|calypsi.
# Nothing compiles yet -- this documents the intended build structure.

# ==== Compiler selection ====
COMPILER ?= vbcc

# ==== Paths ====
SRCDIR   = ../../src
PORTDIR  = .
BUILDDIR = build

# ==== Source files ====
# Platform-specific sources (this port)
PORT_SOURCES = \
	main.c \
	platform/snes_video.c \
	platform/snes_audio.c \
	platform/snes_input.c \
	platform/snes_timer.c

# Assembly sources
ASM_SOURCES = \
	crt0.s \
	vectors.s

# Game library sources (subset of src/ that can compile for 65816)
# Initially empty -- files are added as they become compatible.
# The goal is to eventually include all of src/ except:
#   - core/fiber.c (coroutines -- needs replacement)
#   - vendor/lakesnes/ (SPC700 emulator -- replaced by real hardware)
#   - snes/ppu_render.c (software renderer -- not needed on SNES)
#   - core/state_dump.c (debug feature -- needs stdio)
GAME_SOURCES =

# ==== Compiler-specific configuration ====
ifeq ($(COMPILER),vbcc)
  # vbcc 65816 target
  CC      = vc +65816
  AS      = ca65
  LD      = vlink
  CFLAGS  = -c99 -O2 -I$(SRCDIR) -I$(SRCDIR)/include -D__SNES__
  LDFLAGS = -T snes_port.cfg
  ASFLAGS = --cpu 65816
else ifeq ($(COMPILER),calypsi)
  # Calypsi 65816
  CC      = cc65816
  AS      = as65816
  LD      = ln65816
  CFLAGS  = --code-model=large --data-model=small -I$(SRCDIR) -I$(SRCDIR)/include -D__SNES__
  LDFLAGS = -o $(BUILDDIR)/earthbound_port.sfc
  ASFLAGS =
else
  $(error Unknown COMPILER=$(COMPILER). Use vbcc or calypsi.)
endif

# ==== Derived file lists ====
PORT_OBJS = $(PORT_SOURCES:%.c=$(BUILDDIR)/port/%.o)
GAME_OBJS = $(GAME_SOURCES:%.c=$(BUILDDIR)/game/%.o)
ASM_OBJS  = $(ASM_SOURCES:%.s=$(BUILDDIR)/asm/%.o)
ALL_OBJS  = $(ASM_OBJS) $(PORT_OBJS) $(GAME_OBJS)

TARGET = $(BUILDDIR)/earthbound_port.sfc

# ==== Rules ====
.PHONY: all clean

all: $(TARGET)

$(TARGET): $(ALL_OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $^

$(BUILDDIR)/port/%.o: $(PORTDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILDDIR)/game/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILDDIR)/asm/%.o: $(PORTDIR)/%.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -o $@ $<

clean:
	rm -rf $(BUILDDIR)
