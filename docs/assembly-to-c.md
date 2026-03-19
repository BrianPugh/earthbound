# Assembly-to-C Porting Guide

This guide covers the practical knowledge needed to port 65816 assembly functions from `asm/` to C in `src/`. The assembly was originally compiled from C by the VUCC compiler (Pax Softnica, ~1994), so most functions have recognizable C structure under the assembly syntax.

## Table of Contents

- [Quick Start: Porting a Function](#quick-start-porting-a-function)
- [Reading VUCC Assembly](#reading-vucc-assembly)
- [Function Prologue/Epilogue](#function-prologueepilogue)
- [Variables and Registers](#variables-and-registers)
- [Data Types](#data-types)
- [Common Assembly Patterns](#common-assembly-patterns)
- [Control Flow](#control-flow)
- [Function Calls](#function-calls)
- [Data Access Patterns](#data-access-patterns)
- [Gotchas and Common Bugs](#gotchas-and-common-bugs)
- [Testing and Debug](#testing-and-debug)
- [Worked Example](#worked-example)

---

## Quick Start: Porting a Function

1. **Find the assembly.** `grep -rn 'FUNCTION_NAME' asm/` to locate the `.asm` file. Read the entire file.
2. **Read the prologue.** Count `STACK_RESERVE_*` lines to understand local variables, parameters, and return type.
3. **Trace every constant.** Follow `LDA #immediate`, `.DEFINE`, enum references into `include/` headers. Never guess values.
4. **Write the C function.** Map virtual registers to local variables, `@LOCALxx` to local variables, branch labels to control flow. Port logic line by line.
5. **Grep before naming.** Check that new constant/function names don't collide with existing code.
6. **Build and test.** `cmake --build build` after each logical change.
7. **Rename unknowns.** If the assembly label starts with `UNKNOWN_`, give it a descriptive name (see [Renaming Workflow](#renaming-unknown-labels)).

---

## Reading VUCC Assembly

The 65816 is a 16-bit processor with an 8-bit accumulator mode. VUCC compiles C to 65816 with all operations in 16-bit mode (the prologue's `REP #$30` clears the 8-bit flags). Key registers:

| Register | Width | Role |
|----------|-------|------|
| A | 16-bit | Accumulator — arithmetic, comparisons, first parameter |
| X | 16-bit | Index register — second parameter, loop counter |
| Y | 16-bit | Index register — third parameter, array indexing |
| DP | 16-bit | Direct Page — points to the stack frame base |
| S | 16-bit | Stack pointer |
| DBR | 8-bit | Data Bank Register — determines bank for absolute addressing |

### File Structure

Each `.asm` file typically contains one function:

```asm
; Assembly file: asm/subsystem/function_name.asm

FUNCTION_NAME:              ; Global label (matches .GLOBAL in symbols/)
    BEGIN_C_FUNCTION_FAR    ; Prologue (FAR = RTL, plain = RTS)
    STACK_RESERVE_VARS      ; Allocate virtual registers + locals
    STACK_RESERVE_INT32     ; @LOCAL00 (4 bytes)
    STACK_RESERVE_INT16     ; @LOCAL01 (2 bytes)
    STACK_RESERVE_PARAM_INT16 ; Parameter passed in A register
    END_STACK_VARS          ; Adjust DP register

    ; ... function body ...

    END_C_FUNCTION          ; Restores DP, returns (RTL or RTS)
```

---

## Function Prologue/Epilogue

### BEGIN_C_FUNCTION vs BEGIN_C_FUNCTION_FAR

| Macro | Return | Call | Scope |
|-------|--------|------|-------|
| `BEGIN_C_FUNCTION` | `RTS` (near) | `JSR` | Same bank |
| `BEGIN_C_FUNCTION_FAR` | `RTL` (far) | `JSL` | Cross-bank |

In C, this distinction doesn't matter — just declare a normal function. It affects callers: assembly `JSR` = near call, `JSL` = far call.

### Stack Frame Layout

`STACK_RESERVE_VARS` always comes first and allocates 14 bytes for virtual registers (`@VIRTUAL00` through `@VIRTUAL0C`). Then local variables and parameters are declared:

```asm
STACK_RESERVE_VARS              ; @VIRTUAL00-0C (14 bytes)
STACK_RESERVE_INT32             ; @LOCAL00 at DP+0x0E (4 bytes)
STACK_RESERVE_INT32             ; @LOCAL01 at DP+0x12 (4 bytes)
STACK_RESERVE_INT16             ; @LOCAL02 at DP+0x16 (2 bytes)
STACK_RESERVE_PARAM_INT16       ; 1st param: passed in A register
STACK_RESERVE_PARAM_INT16       ; 2nd param: passed in X register
STACK_RESERVE_PARAM_INT16       ; 3rd param: passed in Y register
END_STACK_VARS                  ; PHD; TDC; ADC #offset; TCD
```

**Parameter passing:**
- First 3 **16-bit** (`STACK_RESERVE_PARAM_INT16`) params → A, X, Y registers (in order)
- **8-bit** params (`STACK_RESERVE_PARAM_INT8`) are always stack-passed, never register-passed
- **32-bit** params (`STACK_RESERVE_PARAM_INT32`) are always stack-passed
- 4th+ 16-bit params are stack-passed

The `STA @LOCAL03` after `END_STACK_VARS` stores the A register parameter into a local. If the function starts with logic using A directly (before any STA), the A register IS the first parameter being used inline.

### Return Values

- Small values (≤ 16-bit): returned in A register → C `return value;`
- 32-bit values: returned via the caller's virtual registers (caller reads `@VIRTUAL06`, etc. after the call)
- `STACK_RESERVE_RETURN_INT32` in the callee allocates space at the caller's frame

---

## Variables and Registers

### Virtual Registers

Virtual registers live at fixed DP offsets and serve as compiler temporaries:

| Name | Offset | Size | Typical Use |
|------|--------|------|-------------|
| `@VIRTUAL00` | $00 | 1 byte | Scratch (rare) |
| `@VIRTUAL01` | $01 | 1 byte | Scratch (rare) |
| `@VIRTUAL02` | $02 | 2 bytes | Temporary, loop counter, intermediate result |
| `@VIRTUAL04` | $04 | 2 bytes | Temporary, multiplicand, coordinate |
| `@VIRTUAL06` | $06 | 4 bytes | Pointer (lo word at $06, hi word at $08) |
| `@VIRTUAL0A` | $0A | 4 bytes | Pointer or 32-bit value (lo at $0A, hi at $0C) |

In C, map these to local variables:

```c
// Assembly:
//   LDA value
//   STA @VIRTUAL04
//   ...later...
//   LDA @VIRTUAL04
//   JSL SOME_FUNCTION

// C:
uint16_t temp = value;
some_function(temp);
```

### Local Variables

`@LOCALxx` are numbered sequentially starting at 0. Each `STACK_RESERVE_*` creates the next `@LOCALxx`. The size determines the type:

| Declaration | C Type | Notes |
|-------------|--------|-------|
| `STACK_RESERVE_INT8` | `uint8_t` | Rare; often stored as 16-bit anyway |
| `STACK_RESERVE_INT16` | `uint16_t` | Most common |
| `STACK_RESERVE_INT32` | `uint32_t` or pointer | Check usage context |

### Globals

Global variables are accessed by their label names in the assembly (e.g., `LDA GAME_STATE+game_state::leader_x_coord`). In C, these map to struct fields:

```asm
LDA GAME_STATE+game_state::leader_x_coord    →    game_state.leader_x_coord
STA ENTITY_FADE_ENTITY                        →    ow.entity_fade_entity = ...
LDA PAD_HELD                                  →    core.pad1_held
LDA PAD_PRESS                                 →    core.pad1_pressed
```

Check the C struct definitions in `src/game/overworld.h` (OverworldState `ow`), `src/game/game_state.h` (GameState `game_state`), `src/core/memory.h` (CoreState `core`), etc.

---

## Data Types

The 65816 defaults to 16-bit in VUCC mode. Key mappings:

| Assembly Pattern | C Type | Notes |
|-----------------|--------|-------|
| `LDA`/`STA` (16-bit mode) | `uint16_t` / `int16_t` | Default; check signed vs unsigned by branch type |
| `AND #$00FF` after `LDA` | `uint8_t` | Masking to 8-bit |
| `@VIRTUAL06` (4 bytes) | `uint32_t` or `const uint8_t *` | 32-bit pointer or value |
| `OPTIMIZED_MULT ..., .SIZEOF(struct)` | struct array index | `array[index]` |
| `ASL` (arithmetic shift left) | `<< 1` or `* 2` | Also used for array indexing |
| `LSR` (logical shift right) | `>> 1` | Unsigned divide by 2 |

### Signed vs Unsigned

The branch instruction after `CMP` tells you signed vs unsigned:

| Branch | Meaning | C Equivalent |
|--------|---------|-------------|
| `BCC` | Carry clear (unsigned <) | `(uint16_t)a < (uint16_t)b` |
| `BCS` | Carry set (unsigned >=) | `(uint16_t)a >= (uint16_t)b` |
| `BMI` | Negative (bit 15 set) | `(int16_t)a < 0` |
| `BPL` | Positive (bit 15 clear) | `(int16_t)a >= 0` |
| `BEQ` | Equal (zero flag set) | `a == b` |
| `BNE` | Not equal | `a != b` |

**Critical:** `BCC`/`BCS` after `CMP` is **unsigned** comparison. Use `(uint16_t)` casts in C if working with potentially negative values.

---

## Common Assembly Patterns

### 32-bit Pointer Load (`LOADPTR`)

```asm
LOADPTR SOME_TABLE, @VIRTUAL0A
; Loads the 24-bit ROM address of SOME_TABLE into @VIRTUAL0A (lo) and @VIRTUAL0C (hi bank)
```

C equivalent: `const uint8_t *ptr = asset_load("path/to/table.bin", &size);`
Or for ROM-embedded data: a `const` pointer to the data.

### 32-bit Copy (`MOVE_INT`)

```asm
MOVE_INT @VIRTUAL06, @LOCAL00
; LDA @VIRTUAL06; STA @LOCAL00; LDA @VIRTUAL06+2; STA @LOCAL00+2
```

C: `local00 = virtual06;` (just an assignment of a 32-bit value or pointer)

### 16-bit to 32-bit Widen (`MOVE_INT1632`)

```asm
MOVE_INT1632 @VIRTUAL02, @VIRTUAL06
; LDA @VIRTUAL02; STA @VIRTUAL06; STZ @VIRTUAL06+2
```

C: `uint32_t v06 = (uint32_t)v02;` (zero-extend 16-bit to 32-bit)

### NULL Check (`MOVE_INT_CONSTANT NULL`)

```asm
MOVE_INT_CONSTANT NULL, @VIRTUAL06
; Sets @VIRTUAL06 to 0x00000000
```

C: `ptr = NULL;` or `value = 0;`

### Struct Field Access

```asm
LDA #struct::field_name        ; Load the byte offset of the field
MOVE_INTX @VIRTUAL0A, @VIRTUAL06  ; Copy base pointer
CLC
ADC @VIRTUAL06                 ; Add offset to base
STA @VIRTUAL06                 ; Now points to the field
LDA [@VIRTUAL06]               ; Load the field value (indirect)
```

C: `value = entry->field_name;`

### Array Indexing with `OPTIMIZED_MULT`

```asm
LDA index
OPTIMIZED_MULT @VIRTUAL04, .SIZEOF(some_struct)  ; index * sizeof(entry)
CLC
ADC @VIRTUAL0A      ; base + index * sizeof
STA @VIRTUAL0A       ; pointer to entry[index]
```

C: `const SomeStruct *entry = &table[index];`

### Comparison and Branch

```asm
CMP #5
BEQL @CASE_5        ; Branch if Equal (Long)
CMP #6
BEQL @CASE_6
JMP @DEFAULT
```

C: `switch (value) { case 5: ... case 6: ... default: ... }`

Note: `BEQL` / `BNEL` are long-branch macros (the assembler translates to BEQ/BNE + JMP for distant targets).

### Loop Pattern

```asm
    LDX #0
    STX counter
    BRA @CHECK
@LOOP_BODY:
    ; ... loop body ...
    LDX counter
    INX
    STX counter
@CHECK:
    CPX #limit
    BCC @LOOP_BODY    ; while counter < limit
```

C: `for (int i = 0; i < limit; i++) { ... }`

### Self-XOR (Dead Code Pattern)

```asm
LDA SOME_VAR
EOR SOME_VAR         ; Always produces 0
BNEL @SOME_BRANCH    ; Never taken
```

This is dead code — `value ^ value` is always 0. When porting, check if both operands really are the same variable or if the disassembly mislabeled two different addresses. If they truly are the same, omit the dead code in C.

---

## Control Flow

### If/Else

```asm
    LDA condition
    BEQ @ELSE_BRANCH
    ; ... then body ...
    BRA @AFTER_IF
@ELSE_BRANCH:
    ; ... else body ...
@AFTER_IF:
```

C: `if (condition) { ... } else { ... }`

### While Loop (BRANCHGTS Pitfall)

```asm
@LOOP:
    ; ... body ...
    LDA #N
    CLC
    SBC counter
    BRANCHGTS @LOOP
```

**BRANCHGTS branches when N xor V = 0**, which includes result == 0 (not strictly > 0). With `CLC; SBC counter`, this computes `N - counter - 1`. BRANCHGTS when `N - counter - 1 >= 0` means `counter <= N - 1`, so this loops N times (counter 0 through N-1).

C: `for (int i = 0; i < N; i++) { ... }` — this is N iterations, NOT N-1.

### Switch/Dispatch

Large switch statements use sequential `CMP`/`BEQL` chains:

```asm
CMP #1
BEQL @CASE_1
CMP #2
BEQL @CASE_2
; ...
JMP @DEFAULT
```

C: Use a `switch` statement. The assembly's fall-through to `JMP @DEFAULT` maps to the `default:` case.

---

## Function Calls

### Near vs Far

| Assembly | Scope | C Port |
|----------|-------|--------|
| `JSR FUNC` | Same bank (near) | `func()` |
| `JSL FUNC` | Any bank (far) | `func()` |

No distinction in C — both become normal function calls.

### Parameter Passing

```asm
; Three 16-bit params: A=first, X=second, Y=third
LDA #42          ; first param
LDX #100         ; second param
LDY #0           ; third param
JSL SOME_FUNCTION
```

C: `some_function(42, 100, 0);`

For stack-passed params (8-bit, 32-bit, or 4th+ 16-bit):
```asm
; The value is written to the callee's stack frame offset before the call.
; Usually via STA to a stack-relative address.
```

### Return Value

```asm
JSL SOME_FUNCTION
CMP #0                ; Test return value (still in A)
BEQ @WAS_ZERO
STA @LOCAL02          ; Save return value
```

C: `uint16_t result = some_function(); if (result == 0) { ... }`

---

## Data Access Patterns

### Asset Loading

Assembly loads binary data via `BINARY`/`LOCALEBINARY` macros that `.INCBIN` files from `asm/bin/`. In C:

```c
// Assembly: BINARY "path/to/data.bin"  →  locale-independent
size_t size;
const uint8_t *data = asset_load("path/to/data.bin", &size);

// Assembly: LOCALEBINARY "path/to/data.bin"  →  locale-specific (US/ or JP/)
const uint8_t *data = asset_load_locale("path/to/data.bin", &size);
```

### Struct Tables

Assembly accesses table entries by computing `base + index * sizeof(entry)`:

```asm
LOADPTR TABLE_NAME, @VIRTUAL0A
LDA index
OPTIMIZED_MULT @VIRTUAL04, .SIZEOF(entry_struct)
CLC
ADC @VIRTUAL0A
STA @VIRTUAL0A
; @VIRTUAL0A now points to table[index]
LDY #entry_struct::field
LDA [@VIRTUAL0A],Y   ; Load field value
```

C: Define a packed struct matching the binary layout, then index:

```c
const EntryStruct *table = (const EntryStruct *)asset_load("table.bin", &size);
uint16_t value = table[index].field;
```

Use `PACKED_STRUCT` / `END_PACKED_STRUCT` / `ASSERT_STRUCT_SIZE` to guarantee the layout matches.

### VRAM Addressing

`VRAM_*` constants are **word addresses** (matching assembly). The C port's `ppu.vram[]` is a **byte array** — always multiply by 2:

```c
memcpy(&ppu.vram[VRAM_ADDR * 2], tile_data, size);
```

### DMA Size

A DMA transfer size of 0 means **65536 bytes** (0x10000), not 0 bytes. This is SNES hardware behavior.

---

## Gotchas and Common Bugs

### Little-Endian Byte Order

Script bytecode stores 16-bit values little-endian. The correct read pattern:

```c
uint16_t lo = script_read_byte(r);
uint16_t hi = script_read_byte(r);
uint16_t value = lo | (hi << 8);
```

Swapping lo/hi is a common and hard-to-diagnose bug.

### Entity Indexing

Entity arrays in the C port are packed `[MAX_ENTITIES]` and indexed directly by slot number. The `ENT(slot)` macro is identity (`#define ENT(slot) (slot)`) — it exists for readability but no longer multiplies by 2:

```c
#define ENT(slot) (slot)
entities.abs_x[ENT(slot)] = x;
```

Note: the assembly side still uses word-indexed tables (`MAX_ENTITIES * 2`). The WRAM-address mapping in `opcodes.c` bridges between ROM word-indexed layout and packed C arrays.

### BCC/BCS Is Unsigned

After `CMP`, `BCC` (branch carry clear) and `BCS` (branch carry set) perform **unsigned** comparison. Use explicit `(uint16_t)` casts in C when the values could be negative:

```c
// Assembly: CMP #100; BCC @LESS_THAN
if ((uint16_t)value < 100) { ... }
```

### Assembly Globals vs C Parameters

Assembly often uses global staging variables where C would use parameters. For example, `CURRENT_ENTITY_SLOT` is a global set before calling a function. The C port passes equivalent values as explicit parameters — this is correct as long as callers pass the right value.

### The 0x0020 Tile Code

In assembly text routines, `LDA #$0020` is a space character (tile code for a blank space in the SNES tilemap). When printing, this maps to `print_char_with_sound(0x0020)`.

### EB Character Encoding

EarthBound uses its own character encoding, not ASCII. Key ranges:
- Space=0x50, `0`-`9`=0x60-0x69, `A`-`Z`=0x71-0x8A, `a`-`z`=0x91-0xAA
- Use `ascii_to_eb_char()` / `eb_char_to_ascii()` from `text.h` for conversion
- `print_string()` accepts ASCII and converts internally; `print_eb_string()` takes raw EB codes

---

## Testing and Debug

### Debug Hotkeys (Unix Port)

| Key | Action |
|-----|--------|
| F5 | Toggle debug mode (`ow.debug_flag`) |
| F1 | Dump PPU state and VRAM as BMP |
| F2 | VRAM visualization |
| F3 | Toggle FPS overlay |
| F4 | Dump game state to binary file |
| Tab | Toggle 4x fast-forward |

### Debug Menu

With debug mode enabled (F5), hold **B** or **SELECT** and press **R** on the overworld to open the debug menu. 23 commands including:

- **Flag** — Interactive event flag editor (d-pad to browse flags, A to toggle)
- **Goods** — Item giver (d-pad to browse items, A to give to character)
- **Save** — Quick save
- **Warp** — Teleport to Onett center
- **Tea** — Random coffee/tea scene
- **Teleport/Star~/Star^** — Learn special PSI abilities
- **Player 0/1** — Open naming screen
- **CAST/STAFF** — Play cast/credits then return
- **STONE** — Play sound stone melody
- **Meter** — Toggle HP/PP flipout mode

### Sanitizer Build

For catching memory and undefined behavior bugs:

```
cd port/unix
cmake -S . -B build-debug -DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

---

## Worked Example

Let's port `DEBUG_Y_BUTTON_GUIDE` (a simple function) step by step.

### Step 1: Read the Assembly

File: `asm/overworld/debug/y_button_guide.asm`

```asm
DEBUG_Y_BUTTON_GUIDE:
    BEGIN_C_FUNCTION_FAR
    STACK_RESERVE_VARS
    STACK_RESERVE_INT32         ; @LOCAL00 (unused directly)
    STACK_RESERVE_INT16         ; @LOCAL01 — loop counter
    STACK_RESERVE_INT16         ; @LOCAL02 — entity count
    END_STACK_VARS
    LDX #0
    STX @LOCAL02                ; count = 0
    TXA
    STA @LOCAL01                ; i = 0
    BRA @COUNT_LOOP_CHECK
@COUNT_ENTITY:
    ASL                         ; i * 2 (entity offset)
    TAX
    LDA ENTITY_SCRIPT_TABLE,X   ; script_table[i*2]
    CMP #.LOWORD(-1)            ; == -1 (0xFFFF)?
    BEQ @NEXT_ENTITY
    LDX @LOCAL02
    INX
    STX @LOCAL02                ; count++
@NEXT_ENTITY:
    LDA @LOCAL01
    INC
    STA @LOCAL01                ; i++
@COUNT_LOOP_CHECK:
    CMP #MAX_ENTITIES           ; i < 30?
    BCC @COUNT_ENTITY
```

### Step 2: Analyze

- **Prologue:** FAR function (RTL), 3 locals: `@LOCAL00` (32-bit, unused by code), `@LOCAL01` (16-bit, loop counter), `@LOCAL02` (16-bit, count). No parameters.
- **Logic:** Counts entities with `script_table[ENT(i)] != -1`. Then displays the count in a window and waits for B/SELECT to exit.
- **Constants:** `MAX_ENTITIES` = 30, `WINDOW::FILE_SELECT_MENU` = 0x14, `.LOWORD(-1)` = 0xFFFF.

### Step 3: Write C

```c
static void debug_y_button_guide(void) {
    int count = 0;
    for (int i = 0; i < MAX_ENTITIES; i++) {
        if (entities.script_table[ENT(i)] != -1)
            count++;
    }

    set_instant_printing();
    create_window(WINDOW_FILE_SELECT_MENU);
    set_window_number_padding(3);
    print_number(count, 1);
    clear_instant_printing();
    window_tick();

    for (;;) {
        if (core.pad1_pressed & PAD_CANCEL)
            break;
        wait_for_vblank();
    }

    close_window(WINDOW_FILE_SELECT_MENU);
}
```

### Step 4: Verify

- Every constant traced: `MAX_ENTITIES` from `constants.h`, `WINDOW_FILE_SELECT_MENU` from `window.h`, `PAD_CANCEL` from `pad.h`.
- Entity indexing uses `ENT(i)` macro (not bare `i * 2`).
- Script table comparison matches assembly's `CMP #.LOWORD(-1)` = `!= -1`.
- Build: `cmake --build build` — no errors.

---

## Renaming Unknown Labels

When porting, rename any `UNKNOWN_*` labels to descriptive names:

### Global Function Labels (`UNKNOWN_C0xxxx`)

1. Read and understand the function
2. Choose a descriptive name, then `grep` the codebase to check for collisions
3. Update ALL of:
   - Source label in the `.asm` file
   - `.GLOBAL` declaration in `include/symbols/bankXX.inc.asm`
   - All callers (`JSL`/`JSR` references in other `.asm` files)
   - Event macros in `include/eventmacros.asm`
   - Event macro callers in `asm/data/events/*.asm`
4. Build with `make` to verify

### Local Branch Labels (`@UNKNOWNxx`)

Rename within the file to describe the branch's purpose:
- `@UNKNOWN7` → `@QUICK_MODE`
- `@UNKNOWN13` → `@BUTTON_PRESSED`

Local labels only need to be unique within their file.

### Naming Collision Avoidance

Common patterns when a name is already taken:
- `_FAR` suffix for far (RTL) wrappers of near (RTS) functions: `SET_INIDISP_FAR`
- `_LONG` suffix also used: `HIDE_HPPP_WINDOWS_LONG`
- Alternate verb: `UPDATE_ENTITY_SPRITE` taken → `RENDER_ENTITY_SPRITE`

---

## Reference: Key Header Files

| Header | Contents |
|--------|----------|
| `include/macros.asm` | VUCC calling convention macros, BINARY/LOCALEBINARY |
| `include/structs.asm` | Assembly struct definitions (char_struct, game_state, etc.) |
| `include/enums.asm` | Enumerations (DIRECTION, WALKING_STYLE, etc.) |
| `include/hardware.asm` | SNES hardware register addresses |
| `include/constants/` | Domain-specific constants (items, enemies, event_flags, music) |
| `include/symbols/` | ROM address symbols for cross-bank references |
| `include/eventmacros.asm` | Event script helper macros |

## Reference: C Port Conventions

| Convention | Details |
|------------|---------|
| Asset loading | `asset_load()` / `asset_load_locale()` |
| Packed structs | `PACKED_STRUCT` / `END_PACKED_STRUCT` / `ASSERT_STRUCT_SIZE` |
| Entity indexing | `ENT(slot)` macro, never bare `* 2` |
| VRAM addresses | Word addresses; multiply by 2 for `ppu.vram[]` byte offsets |
| EB text encoding | `ascii_to_eb_char()` / `eb_char_to_ascii()` in `text.h` |
| RNG | `rng_next_byte()` (0-255), matching assembly `RAND` |
| Wait for frame | `wait_for_vblank()` (yields to host, does rendering + input) |
