# TYPE / STRUCT port plan

Sub-plan for Phase A item 5 of `docs/upstream-catchup-plan.md`. The feature is the centerpiece of upstream 6.02, and the catch-up plan explicitly calls for a sub-plan before starting because the surface area spans the program loader, variable table, expression parser, DIM/REDIM/ERASE machinery, and a new family of commands.

- **Upstream reference:** UKTailwind/PicoMiteAllVersions `@04f81d0`, version 6.02.02B0. Everything in scope is guarded by `#ifdef STRUCTENABLED`.
- **Our branch:** `catchup/type-struct` (off `main`, will land per the standard catch-up workflow).
- **Gate:** same as every catch-up port — host `./run_tests.sh` default compare mode green, `./build_firmware.sh rp2040` + `rp2350` green, `./host/build_wasm.sh` green.

## Feature surface

Totals from the scope pass (see "Upstream anchors" below):

| file | approx lines added | what lands |
|---|---|---|
| `Commands.c` | ~700 | `cmd_type`, `cmd_endtype`, `cmd_struct` (COPY / SORT / CLEAR / SWAP / SAVE), `fun_struct` (`STRUCT(FIND …)`), `ParseStructMember`, `FindStructType`, `FindStructMember`, `CheckIfTypeSpecified` hook into `cmd_dim` |
| `MMBasic.c` | ~300 | TYPE-block pass inside `PrepareProgramExt`, `ResolveStructMember`, `FindStructBase`, struct-aware `findvar` + `erase` paths |
| `MMBasic.h` | ~50 | `T_STRUCT` type flag, `s_structdef` / `s_structmember` typedefs, `MAX_STRUCT_TYPES`, `MAX_STRUCT_MEMBERS`, `MAX_STRUCT_NEST_DEPTH`, new externs |
| `AllCommands.h` | ~10 | `Type`, `End Type`, `Struct`, `Struct(` registrations |

New globals: `g_structtbl[MAX_STRUCT_TYPES]`, `g_structcnt`, `g_StructArg`, `g_StructMemberType`, `g_StructMemberOffset`, `g_StructMemberSize`, `g_ExprStructType`, plus the tokens `cmdTYPE` / `cmdEND_TYPE` fetched in the init path.

Field reuse: **no new `s_vartbl` fields**. When `type & T_STRUCT`, the existing `size` byte holds the struct-type index. That overload is a load-bearing invariant the port must preserve.

## Upstream anchors

For every step below, the porter reads upstream first — do not copy blindly (per the top-level plan). The numbers are from `git show upstream/main:<file>` at `@04f81d0`.

| concern | file:line | notes |
|---|---|---|
| TYPE block scan | `MMBasic.c:949-1157` | inside `PrepareProgramExt`; walks the tokenized program looking for `cmdTYPE`, calls `ParseStructMember` per member, rejects nested TYPE |
| `cmd_type` | `Commands.c:6663-6689` | runtime body just skips to `END TYPE` (definition already built in `PrepareProgramExt`) |
| `cmd_endtype` | `Commands.c:6690-6703` | error if reached directly |
| `cmd_struct` | `Commands.c:6704-7400+` | COPY / SORT / CLEAR / SWAP / SAVE sub-commands |
| `fun_struct` | `Commands.c:8060+` | `STRUCT(FIND …)` |
| `ParseStructMember` | `Commands.c:7776-7980` | ~205 lines; member name + type + optional dims + alignment |
| `FindStructType` | `Commands.c:7986-8010` | name → struct index |
| `FindStructMember` | `Commands.c:8014-8060` | struct index + name → offset / type / size |
| `ResolveStructMember` | `MMBasic.c:3371-3670` | ~300 lines; chained dot notation `a.b(i).c`, nesting depth up to `MAX_STRUCT_NEST_DEPTH`, sets `g_StructMemberType/Offset/Size`, returns pointer to member data |
| `FindStructBase` | `MMBasic.c:3686-3750` | dot-notation entry in `findvar` |
| `CheckIfTypeSpecified` + DIM init | `Commands.c:6286-6550` | resolves `AS mytype` via `FindStructType`; allocates `struct_size × num_elements`; supports `DIM s AS Point = (x, y, …)` initializer |
| alignment | `Commands.c:7939`, `MMBasic.c:1125-1130` | 8-byte align for INTEGER / FLOAT / nested-struct members; struct padded up so array elements sit on aligned offsets |
| REDIM with struct arrays | `Commands.c:5612-5705` | sets `g_StructArg = structIdx`, strips ` AS structtype` before `findvar`, PRESERVE copies existing struct data |
| ERASE string-member cleanup | `Commands.c:5785` | walks struct members, frees T_STR members with `g_StructMemberType` / member-size contract |
| `STRUCTENABLED` define | not located in scoping; check `configuration.h` and per-target CMake | needs a project-wide decision — see "Decisions" |

## Why this is interpreter-only for now

