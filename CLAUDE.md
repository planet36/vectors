# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A header-only C++ library of fixed-capacity vectors. There is no build system or package
manifest — just the headers and their standalone test programs.

- `fixed_vector.hpp` — `fixed_vector<T, N, Align>`: capacity `N` is a **compile-time**
  template parameter, backed by in-place `std::array<T, N>` storage (no heap allocation).
  Fully `constexpr`.
- `dynamic_fixed_vector.hpp` — `dynamic_fixed_vector<T, Align>`: the same container shape
  with capacity chosen at **run time** (constructor argument) and heap storage that may be
  **over-aligned** (`Align` can exceed `alignof(T)` — e.g. `std::byte` data aligned to 16 for
  SIMD). See its own "Design invariants" note below.
- `aligned_byte_buffer.hpp` — `aligned_byte_buffer<Align>`: the `std::byte` specialization of
  `dynamic_fixed_vector` (element type fixed to `std::byte`, so only `Align` is a template
  parameter, default 16). Same API, simpler and faster — see its differences below.

Two documents accompany the headers; an API change should update both:

- `README.md` — the entry point for a human reader: what the three containers are, a quick
  start for each, an API summary, and the build commands. Its snippets and commands are
  verified to compile and run as written — keep them that way rather than sketching pseudocode.
- `DESIGN.md` — the rationale behind the invariants below (why each was chosen, the allocation
  strategy, the edge cases). This file states the invariants and the README summarizes them;
  DESIGN.md is where the reasoning lives, so revising an invariant means revising it there too.

## Build & test

Requires **GCC 16 / `-std=gnu++26`** (uses `std::from_range`, ranges, concepts) and the
**{fmt}** library (tests include `<fmt/ranges.h>` and link `-lfmt`).

The repo assumes a personal wrapper `gpp` on `PATH` — it is just
`g++ $CPPFLAGS $CXXFLAGS "$@" -lfmt`, where `$CXXFLAGS` already sets `-std=gnu++26` and a
large warning set. Build and run a test:

```sh
gpp test-fixed_vector.cpp -o a.out && ./a.out
```

Notes:
- Each test file's leading comment shows the original command (`gpp -I ../include … && d ./a.out`).
  `../include` is historical — the header sits next to the tests here, so plain `gpp test-X.cpp`
  resolves the `#include "fixed_vector.hpp"`. `d` is a personal run helper; just use `./a.out`.
- Without the `gpp` wrapper, compile directly: `g++ -std=gnu++26 test-X.cpp -lfmt -o a.out`.
- There is no test framework or runner. A test passes when it **exits 0** (all `assert`s hold);
  each file also embeds its **expected stdout** in a trailing comment block — diff the program's
  output against that block to catch regressions the asserts don't cover.
- `test-fixed_vector.cpp` is the hand-written suite; `test-fixed_vector2.cpp` (ChatGPT) and
  `test-fixed_vector3.cpp` (Claude) are generated suites covering the same API.
- `test-dynamic_fixed_vector.cpp` covers `dynamic_fixed_vector` and
  `test-aligned_byte_buffer.cpp` covers `aligned_byte_buffer` (every member each). Because both
  hand-manage aligned heap memory (and the byte buffer reads partially-uninitialized storage),
  also run them once under sanitizers, e.g.:
  `g++ -std=gnu++26 -g -fsanitize=address,undefined test-aligned_byte_buffer.cpp -lfmt -o a.san && ./a.san`.

## Design invariants (the reason this isn't `std::inplace_vector`)

The header's class docstring lists the intended differences from `std::inplace_vector` /
`boost::static_vector`. These are deliberate and drive the whole implementation — do not
"fix" them toward standard-container semantics without cause:

- **All N elements are value-initialized at construction.** Storage is a real
  `std::array<T, N>`, fully constructed up front, not raw aligned bytes.
- **Elements are never destroyed.** `clear()`, `pop_back()`, and `resize()` only adjust the
  `size_` counter; the underlying array elements stay alive. Consequently the type is
  constrained to `std::is_trivially_destructible_v<T>` (enforced in the `requires` clause,
  along with `N > 0`, `default_initializable`, `movable`, and `Align` being a power of two).
- **`operator[]` is capacity-based and unchecked** — it can legitimately read an initialized
  element at an index `>= size()` (this is tested intentionally). `at()` is the only
  bounds-checked accessor.

### `dynamic_fixed_vector` differences from `fixed_vector`

Same invariants (all capacity elements alive up front — value-initialized by the reserve
constructor, constructed directly from the source by the copy/fill/range constructors — never
destroyed, trivially destructible only, unchecked `operator[]`), but adapted for runtime
capacity + heap storage:

