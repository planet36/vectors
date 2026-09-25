# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this
repository.

## What this is

This is a header-only C++ library of fixed-capacity vectors.  Consuming it needs no build system
or package manifest, since the headers are standalone.  They live in `include/`, while the test
suites and `test_utils.hpp` stay at the top level.  A `Makefile` builds and runs the test
programs, putting `include/` on the include path (`CPPFLAGS`) so no source names the directory.

- `include/fixed_vector.hpp` — `fixed_vector<T, N, Align>`: storage is an in-place
  `std::array<T, N>` (no heap allocation), so `N` is a **compile-time** bound.  `capacity()` is
  still a run-time value in `[0, N]` that `reserve()` moves, and `max_size()` is the `static`
  one reporting `N`.  It is fully `constexpr`.
- `include/dynamic_fixed_vector.hpp` — `dynamic_fixed_vector<T, Align>`: the same container
  shape with capacity chosen at **run time** (constructor argument) and heap storage that may be
  **over-aligned** (`Align` can exceed `alignof(T)`, e.g. `std::byte` data aligned to 16 for
  SIMD).  See its own "Design invariants" note below.
- `include/aligned_byte_buffer.hpp` — `aligned_byte_buffer<Align>`: the `std::byte`
  specialization of `dynamic_fixed_vector` (element type fixed to `std::byte`, so only `Align`
  is a template parameter, default 16).  It has the same API but is simpler and faster (see its
  differences below).
- `include/borrowed_byte_buffer.hpp` — `borrowed_byte_buffer`: a **non-owning**,
  run-time-capacity byte view.  It has the same byte-buffer API as `aligned_byte_buffer`, but
  over storage it does **not** own (a pointer or a contiguous range).  It never allocates,
  copies and moves shallowly, has no `Align` parameter, and adds `adopting` named constructors.
  See its own "differences" note below.
- `include/byte_compare.hpp` — the shared `equal_constant_time(span, span)` free function,
  `#include`d by both byte buffers.  It lives in its own header so it is defined once.  It is
  explicitly `inline` (a `volatile` accumulator bars `constexpr`), so two definitions across TUs
  would violate the ODR without it.

Two documents accompany the headers, and an API change should update both:

- `README.md` — the entry point for a human reader: what the four containers are, a quick
  start for each, an API summary, and the build commands.  Its snippets and commands are
  verified to compile and run as written.  Keep them that way rather than sketching pseudocode.
- `DESIGN.md` — the rationale behind the invariants below (why each was chosen, the allocation
  strategy, the edge cases).  This file states the invariants and the README summarizes them.
  DESIGN.md is where the reasoning lives, so revising an invariant means revising it there too.

The invariants are therefore stated in all three files, and they drift independently.
`DESIGN.md` is the authority for the *why* and `README.md` for the user-facing summary.  Where
this file disagrees with either, assume `DESIGN.md` is right and this file is what needs fixing.

`COMMENT-STYLE.md` holds the prose rules for code comments, doc blocks, and commit messages
here.  It governs how prose is written rather than what the code does, so an API change does not
update it.  It is written to be copied into any repo, so it names nothing specific to this one.

## Build & test

The code requires **GCC 16 / `-std=c++23`** (it uses `std::from_range_t`,
`std::start_lifetime_as_array`, ranges, and concepts).  There are no third-party libraries, and
the tests need only the standard library.

```sh
make              # build the test programs -- both variants (release and debug, see below)
make test         # build if needed, then run both variants -- the one to run by default
make clean
make lint         # clang-tidy over the four suites (and, via HeaderFilterRegex, the headers)
```

- There is no test framework or runner.  **A test passes when it prints nothing and exits 0**,
  and that exit status is the whole contract.  On the first failed check the program prints one
  line to stderr (`file:line: function: CHECK failed: <expr>`) and exits `EXIT_FAILURE`
  immediately, leaving the remaining checks unrun.
- `make test` is that contract applied to all four programs in both variants: a passing run
  prints nothing at all.  `set -e` stops at the first program that fails, and make names the
  target it was under.  A single program still builds and runs by hand —
  `g++ -std=c++23 -Iinclude test-fixed_vector.cpp -o test-fixed_vector && ./test-fixed_vector` —
  since nothing in the suites needs the Makefile beyond that `-I`.
