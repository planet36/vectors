# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A header-only C++ library of fixed-capacity vectors. Consuming it needs no build system or
package manifest — the headers are standalone. A `Makefile` builds and runs the test programs.

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

Requires **GCC 16 / `-std=c++26`** (uses `std::from_range`, ranges, concepts). No third-party
libraries: the tests need only the standard library.

```sh
make              # build the test programs -- both variants (release and debug, see below)
make test         # build if needed, then run both variants -- the one to run by default
make clean
```

Notes:
- There is no test framework or runner. **A test passes when it prints nothing and exits 0**;
  that exit status is the whole contract. On the first failed check the program prints one line
  to stderr (`file:line: function: CHECK failed: <expr>`) and exits `EXIT_FAILURE` immediately,
  leaving the remaining checks unrun.
- `make test` is that contract applied to all three programs in both variants: a passing run
  prints nothing at all. `set -e` stops at the first program that fails, and make names the
  target it was under. A single program still builds and runs by hand — `g++ -std=c++26
  test-fixed_vector.cpp -o test-fixed_vector && ./test-fixed_vector` — since nothing in the
  suites needs the Makefile.
- **`make test` runs both variants because neither subsumes the other**, and running only the
  release half is how a bug hides: it is the *weaker* check, yet the habitual one. A violated
  precondition passes it silently and only the debug build reports it.
  - The debug build finds what the release build hides: the asserts, `_GLIBCXX_DEBUG`, and the
    sanitizers.
  - The release build finds what the optimizer's assumptions do, and `-Og` cannot. `-Og` leaves
    `-fstrict-aliasing` **off** (it is on from `-O2`), which is exactly the UB class that
    `aligned_byte_buffer`'s `start_lifetime_as_array` lifetime work could harbor. And `-Og`
    never cashes in `data()`'s `assume_aligned<Align>`: the caller loop that promise exists for
    compiles to no SIMD at all at `-Og`, where `-O3 -march=native` emits the aligned `vmovdqa`
    that faults if the promise is ever broken. A debug-only run leaves that untested.
  - Checked and *not* a difference: `_GLIBCXX_DEBUG` does not change which `if constexpr
    (std::sized_sentinel_for<...>)` branch the tests take. `__debug::vector`'s iterators still
    model `contiguous_iterator` / `sized_sentinel_for`, so both variants exercise the same
    paths. Don't re-derive this one; it was measured.
- The debug half is `test-*.debug`, which `make` builds alongside the release binaries, with
  asserts, libstdc++ debug mode, fortified string ops, and ASan/UBSan (see the Makefile's
  `DEBUG_CXXFLAGS`, which explains each). The debug and release binaries have different names,
  so neither build ever silently serves the other's stale binary. Two of those flags are
  subtler than they look:
  - **`-UNDEBUG` is not decorative.** `assert` obeys `NDEBUG`, so an `NDEBUG` arriving from the
    environment's `CPPFLAGS` would disable every assert while the debug build still looked like
    it worked. It only wins because the recipe puts `DEBUG_CXXFLAGS` after `CPPFLAGS`; `-D`/`-U`
    apply in command-line order, so don't reorder them.
  - **`-D_FORTIFY_SOURCE=3` requires the `-Og`.** At `-O0` it warns and silently degrades to
    level 0. It does stay live under ASan.
- `test-utils.hpp` holds the shared harness: `CHECK` / `CHECK_THROWS`, `run_tests`, and the
  `to_ivec` / `to_byte` / `_b` / `is_aligned` helpers. **Do not use `assert`** in a test — it
  calls `abort()` and dumps core; `CHECK` exits cleanly instead. For the same reason `run_tests`
  catches everything, so a stray exception cannot reach `terminate()`. This bans `assert` from
  the *tests* only; the headers assert their own preconditions under `-DDEBUG` (below), which is
  the opposite situation — a caller in UB, where aborting loudly is the point.
- One suite per type, each covering every member — **including both overloads** (const and
  non-const accessors, `const&` and `&&` parameters). Each `{ }` block is a `static void
  test_<member>()` called from `main`, so reading `main` is how you audit that coverage.
- `test-fixed_vector.cpp` covers `fixed_vector`; because nearly its whole API is `constexpr`, it
  front-loads a `static_assert` block that drives the container at compile time — a semantic
  regression there fails the build, not just the run.
- `test-dynamic_fixed_vector.cpp` covers `dynamic_fixed_vector` and
  `test-aligned_byte_buffer.cpp` covers `aligned_byte_buffer` (every member each). Because both
  hand-manage aligned heap memory (and the byte buffer reads partially-uninitialized storage),
  they need a sanitizer run — `make test` covers it via the debug variant, and both are expected
  to be clean.

### `DEBUG` assertions in the headers