The VM's array-slot model is the reason REDIM is interpreter-only (see `docs/upstream-catchup-plan.md` REDIM row and the commit note on `ef9fc5d`). TYPE/STRUCT has the same problem, harder:

- `findvar` is the dot-notation entry point, but the VM emits `OP_LOAD_VAR_*` / `OP_LOAD_ARR_*` and never goes through `findvar`.
- `ResolveStructMember` sets global state (`g_StructMemberOffset/Type/Size`) that the interpreter threads into subsequent operations. The VM has no call site that would read those globals.
- `cmd_struct COPY/SORT/SWAP/SAVE` mutate interpreter-side memory that the VM's `vm->arrays[slot]` knows nothing about.

So in the first cut, **every struct-touching test gets `' RUN_ARGS: --interp`** and a `FRUN` program that references a struct is a "do not do that" case. If later performance demands struct support in `FRUN`, that's a separate design: native `OP_DIM_STRUCT` / `OP_LOAD_STRUCT_FIELD_*` opcodes and a VM-side struct registry. Out of scope here — note only.

## Decisions that need to be made before coding

1. **Enable `STRUCTENABLED` globally or gate it?** Upstream wraps every struct site in `#ifdef STRUCTENABLED`. Our HAL-first philosophy pushes against more `#ifdef`s. Options:
   - **Always on.** Add `#define STRUCTENABLED 1` in `configuration.h` (or remove the guard entirely from the port). Simpler, one less compile variant, but the feature ships into every firmware build.
   - **Per target.** Keep the guard, define it in every target we ship (rp2040, rp2350, host, wasm). Mechanically identical to "always on" but leaves an escape hatch.
   - **Keep it off somewhere.** No good reason — disabling the feature on rp2040 only splits the test matrix.

   **Recommended:** always-on, drop the guard in ported code. That matches the catch-up plan's invariant #4 ("No new hardware `#ifdef` gates added to core interpreter files").

2. **Alignment.** Upstream hardcodes 8-byte alignment for INTEGER / FLOAT / nested-struct members. That works across our targets too (ARM32 RP2040, ARM33 RP2350, x86_64 / arm64 host, wasm32). Keep it as-is.

3. **Struct-type capacity.** Upstream uses `MAX_STRUCT_TYPES = 32`, `MAX_STRUCT_MEMBERS = 16`. We keep the same values unless a test case demands more — memory impact is `32 × sizeof(s_structdef)` globally.

4. **Static memory vs. heap for `g_structtbl[]`.** Upstream allocates each `s_structdef` on the heap (`GetMemory`) from `PrepareProgramExt`. Keep that — it reuses the existing MMHeap management and is cleared on program reload.

## Phased port plan

Each phase ends with the full gate green. If a phase fails, its commit does not land — the next phase starts from the last green tree.

### Phase 1 — minimum viable TYPE (skeleton)

Goal: a program can declare a TYPE, `DIM` a scalar instance, write / read scalar numeric fields. No nested, no array-of-struct, no `STRUCT` sub-commands, no `fun_struct`.

- `MMBasic.h`: add `T_STRUCT`, `s_structdef`, `s_structmember`, capacity defines, new externs.
- `AllCommands.h`: register `Type`, `End Type`, `Struct` (body stub — sub-commands come later), no `Struct(`.
- `MMBasic.c`: fetch `cmdTYPE` / `cmdEND_TYPE` in init. Add the PrepareProgramExt TYPE-scan pass.
- `Commands.c`: add `cmd_type` (runtime skip), `cmd_endtype` (error), `ParseStructMember`, `FindStructType`, `FindStructMember`, `CheckIfTypeSpecified` (called from `cmd_dim` to resolve `AS mytype`).
- `MMBasic.c`: add `FindStructBase` and `ResolveStructMember` — dot-notation for a single scalar numeric field. Array-of-struct and nested struct return "not yet supported" errors.
- Test: `host/tests/frontend/t050_struct_basic.bas` (RUN_ARGS: --interp) — one TYPE with one INTEGER and one FLOAT member, DIM, assign, print.

Gate. Commit: `"Upstream 6.02 parity: TYPE/STRUCT phase 1 (skeleton) — scalar numeric fields"`.

### Phase 2 — string members

Add `T_STR` struct members. Needs the ERASE cleanup path (`Commands.c:5785`) so `ERASE s` (and program reload) frees string members without leaking or double-freeing.

Test: `t051_struct_string.bas` — string members assign / read / ERASE.

### Phase 3 — array-of-struct + struct field access `a(i).x`

Extend DIM allocation to `struct_size × (ubound+1)`. Extend `ResolveStructMember` to consume array indices before the dot (`points(i).x`). Extend `cmd_redim` to handle `AS structtype` arrays (the existing REDIM code already threads `g_StructArg` — port that call site).

Test: `t052_struct_array.bas` — DIM `pts(10) AS Point`, FOR-NEXT assign, read back, REDIM PRESERVE.

### Phase 4 — nested structs + chained field access `s.a.b.c`