- **Storage** is `std::unique_ptr<T, aligned_deleter>` over a block from
  `::operator new(bytes, std::align_val_t{Align})`, freed by the matching aligned
  `::operator delete`. **Do not** rewrite this as an array `new`/`unique_ptr<T[]>`: the
  over-alignment comes from the `Align` template parameter, not from `T`, so `delete[]` would
  route to the non-aligned deallocation → UB. The `allocate_` helper also guards
  `capacity * sizeof(T)` against `std::size_t` overflow (the language's array-new check is
  bypassed when you size the allocation yourself).
- **`capacity()` / `max_size()` are non-static** and return the runtime capacity (deliberately
  **not** a `SIZE_MAX`-ish value like `std::vector::max_size()`).
- **`data()` applies `std::assume_aligned<Align>`** (guarded for the null/empty case) so caller
  loops can vectorize.
- **`X(n)` reserves capacity `n` and starts empty** (`size()==0`) — unlike `fixed_vector`
  where `X(count)` created `count` elements. Range / iterator-sentinel **constructors require
  forward** iterators (capacity must be computed up front); input-only sources use `X(capacity)`
  then `append_range`. (`append_range` itself still accepts input iterators.)
- **Copy is a deep copy; move construction transfers the pointer** and leaves the source empty
  (capacity 0). Move assignment swaps, so the source keeps the target's former buffer until it
  is destroyed. Copy/move assignment replace capacity too; `assign_range` keeps the current
  capacity and throws `std::bad_alloc` if the source doesn't fit.
- **`constexpr`/`noexcept` annotated like `fixed_vector`,** but over-aligned allocation is not
  usable in constant evaluation, so only empty/zero-capacity instances (and the non-allocating
  members) are usable in constant expressions — the allocating paths sit behind a `capacity == 0`
  guard. Each test has a `static_assert(constexpr_empty_ok())` verifying this.

### `aligned_byte_buffer` differences from `dynamic_fixed_vector`

The `std::byte` specialization keeps the same API and error-handling conventions but exploits
the fixed element type:

- **No `T` parameter;** only `Align` (power of two, default 16). The `requires` clause reduces
  to `(std::has_single_bit(Align))`.
- **Allocation is `::operator new(capacity, std::align_val_t{Align})`** — no overflow guard
  (`sizeof(std::byte) == 1`, so byte count == capacity).
- **Reserved capacity is left uninitialized:** lifetimes are begun with
  `std::start_lifetime_as_array<std::byte>` (no whole-capacity `memset`). Bytes that enter
  `size()` are always written; reading beyond `size()` via `operator[]` yields an *unspecified*
  byte — **well-defined, not UB, for `std::byte`** (so the test does not assert a value there,
  unlike the `dynamic_fixed_vector` test).
- **Byte-grained bulk ops:** `std::memcpy` for the span append fast path (non-overlap assumed),
  `std::memset` for `fill_*` / `resize`-grow; the copy ctor copies only the live `[0,size)`
  bytes; `operator==` / `operator<=>` are unconditional (`std::byte` is always comparable).
- **Zeroization, emplace constraint, constant-time compare:** `zeroize_remaining_space()` (see
  API conventions) additionally turns the *unspecified* reserved tail into determinate zeros
  (lane padding). `emplace_back` accepts at most one `std::byte`/integral argument (floats and
  other enums rejected). The free function `constant_time_equal(span, span)` compares with no
  data-dependent branches, for secret-dependent data (e.g. tag verification); the container's
  `operator==` stays variable-time.

## API / error-handling conventions

- Capacity overflow throws **`std::bad_alloc`** (not `length_error`); `at()` throws
  **`std::out_of_range`**. The `try_*` family (`try_push_back`, `try_emplace_back`,
  `try_append_range`) returns `bool` instead of throwing and is marked `[[nodiscard]]`.
- `unchecked_*` variants skip the capacity check and assume `!is_full()` — the checked
  `emplace_back`/`push_back`/`append_range` delegate to them after validating.
- Append overloads that can know the source size up front (span, iterator+count,
  `initializer_list`, sized ranges/sentinels) are all-or-nothing; truly unsized sources append
  element-wise and may partially append before throwing / returning `false`.
- `zeroize_remaining_space()` (all three; trivially copyable element types only) zeroizes the
  reserved tail with non-elidable stores (`memset_explicit`/`explicit_bzero` when the libc
  declares one — detected by name lookup, there is no feature-test macro — else a volatile-write
  fallback); `clear()` + `zeroize_remaining_space()` scrubs the whole container. In
  `fixed_vector` it also works in constant evaluation (value-assigns the tail).
- Nearly the entire interface is `constexpr`. `operator==` / `operator<=>` are gated on
  `std::equality_comparable` / `std::three_way_comparable`.
- `append_range` / `assign_range` are overloaded for span, iterator+sentinel, iterator+count,
  `initializer_list`, and arbitrary input ranges; `assign_range` is `clear()` + `append_range`.