`-DDEBUG` (set by the Makefile's `DEBUG_CXXFLAGS`, so it reaches only the `test-*.debug`
binaries) enables an `assert` for each precondition the headers already
document with a `\pre` tag *and* can check cheaply: `!is_full()` on the `unchecked_*` family,
`!is_empty()` on `front`/`back`, and `i < capacity()` on `operator[]`. Each sits in a
`#if defined(DEBUG)` block, so a release build contains no `__assert_fail` at all.

- **The `\pre` tags are the spec; the asserts only enforce them.** Adding an assert without a
  matching `\pre`, or asserting something stricter than the tag says, is how this drifts into
  contradicting the design. In particular `operator[]` asserts `i < capacity()`, **not**
  `i < size()` — reading a live element at an index `>= size()` is intended, and the tests do it.
- The remaining `\pre` tags are unasserted: the non-overlap tags on the `span` overloads — and on
  the range overloads, which carry them for the contiguous case they forward to the `span`
  overload — would need a runtime aliasing check that these paths exist to avoid, and
  "`[first, last)` is a valid
  range" is not checkable *by the container*. `_GLIBCXX_DEBUG` covers that last one from the
  other side, whenever the iterators come from a std container — which is how the tests use it.
  It is worth keeping for that alone: an invalid iterator otherwise yields a garbage distance,
  which the up-front capacity check reports as **`std::bad_alloc`**, sending you after a
  phantom capacity bug. Debug mode names the real one instead ("attempt to copy a singular
  iterator", "iterators from different sequences"), and plain ASan does not catch it at all.
- `unchecked_push_back` delegates to `unchecked_emplace_back`, which is where its `!is_full()`
  assert lives — one check at the leaf, and violations through either overload still trip it.
- The asserts are `constexpr`-clean: an assert whose condition holds is fine in constant
  evaluation, so `test-fixed_vector.cpp`'s `static_assert` block still compiles under `-DDEBUG`.
- Standard `NDEBUG` caveat: `DEBUG` changes the definition of inline/template functions, so
  don't link a `-DDEBUG` TU against a non-`DEBUG` one — that is an ODR violation. `_GLIBCXX_DEBUG`
  is the same hazard one level down (it swaps in `__debug::vector` etc.), which is safe here only
  because each test is a single TU. Both are reasons the debug build gets its own binaries.
- `-fhardened` was considered and rejected: GCC warns that it declines to apply its own
  `_FORTIFY_SOURCE` / `_GLIBCXX_ASSERTIONS` when those are set explicitly (they are), and its
  remaining parts — PIE, relro, cf-protection, stack-protector — harden a shipped binary rather
  than find bugs in a test run, with ASan already detecting stack smashes more precisely.

## Design invariants (the reason this isn't `std::inplace_vector`)

The header's class docstring lists the intended differences from `std::inplace_vector` /
`boost::static_vector`. These are deliberate and drive the whole implementation — do not
"fix" them toward standard-container semantics without cause:

- **All N elements are value-initialized at construction.** Storage is a real
  `std::array<T, N>`, fully constructed up front, not raw aligned bytes.
- **Elements are never destroyed.** `clear()`, `pop_back()`, and `resize()` only adjust the
  `size_` counter; the underlying array elements stay alive. Consequently the type is
  constrained to `std::is_trivially_destructible_v<T>` (enforced in the `requires` clause,
  along with `N > 0`, `default_initializable`, `movable`, and `Align` being a power of two
  that is at least `alignof(T)`).
- **`Align >= alignof(T)` is deliberate in `fixed_vector`, not redundant** — do not drop it as
  "already guaranteed by `alignas`". It is a *diagnostic*: `alignas` cannot weaken natural
  alignment, so the array is safe either way, but a weakened `alignas` is ill-formed and GCC
  ignores it silently (Clang errors), so without the constraint `fixed_vector<int, 8, 1>` would
  compile and quietly ignore the request. See DESIGN.md; it is load-bearing on
  `dynamic_fixed_vector` for a different reason.
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
- **`Align >= alignof(T)` is in the `requires` clause and is load-bearing here** (unlike in
  `fixed_vector`, where it is only a diagnostic): the block is raw storage from the aligned
  `::operator new`, so nothing but the constraint prevents a smaller `Align` from under-aligning
  the elements → UB. Vacuous for `aligned_byte_buffer` (`alignof(std::byte) == 1`), which is why
  its clause is only `has_single_bit(Align)`.
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
- **The input-range overload's dispatch to the `span` overload is not redundant** — do not
  collapse it as "the `span` overload already handles that". A sized contiguous range of exactly
  the element type is forwarded there so it reaches the bulk copy; without that `if constexpr`,
  overload resolution picks the `R&&` template for `std::vector<T>` (exact match, vs. a
  user-defined conversion for the `span` overload) and the bulk path is dead code for every
  caller who does not hand-write a span. It inherits the `span` overload's non-overlap `\pre` for
  that case. Sized non-contiguous sources go through `unchecked_emplace_back` — the up-front size
  check already covers every element. See DESIGN.md.