Extend `ResolveStructMember` to handle nesting up to `MAX_STRUCT_NEST_DEPTH` (upstream has a hard limit — preserve it; document the limit in an error message).

Test: `t053_struct_nested.bas` — two TYPEs, one containing the other, chained read / write.

### Phase 5 — `STRUCT COPY` / `STRUCT CLEAR` / `STRUCT SWAP`

Port `cmd_struct` sub-commands COPY, CLEAR, SWAP (not SORT, not SAVE yet). These three are pure memory ops.

Test: `t054_struct_copy_clear_swap.bas`.

### Phase 6 — `STRUCT SORT`

Port the Shell sort (`Commands.c:6818-7050`, ~233 lines). Supports reverse, case-insensitive, empty-at-end flags. Nontrivial — gets its own commit.

Test: `t055_struct_sort.bas`.

### Phase 7 — `STRUCT SAVE` + `STRUCT(FIND …)`

File I/O for SAVE (`Commands.c:7157+`) and the `fun_struct` FIND subfunction (`Commands.c:8060+`). SAVE writes raw struct bytes — endian note in commit message.

Test: `t056_struct_save_find.bas`.

### Phase 8 — DIM initializer `DIM s AS Point = (1, 2)`

The init-list syntax from `Commands.c:6450-6550`. Last because it is sugar — the phases above give working structs without it.

Test: `t057_struct_dim_init.bas`.

## Risks and gotchas

From the scope report — things that will bite a porter who copies upstream verbatim:

1. **`g_vartbl[idx].size` overload.** Any code path that reads `.size` to mean "string length" must guard `!(type & T_STRUCT)`. Audit existing string-handling in `Commands.c` / `MATHS.c` / `I2C.c` for this pattern.
2. **`NAMELEN_STATIC` collision.** Static variables tag `namelen` with a bit flag; `FindStructBase` must mask it off before name comparison. Upstream does; our port must keep that.
3. **REDIM + struct arrays interaction.** Our REDIM port (commit `ef9fc5d`) doesn't know about `g_StructArg`. Phase 3 must wire that in — otherwise REDIM on a struct array crashes.
4. **ERASE string-member free.** Phase 2 requirement. `ERASE s` on a struct with string members must free each string slot; otherwise reload leaks.
5. **`checkstring` in `cmd_redim`'s LENGTH path.** My REDIM port synthesises `"VAR LENGTH n"` for string arrays. Struct-of-string-array is a new code path — verify the synthesis still works or extend it.
6. **Tokenizer greediness.** `End Type` is two tokens in the command table, but `End` alone is already a command. Confirm upstream's `AllCommands.h` ordering (longest-match first) and mirror it exactly.
7. **Alignment on wasm32.** Emscripten `wasm32` uses 4-byte `size_t` but 8-byte `long long`. Upstream's 8-byte align for INTEGER members is correct; test with a struct containing INTEGER + FLOAT + STR to be sure offsets match on all four targets.
8. **Program-reload / `NEW` cleanup.** `PrepareProgramExt` allocates `g_structtbl[]` entries on the heap; program reload must free them. Upstream does this somewhere — find it and mirror (probably in `ClearRuntime` or a dedicated `ClearStructs`).
9. **Commit size.** The skeleton (Phase 1) alone is ~400 lines across four files. Resist the temptation to squash Phase 1+2 into one commit — Phase 2 adds a new lifetime management concern (string-free on erase) that benefits from its own review.

## Test list

Each phase gets one test. Running list lives in this doc — strike through as committed:

- [ ] `t050_struct_basic.bas` — Phase 1
- [ ] `t051_struct_string.bas` — Phase 2
- [ ] `t052_struct_array.bas` — Phase 3
- [ ] `t053_struct_nested.bas` — Phase 4
- [ ] `t054_struct_copy_clear_swap.bas` — Phase 5
- [ ] `t055_struct_sort.bas` — Phase 6
- [ ] `t056_struct_save_find.bas` — Phase 7
- [ ] `t057_struct_dim_init.bas` — Phase 8

Every test header: `' RUN_ARGS: --interp` (struct support is interpreter-only in this port — see "Why this is interpreter-only for now").

## Exit criteria

- All eight phase tests green under `./run_tests.sh`.
- Firmware builds (rp2040, rp2350) produce `.uf2` without new warnings.
- WASM build produces `picomite.{mjs,wasm}` without new warnings.
- `docs/upstream-catchup-plan.md` updated: Phase A item 5 marked DONE with the commit range.
- Tag `v6.01-parity-plus-lang` applied on main (end of Phase A per the parent plan).

## Out of scope

- Native VM opcodes for struct field access (`OP_LOAD_STRUCT_FIELD_*`). Deferred until a user program's profile justifies it.
- TYPE inheritance (upstream doesn't have it).
- Struct-returning functions beyond what upstream's `CopyStructReturn` already handles.
- Anything in upstream's `STRUCTENABLED` block that isn't anchored in this doc — re-scope first, don't port cold.