- **`make lint` is advisory and is not part of `make test`.**  The recipe is prefixed with `-`,
  so a nonzero clang-tidy exit does not fail the build, and the checks are configured in
  `.clang-tidy` (`bugprone-*`, `cert-*`, `cppcoreguidelines-*`, `readability-*`, … minus a long
  opt-out list).  **It currently prints nothing**, and that silence is the baseline: a warning
  under a change is that change's, not a pre-existing noise floor to read past.  Every
  deliberate case is answered in place by a `// NOLINT*` comment, which makes those comments the
  record of what was decided.  **A silenced warning can still be one whose flagged form is the
  point**, so deleting the marker to "fix" the code is backwards.  Five cases stand, each for
  its own reason:
  - `cppcoreguidelines-missing-std-forward` on each of `borrowed_byte_buffer`'s three
    deliberately un-forwarded borrowing constructors: the two `R&&` range ones and the `P&&`
    single-object one (see DESIGN.md).  Nothing is consumed there: the type borrows, so it takes
    only `std::ranges::data(r)` / the pointee `sizeof`, and the forwarding reference is there to
    widen what binds (rvalue views as well as lvalue containers, and a C array reaching the
    range constructor rather than decaying), not to move from.
  - `performance-move-const-arg` / `hicpp-move-const-arg` on the two `std::move`s in
    `test-borrowed_byte_buffer.cpp` that test the non-emptying move.  The check is right that
    the `std::move` has no effect on a trivially copyable view, and that *is* the assertion, so
    removing it removes the test.
  - `cppcoreguidelines-avoid-c-arrays` on the C array that `test-borrowed_byte_buffer.cpp`
    borrows from: that array *is* the case being tested (a C array reaching the range
    constructor instead of decaying to a one-element view), so rewriting it as a `std::array` is
    not the fix.
  - `readability-use-anyofallof` on the element-wise loop in all four `try_append_range(R&&)`
    overloads.  The rewrite does not compile for the same inputs: `std::ranges::all_of` requires
    `indirect_unary_predicate`, which requires the predicate be invocable with
    `iter_value_t<I>&`, an *lvalue*.  `try_emplace_back` is constrained
    `std::constructible_from<T, Args...>`, so a move-only `T` (trivially destructible and
    `movable` but not copyable, which the `requires` clause admits) appends fine through the
    loop and fails to resolve as an `all_of`.  The loop needs only `iter_reference_t`.  It is
    also a mutating loop whose `false` means *out of capacity*, not *element failed a test*.
    That is the partial-append behavior the `\note` above it documents, which folding into a
    query-named algorithm hides.
  - `readability-simplify-boolean-expr` on the `if (!(...))` checks in the four suites'
    `constexpr_*_ok()` functions.  Where every check in a function is suppressed the marker is a
    `NOLINTBEGIN` / `NOLINTEND` pair spanning the whole body rather than a `NOLINTNEXTLINE` per
    line.  This is deliberate, so do not narrow them back to per-line comments.
- **`make test` runs both variants because neither subsumes the other**, and running only the
  release half is how a bug hides: it is the *weaker* check, yet the habitual one.  A violated
  precondition passes it silently and only the debug build reports it.
  - The debug build finds what the release build hides: the asserts, `_GLIBCXX_DEBUG`, and the
    sanitizers.
  - The release build finds what the optimizer's assumptions do, and `-Og` cannot.  `-Og` leaves
    `-fstrict-aliasing` **off** (it is on from `-O2`), which is exactly the UB class that
    `aligned_byte_buffer`'s `start_lifetime_as_array` lifetime work could harbor.  And `-Og`
    never cashes in `data()`'s `assume_aligned<Align>`: the caller loop that promise exists for
    compiles to no SIMD at all at `-Og`, where `-O3 -march=native` emits the aligned `vmovdqa`
    that faults if the promise is ever broken.  A debug-only run leaves that untested.
  - One thing was checked and is *not* a difference: `_GLIBCXX_DEBUG` does not change which
    `if constexpr (std::sized_sentinel_for<...>)` branch the tests take.  `__debug::vector`'s
    iterators still model `contiguous_iterator` / `sized_sentinel_for`, so both variants
    exercise the same paths.  Don't re-derive this one, since it was measured.
- The debug half is `test-*.debug`, which `make` builds alongside the release binaries, with
  asserts, libstdc++ debug mode, fortified string ops, and ASan/UBSan (see the Makefile's
  `DEBUG_CXXFLAGS`, which explains each).  The debug and release binaries have different names,
  so neither build ever silently serves the other's stale binary.  Two of those flags are
  subtler than they look:
  - **`-UNDEBUG` guards the asserts.**  `assert` obeys `NDEBUG`, so an `NDEBUG` reaching this
    build would disable every assert while the debug build still looked like it worked.  The
    `-U` only wins because the recipe puts `DEBUG_CXXFLAGS` after `CPPFLAGS`, and `-D`/`-U`
    apply in command-line order, so don't reorder them.
  - **`-D_FORTIFY_SOURCE=3` requires the `-Og`.**  At `-O0` it warns and silently degrades to
    level 0.  It does stay live under ASan.
- `test_utils.hpp` holds the shared harness: `CHECK` / `CHECK_THROWS`, `run_tests`, and the
  `to_ivec` / `to_byte` / `_b` / `is_aligned` helpers.  **Do not use `assert`** in a test,
  because it calls `abort()` and dumps core.  `CHECK` exits cleanly instead.  For the same
  reason `run_tests` catches everything, so a stray exception cannot reach `terminate()`.  This
  bans `assert` from the *tests* only.  The headers assert their own preconditions under
  `-DDEBUG` (below), which is the opposite situation: a caller in UB, where aborting loudly is
  the point.
- There is one suite per type, each covering every member, **including both overloads** (const
  and non-const accessors, `const&` and `&&` parameters).  Each `{ }` block is a
  `static void test_<member>()` called from `main`, so reading `main` is how you audit that
  coverage.
- `test-fixed_vector.cpp` covers `fixed_vector`.  Because nearly its whole API is `constexpr`,
  it front-loads `static_assert` blocks that drive the container at compile time, so a semantic
  regression there fails the build, not just the run.
- `test-dynamic_fixed_vector.cpp` covers `dynamic_fixed_vector` and
  `test-aligned_byte_buffer.cpp` covers `aligned_byte_buffer` (every member each).  Because both
  hand-manage aligned heap memory (and the byte buffer reads partially-uninitialized storage),
  they need a sanitizer run.  `make test` covers it via the debug variant, and both are expected
  to be clean.
- `test-borrowed_byte_buffer.cpp` covers `borrowed_byte_buffer` (every member).  It allocates
  nothing, but reads the borrowed tail past `size()` and forms its view via `reinterpret_cast`,
  so it wants the same sanitizer run.  It also carries the view-specific behavior (shallow-copy
  aliasing, non-emptying move, overlaying a typed object) and a `static_assert` block pinning
  the construction constraints (rvalue owners / `const` elements / non-pointer non-ranges
  rejected).

### `DEBUG` assertions in the headers

`-DDEBUG` (set by the Makefile's `DEBUG_CXXFLAGS`, so it reaches only the `test-*.debug`
binaries) enables an `assert` for each precondition the headers already document with a `\pre`
tag *and* can check cheaply: `!is_full()` on the `unchecked_*` family, `!is_empty()` on
`front`/`back`, `i < capacity()` on `operator[]`, and (in `borrowed_byte_buffer` only)
`capacity() <= std::span{r}.size_bytes()` on the range constructor.  Each sits in a
`#if defined(DEBUG)` block, so a release build contains no `__assert_fail` at all.  That is the
whole list: 7 per header (`front`, `back`, and `operator[]` on both of their overloads each,
plus the one in `unchecked_emplace_back`) and `borrowed_byte_buffer`'s range-constructor check,
for 29 in all.

**DESIGN.md's "Precondition checks under `-DDEBUG`" holds the reasoning**: why that set and no
other, why each unasserted `\pre` stays a promise, what `_GLIBCXX_DEBUG` adds, the ODR hazard,
and why `-fhardened` was rejected.  Read it before changing any of this.  It obliges you to do
the following here:

- **The `\pre` tags are the spec, and the asserts only enforce them.**  Adding an assert without
  a matching `\pre`, or asserting something stricter than the tag says, is how this drifts into
  contradicting the design.  In particular `operator[]` asserts `i < capacity()`, **not**
  `i < size()`.  Reading a live element at an index `>= size()` is intended, and all four suites
  do it on purpose, so tightening the assert would fail them.
- **Every `\pre` goes on both overloads' docs, not just the first.**  `front`, `back`, and
  `operator[]` state the tag once on the non-const overload, and the `const` twin gets a
  `/// \copydoc front()` (etc.).  `data()` and the `adopting` family follow the same convention.
- **New asserts go at the leaf.**  `unchecked_push_back` has none of its own, because its
  `!is_full()` check lives in the `unchecked_emplace_back` it delegates to, and
  `adopting(R&&, capacity)` inherits the range constructor's the same way.
- **Keep them `constexpr`-clean**, or `test-fixed_vector.cpp`'s `static_assert` blocks stop
  compiling under `-DDEBUG`.
- **Never link a `-DDEBUG` TU against a non-`DEBUG` one.**  That is an ODR violation, and
  `_GLIBCXX_DEBUG` is the same hazard one level down.  Both are why the debug build has its own
  binaries.

## Design invariants (the reason this isn't `std::inplace_vector`)

The header's class docstring lists the intended differences from `std::inplace_vector` /
`boost::static_vector`.  These are deliberate and drive the whole implementation, so do not
"fix" them toward standard-container semantics without cause:

- **All N elements are value-initialized at construction.**  Storage is a real
  `std::array<T, N>`, fully constructed up front, not raw aligned bytes.
- **Elements are never destroyed.**  `clear()`, `pop_back()`, and `resize()` only adjust the
  `size_` counter, and the underlying array elements stay alive.  Consequently the type is
  constrained to `std::is_trivially_destructible_v<T>` (enforced in the `requires` clause,
  along with `N > 0`, `default_initializable`, `movable`, and `Align` being a power of two
  that is at least `alignof(T)`).
- **`Align >= alignof(T)` is deliberate in `fixed_vector`, not redundant.**  Do not drop it as
  "already guaranteed by `alignas`".  It is a *diagnostic*: `alignas` cannot weaken natural
  alignment, so the array is safe either way, but a weakened `alignas` is ill-formed and GCC
  ignores it silently (Clang errors), so without the constraint `fixed_vector<int, 8, 1>` would
  compile and quietly ignore the request.  See DESIGN.md.  On `dynamic_fixed_vector` the same
  constraint is required for correctness, for a different reason.
- **`operator[]` is capacity-based and unchecked.**  It can legitimately read an initialized
  element at an index `>= size()` (this is tested intentionally).  `at()` is the only
  bounds-checked accessor.
- **`capacity()` is a run-time window over the `N` slots, moved by `reserve()`, and `max_size()`
  is `N`.**  `capacity_` starts at `N`, so nothing changes for a caller who never calls
  `reserve()`.  That equivalence is what keeps the feature invisible, so keep the default.  The
  rules below are all reasoned out in DESIGN.md, so do not "improve" one without it:
  - `reserve()` never (de)allocates, constructs, or destroys, since the whole `std::array<T, N>`
    is the storage regardless.  Growing therefore leaves the regained slots holding whatever
    they last held (scrubbing is `zeroize_*`'s job, always explicit).  Shrinking below `size()`
    truncates `size()` the same non-destructive way `clear()` / `pop_back()` / `resize()` do.
  - **`capacity()` is the single limit every mutator honors.**  `resize(count)` with
    `count > capacity()` throws `std::bad_alloc`.  It does **not** implicitly reserve, or
    `is_full()` / `reserved_unused()` would stop bounding every path in.  `reserve()` is the
    only member that changes the capacity.
  - `unreserved()` and `zeroize_unreserved()` cover `[capacity(), max_size())`, the half that
    `reserved_unused()` / `zeroize_reserved_unused()` do not.  A full scrub is `clear()` plus
    both zeroize calls.  `fill_capacity()` stops at `capacity()`, not `max_size()`.
  - `operator[]`'s `\pre` (and `-DDEBUG` assert) is the *current* `capacity()`, so a shrink puts
    the slots beyond it out of contract even though they are alive.
  - Three members are `fixed_vector`-only: `reserve()`, `unreserved()`, and
    `zeroize_unreserved()`.  (`reserved_unused()`, `zeroize_reserved_unused()`, and
    `fill_capacity()`, named above, are common to all four types.)  The heap-backed siblings'
    capacity is settled by the constructor, since moving it there would mean reallocating.

### `dynamic_fixed_vector` differences from `fixed_vector`

The invariants are the same, adapted for runtime capacity and heap storage.  All capacity
elements are alive up front (value-initialized by the reserve constructor, and constructed
directly from the source by the copy/fill/range constructors), none is ever destroyed, the
element type must be trivially destructible, and `operator[]` is unchecked.  The differences are
these:

- **Storage** is `std::unique_ptr<T, aligned_deleter>` over a block from
  `::operator new(bytes, std::align_val_t{Align})`, freed by the matching aligned
  `::operator delete`.  **Do not** rewrite this as an array `new`/`unique_ptr<T[]>`: the
  over-alignment comes from the `Align` template parameter, not from `T`, so `delete[]` would
  route to the non-aligned deallocation → UB.  The `allocate_` helper also guards
  `capacity * sizeof(T)` against `std::size_t` overflow (the language's array-new check is
  bypassed when you size the allocation yourself).
- **`Align >= alignof(T)` is in the `requires` clause and is required for correctness here**
  (unlike in `fixed_vector`, where it is only a diagnostic): the block is raw storage from the
  aligned `::operator new`, so nothing but the constraint prevents a smaller `Align` from
  under-aligning the elements → UB.  It is vacuous for `aligned_byte_buffer`
  (`alignof(std::byte) == 1`), which is why its clause is only `has_single_bit(Align)`.
- **`max_size()` is non-static** and equals `capacity()`, the runtime capacity, deliberately
  **not** a `SIZE_MAX`-ish value like `std::vector::max_size()`.  (In `fixed_vector`,
  `max_size()` is `static` and returns `N`, which `capacity()` may now be below.)  There is **no
  `reserve()`** here: capacity is fixed at construction, so moving it would mean reallocating.
- **`data()` applies `std::assume_aligned<Align>`** (guarded for the null/empty case) so caller
  loops can vectorize.
- **`X(n)` reserves capacity `n` and starts empty** (`size()==0`), unlike `fixed_vector`, where
  `X(count)` creates `count` elements.  Range / iterator-sentinel **constructors require
  forward** iterators (capacity must be computed up front).  Input-only sources use
  `X(capacity)` then `append_range`.  (`append_range` itself still accepts input iterators.)
- **Copy is a deep copy, and move construction transfers the pointer** and leaves the source
  empty (capacity 0).  Move assignment swaps, so the source keeps the target's former buffer
  until it is destroyed.  Copy/move assignment replace capacity too, while `assign_range` keeps
  the current capacity and throws `std::bad_alloc` if the source doesn't fit.
- **`constexpr`/`noexcept` are annotated like `fixed_vector`,** but over-aligned allocation is
  not usable in constant evaluation, so only empty/zero-capacity instances (and the
  non-allocating members) are usable in constant expressions.  The allocating paths sit behind a
  `capacity == 0` guard.  Each test has a `static_assert(constexpr_empty_ok())` verifying this.

### `aligned_byte_buffer` differences from `dynamic_fixed_vector`

The `std::byte` specialization keeps the same API and error-handling conventions but exploits
the fixed element type:

- **There is no `T` parameter,** only `Align` (power of two, default 16).  The `requires` clause
  reduces to `(std::has_single_bit(Align))`.
- **Allocation is `::operator new(capacity, std::align_val_t{Align})`,** with no overflow guard
  (`sizeof(std::byte) == 1`, so byte count == capacity).
- **Reserved capacity is left uninitialized:** lifetimes are begun with
  `std::start_lifetime_as_array<std::byte>` (no whole-capacity `memset`).  Bytes that enter
  `size()` are always written.  Reading beyond `size()` via `operator[]` yields an *unspecified*
  byte, which is **well-defined, not UB, for `std::byte`** (so the test does not assert a value
  there, unlike the `dynamic_fixed_vector` test).
- **Bulk ops are byte-grained.**  `std::memcpy` serves the span append fast path (non-overlap
  assumed), and `std::memset` serves `fill_*` / `resize`-grow.  The copy ctor copies only the
  live `[0,size)` bytes.  `operator==` / `operator<=>` are unconditional (`std::byte` is always
  comparable).
- **Zeroization, emplacement, and comparison also exploit the byte type.**
  `zeroize_reserved_unused()` (see API conventions) additionally turns the *unspecified*
  reserved tail into determinate zeros (lane padding).  `emplace_back` accepts at most one
  `std::byte`/integral argument (floats and other enums rejected).  The free function
  `equal_constant_time(span, span)` compares with no data-dependent branches, for
  secret-dependent data (e.g. tag verification).  Its accumulator is `volatile`, so it is
  `inline` rather than `constexpr`, and the container's `operator==` stays variable-time.

### `borrowed_byte_buffer` differences (the non-owning byte buffer)

It has the same API and conventions as `aligned_byte_buffer`, but it **borrows** storage rather
than owning it, which removes the ownership machinery and changes construction:

- **It is non-owning.**  The object is `{std::byte* data_, capacity_, size_}`.  It has no
  aligned `new`/`delete`, no `unique_ptr`, and no overflow guard, and it has a trivial
  destructor and all special members defaulted.  Copy/move are **shallow**: a copy aliases the
  same bytes, and move does **not** empty the source (the opposite of the heap types' move).
  The type is trivially copyable.  The caller keeps the borrowed storage alive.  A dangling
  source is UB the container cannot detect.
- **A read past `size()` returns the caller's bytes**, which is *not* `aligned_byte_buffer`'s
  situation even though neither container promises a value.  There the tail is heap storage
  never written, so the byte is genuinely arbitrary and only `std::byte`'s exemption from the
  indeterminate-value rules keeps the read out of UB.  Here it is memory the caller owns and
  probably initialized, so it is usually determinate, and it is the caller's data, which a
  beyond-size read can disclose.  Don't collapse the two back into one word.
  `zeroize_reserved_unused()` is the answer when the tail must not leak.  DESIGN.md's
  `borrowed_byte_buffer` section has the why.
- **There is no `Align` parameter.**  Borrowed memory carries no alignment promise, so `data()`
  returns the raw pointer.  Do **not** add `assume_aligned`.
- **Construction is over existing memory, not the shared owning constructors.**  Value
  constructors start empty (`size()==0`, scratch to build into).  The `adopting(...)` named
  constructors start full (`size()==capacity()`) to read bytes already present.  The range
  constructor takes a `contiguous_range` (bare `std::array` / `vector` / `span` / C array /
  `string`), **not** `std::span<T>`, because deduction will not convert an array to a span.  The
  single-object constructor takes a **forwarding reference** (`borrowable_object_ptr`), so a C
  array reaches the range constructor instead of decaying to a one-element `T*` view.
- **The two constraints, `borrowable_range` and `borrowable_object_ptr`, are namespace-scope
  concepts, not `bool` traits in the class.**  They sit above the class, after a forward
  declaration of it.  Concept conjunction short-circuits, which is what keeps `range_value_t`
  from being formed (and hard-erroring) for the non-range `T*` path.  A `constexpr bool`
  variable template instantiates every operand.  Do not "simplify" `borrowable_range`'s clauses:
  it excludes `borrowed_byte_buffer` itself (else a non-`const` buffer lvalue binds the range
  constructor over the copy constructor, as the less-cv-qualified reference, and reinterprets
  its own bytes), and it rejects rvalue owning containers (would dangle) and `const` elements
  (unwritable).
- **`data()` is *not* null-iff-capacity-0** here (a caller may borrow a zero-length region at a
  non-null address).  The mutating members' `!is_full()` / `!is_empty()` / `i < capacity()`
  preconditions still reach a non-null block via the constructor precondition that the source
  has at least `capacity()` writable bytes.
- **Only the default instance is usable in constant expressions.**  The borrowing constructors
  use `reinterpret_cast` (not usable in constant evaluation), and
  `static_assert(constexpr_empty_ok())` checks the default instance.
- **`equal_constant_time`** comes from `byte_compare.hpp` (shared with `aligned_byte_buffer`),
  not redefined here.

## API / error-handling conventions

- **A `\copydoc` / `\copydetails` target block must hold only what is true of every overload
  that copies it.**  That rules out a `\a param` naming a parameter the others do not have, and
  any claim that holds for only some of them.  The tag copies the text verbatim, nothing checks
  that the text still fits, and there is no Doxygen build here to catch it, so a target block is
  a promise that the overloads share one contract.  Three defects of exactly this shape were
  found and fixed in one pass: a `\copydetails` importing a `\pre` about a `capacity` parameter
  onto the constructor that has none, an `assign_range` post-condition
  (`leaves the container empty`) true only of the sized overloads, and that same block's
  `\pre \a spn` reaching four overloads with no `spn`.  Where the overloads genuinely differ,
  give each its own block.  The current targets are `data()`, `front()`, `back()`, `operator[]`,
  `at()`, `push_back`, `assign_range`, and `adopting`.
- Capacity overflow throws **`std::bad_alloc`** (not `length_error`), and `at()` throws
  **`std::out_of_range`**.  The `try_*` family (`try_push_back`, `try_emplace_back`,
  `try_append_range`) returns `bool` instead of throwing and is marked `[[nodiscard]]`.
- **A throwing member documents it with `\exception`.**  Doxygen's `\throw` and `\throws` are
  exact synonyms, so nothing in a build catches the difference.  The headers were converted to
  the one spelling and hold no instance of the other two.  Keep new tags on that spelling.
- `unchecked_*` variants skip the capacity check and assume `!is_full()`.  The checked
  `emplace_back`/`push_back`/`append_range` delegate to them after validating.
- Append overloads that can know the source size up front (span, iterator+count,
  `initializer_list`, sized ranges/sentinels) append nothing when they throw.  Truly unsized
  sources append element-wise and may partially append before throwing / returning `false`.
- `zeroize_reserved_unused()` (all four, for trivially copyable element types only) zeroizes the
  reserved tail with non-elidable stores (`memset_explicit`/`explicit_bzero` when the libc
  declares one, else a volatile-write fallback).  The libc functions are detected by
  *unqualified* name lookup, because there is no feature-test macro.

  Because that lookup is unqualified it needs the names in the global namespace, which is why
  every header includes **`<string.h>`**.  The byte buffers also include `<cstring>` for their
  `std::memcpy` / `std::memset` calls, and there the two are not redundant: **collapsing them to
  the C++ spelling is not a modernization**.  Collapsing them drops zeroization to the volatile
  fallback with no build error and no test failure, since all three branches zero correctly.
  (DESIGN.md has the why, next to the matching rule that the call must stay unqualified.)

  `clear()` + `zeroize_reserved_unused()` scrubs everything up to `capacity()`.  In
  `fixed_vector` it also works in constant evaluation (value-assigns the tail), and covering
  the whole container after a `reserve()` shrink additionally needs `zeroize_unreserved()`.
- Nearly the entire interface is `constexpr`.  `operator==` / `operator<=>` are gated on
  `std::equality_comparable` / `std::three_way_comparable`.
- `append_range` / `assign_range` are overloaded for span, iterator+sentinel, iterator+count,
  `initializer_list`, and arbitrary input ranges.  `assign_range` is `clear()` + `append_range`.
- **The input-range overload's dispatch to the `span` overload is not redundant.**  Do not
  collapse it as "the `span` overload already handles that".  A sized contiguous range of
  exactly the element type is forwarded there so it reaches the bulk copy.  Without that
  `if constexpr`, overload resolution picks the `R&&` template for `std::vector<T>` (exact
  match, vs. a user-defined conversion for the `span` overload) and the bulk path is dead code
  for every caller who does not hand-write a span.  It inherits the `span` overload's
  non-overlap `\pre` for that case.  Sized non-contiguous sources go through
  `unchecked_emplace_back`, since the up-front size check already covers every element.  See
  DESIGN.md.
