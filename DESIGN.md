# Design notes

Engineering rationale for the fixed-capacity vector family in this repository. These notes are
implementation-agnostic and intended for anyone (human or tool) reviewing or extending the code.
See `README.md` for the API overview, a quick start for each container, and the build
instructions — this file covers only the *why*.

## The family

Three related containers, each a resizable sequence with a **fixed capacity** and
trivially-destructible elements. They share an API and a set of invariants; they differ in *when*
the capacity is fixed and *how* storage is obtained.

| Type | Capacity fixed at | Storage | Alignment |
|------|-------------------|---------|-----------|
| `fixed_vector<T, N, Align>` | compile time (`N`) | in-place `std::array<T, N>` | template param |
| `dynamic_fixed_vector<T, Align>` | run time (constructor) | one aligned heap block | template param |
| `aligned_byte_buffer<Align>` | run time (constructor) | one aligned heap block | template param, element type fixed to `std::byte` |

`dynamic_fixed_vector` is the runtime-capacity analogue of `fixed_vector`; `aligned_byte_buffer` is
the `std::byte` specialization of `dynamic_fixed_vector`, kept as a separate type so it can be
simpler and faster.

## Shared invariants

These are deliberate and hold across all three types:

- **All capacity slots are alive up front.** Capacity storage is not raw bytes waiting for placement
  construction; every slot holds a live element from the moment the container exists.
- **Elements are never individually destroyed.** `clear()`, `pop_back()`, and `resize()` only adjust
  the size counter. This is why the element type is constrained to be trivially destructible.
  A consequence: `emplace_back` cannot construct in place — the target slot already holds a live
  element — so it constructs a temporary from its arguments and assigns it into the slot,
  equivalent to `push_back(T(args...))` (kept for API parity with `std::inplace_vector`).
- **`operator[]` is unchecked and capacity-based.** It may legitimately access a slot at an index
  `>= size()` (within capacity). `at()` is the only bounds-checked accessor and throws
  `std::out_of_range`.
- **Capacity overflow throws `std::bad_alloc`** (not `std::length_error`).
- **Three method families for adding elements:**
  - checked (`push_back` / `emplace_back` / `append_range`) — validate, then throw on overflow;
  - `try_*` (`[[nodiscard]] bool`) — validate, return `false` on overflow instead of throwing;
  - `unchecked_*` — assume `!is_full()` / sufficient space; the checked forms delegate to these.
- **`capacity()` / `max_size()` report the fixed capacity.** For the runtime types this is the value
  passed to the constructor — intentionally *not* a `SIZE_MAX`-style theoretical maximum.
- **`append_range` / `assign_range`** are overloaded for span, iterator+sentinel, iterator+count,
  `initializer_list`, and input ranges; `assign_range` is `clear()` followed by `append_range`.
  Overloads that can know the source size up front (span, iterator+count, `initializer_list`,
  sized ranges, sized sentinels) validate before writing and are all-or-nothing; truly unsized
  sources append element-wise, so an overflowing append may add the elements that fit before
  throwing (or returning `false` from `try_*`).
- **The input-range overload forwards a sized contiguous range of exactly the element type to the
  span overload**, so an ordinary `std::vector<T>` / `std::array<T, N>` lands on the bulk copy
  (`std::ranges::copy`, or `memcpy` in the byte buffer) instead of being appended element-wise.
  The dispatch is explicit because overload resolution cannot reach that conclusion: for
  `std::vector<T>` the `R&&` template is an *exact match* while the span overload needs a
  user-defined conversion, so the template always wins and the bulk path would be reachable only
  by spelling the span out at the call site — which no caller should have to know to do. The
  element type must match exactly; a contiguous range of some *other* type (`std::vector<int>`
  into a byte buffer) still converts element by element, as it must. Sized-but-not-contiguous
  sources append through `unchecked_emplace_back`, the up-front size check having already covered
  every element, so a `sized_range` that misreports its size trips that family's `!is_full()`
  assert under `-DDEBUG` rather than throwing.
- **`zeroize_remaining_space()`** (trivially copyable element types only) sets the object
  representation of the reserved tail `[size(), capacity())` to all-zero bytes without changing
  `size()`; `clear()` followed by it scrubs the whole container. The stores are guaranteed to
  happen even when nothing reads the tail afterward — a plain fill before deallocation is a dead
  store the optimizer may elide. The zeroing primitive is `::memset_explicit` (C23) or
  `explicit_bzero` (glibc ≥ 2.25, BSDs) when the C library declares one, else a volatile-write
  fallback; availability is detected with a requires-expression on a dependent call — neither
  function has a feature-test macro, and a `__cplusplus` check is useless: it is the C library,
  not the language mode, that provides them. glibc declares both under `__USE_MISC`, which the
  `_GNU_SOURCE` that g++ defines at every `-std` turns on, so the selection does not move with
  the language standard.

  **Why the call is unqualified, and why it must stay that way.** It is `::memset_explicit`
  (C23), not `std::memset_explicit` (C++26) — and that is not a shortcut, it is the only form
  that works. libstdc++ 16 does not define `std::memset_explicit` at any `-std`, and there is
  no way to ask whether it exists: [SD-6][sd6] lists no feature-test macro for it, and a
  requires-expression cannot substitute for one, because a qualified name into a namespace that
  lacks the member is a hard error at template definition rather than a substitution failure.
  So `requires { std::memset_explicit(...); }` does not evaluate to `false` and fall through —
  it fails the build, taking the `explicit_bzero` and volatile branches down with it. The
  unqualified name is the only spelling this detection idiom can probe. Nothing is lost by it:
  the project targets Linux/glibc, where C23 guarantees `::memset_explicit`, and when libstdc++
  eventually adds the `std::` name it will be a using-declaration for this same function.

  [sd6]: https://isocpp.org/std/standing-documents/sd-6-sg10-feature-test-recommendations

  During constant evaluation
  `fixed_vector` value-assigns the tail instead (there is no memory to scrub at compile time);
  the heap types are only ever empty in constant evaluation.

## `fixed_vector<T, N, Align>`

The baseline. Storage is a value-initialized `std::array<T, N>`; the object is self-contained (no
heap), copy/move are member-wise — and trivial for trivially copyable `T`, the intended case —
and essentially the entire interface is `constexpr`. Moving does
*not* empty the source: the defaulted move is member-wise, so for trivially copyable `T` a
moved-from `fixed_vector` is left unchanged — unlike the heap-backed siblings, where move
construction empties the source. Constraints:
`N > 0`, default-initializable, movable, trivially destructible, `Align` a power of two and at
least `alignof(T)`.

The default `Align`, `std::max(alignof(std::size_t), alignof(T))`, only ever *over*-aligns: even
for small `T` the array is at least word-aligned, and the `alignas` costs nothing because the
adjacent `std::size_t` member already gives the object that alignment.

The `Align >= alignof(T)` bound is **not** needed for correctness here, unlike on the heap-backed
siblings: storage is a real `std::array<T, N>` member, and `alignas` cannot weaken a type's
natural alignment, so the array is suitably aligned even for an under-aligned request. The bound
exists for diagnosis. A weakened `alignas` is ill-formed ([dcl.align]/5), but GCC accepts it
silently at any warning level (Clang rejects it), so without the constraint
`fixed_vector<int, 8, 1>` compiles, ignores the request, and yields a 4-aligned array — an intent
the container cannot honor, expressed with no diagnostic, in code that builds on only one
compiler. The constraint turns that into a named constraint failure, and makes the family's three
`requires` clauses consistent.

## `dynamic_fixed_vector<T, Align>`

Same shape as `fixed_vector`, but capacity is a constructor argument and storage is a single
over-alignable heap block.

- **Allocation.** Storage comes from the aligned allocation function
  `::operator new(bytes, std::align_val_t{Align})`, owned by a `std::unique_ptr<T, Deleter>` whose
  stateless deleter calls the matching `::operator delete(p, std::align_val_t{Align})`. Every
  capacity element's lifetime is begun exactly once at construction: the reserve constructor
  value-initializes them (`std::uninitialized_value_construct_n`), while the copy/fill/range
  constructors construct them directly from the source (`std::uninitialized_copy_n` /
  `std::uninitialized_fill_n` / `std::construct_at`), avoiding a value-initialize-then-overwrite
  double write. The block is adopted by the `unique_ptr` *before* element construction so a
  throwing element constructor still frees it (the constructed elements need no destruction —
  `T` is trivially destructible).

- **Why not an array `new` expression** (`new (std::align_val_t{Align}) T[n]`). The over-alignment
  is supplied by the `Align` *template parameter*, not by the type `T`. A `delete[]` expression (and
  the default deleter of `unique_ptr<T[]>`) selects the aligned vs. plain deallocation function from
  the *type* — whether `alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__` — so for a normally-aligned
  `T` it would call the **non-aligned** `operator delete[]`, mismatching the aligned allocation
  (undefined behavior), and there is no syntax to make `delete[]` pass the alignment. Array `new`
  may also insert an implementation-defined element-count cookie that offsets the pointer and
  inflates the request. Using the raw allocation/deallocation functions with an explicit
  `align_val_t` avoids both problems and cleanly separates *storage* from *construction*.

- **Overflow guard.** Because the allocation size is computed by hand (`capacity * sizeof(T)`), the
  language's array-`new` overflow check does not apply, so `capacity > SIZE_MAX / sizeof(T)` is
  checked explicitly before allocating.

- **`Align >= alignof(T)` is required for correctness.** The `requires` clause demands it (alongside
  power-of-two), and here — unlike `fixed_vector`, where `alignas` would keep the array naturally
  aligned regardless — nothing else would enforce it: the block is raw storage from the aligned
  `::operator new`, so a smaller `Align` would begin element lifetimes at an under-aligned address,
  which is undefined behavior. `aligned_byte_buffer` needs no such constraint because
  `alignof(std::byte)` is 1, so every power of two satisfies it; that is why its clause reduces to
  `has_single_bit(Align)`.

- **Default alignment.** `Align` defaults to `std::max(alignof(std::size_t), alignof(T))`, matching
  `fixed_vector`: the storage is never under-aligned, and word alignment for small `T` is free —
  any alignment up to `__STDCPP_DEFAULT_NEW_ALIGNMENT__` costs the aligned allocator nothing.
  (`aligned_byte_buffer` instead defaults to 16, its SIMD-lane motivation.)

- **`data()` asserts alignment to the compiler.** It returns `std::assume_aligned<Align>(ptr)`
  (guarded for the null/empty case, whose precondition needs a real aligned pointer) so caller loops
  can vectorize on the known alignment. `begin() == data()` is aligned; `end() = data() + size()` is
  not claimed to be.

- **Constructing from a range/iterators requires forward iterators.** Capacity must be known before
  allocation, so the range and iterator+sentinel *constructors* require forward iterators/ranges
  (capacity = distance). Input-only sources are handled by constructing with an explicit capacity
  and then calling `append_range` (which still accepts input iterators).

- **Value semantics.** Copy performs a deep copy (independent buffer). Move *construction* transfers
  the pointer and leaves the source empty (capacity 0); move *assignment* swaps, so the source is
  left holding the target's former buffer until it is destroyed. Copy/move assignment replace the
  capacity too; `assign_range`
  keeps the current capacity and throws if the source does not fit. Copy-and-swap gives assignment a
  strong guarantee.

- **`constexpr` interface, limited compile-time use.** The members are annotated `constexpr`
  (matching `fixed_vector`), so empty / zero-capacity instances and the non-allocating members
  are usable in constant expressions. Instances with capacity `> 0` cannot be constructed at
  compile time, because over-aligned allocation (the raw aligned `operator new`) is not usable
  during constant evaluation; those code paths sit behind a `capacity == 0` guard and are simply
  never taken by a constant evaluation. `noexcept` follows `fixed_vector`'s placement (observers,
  element access, iterators, `clear`, `pop_back`, move, `swap`); allocating/throwing members are
  not `noexcept`.

## `aligned_byte_buffer<Align>`

The `std::byte` specialization of `dynamic_fixed_vector`. The element type is fixed, so only the
alignment is a template parameter (a power of two, default 16). Distinct alignments are distinct
types. The API and conventions are unchanged; the element type enables these differences:

- **No overflow guard.** `sizeof(std::byte) == 1`, so the byte count equals the capacity — there is
  no multiplication and nothing to overflow.

- **Reserved capacity is left uninitialized.** Rather than value-initializing (zeroing) every byte up
  front, the block's object lifetimes are begun with `std::start_lifetime_as_array<std::byte>` and
  the reserved tail is not written. Bytes that enter `size()` (via `push_back` / `append` / `resize`
  / `fill_*`) are always written first. Reading beyond `size()` through `operator[]` therefore yields
  an **unspecified** byte value — which is well-defined, *not* undefined behavior, because `std::byte`
  (like `unsigned char`) is exempt from the indeterminate-value rules. This trades a strict
  "beyond-size reads as zero" guarantee for avoiding an O(capacity) zeroing at construction, which
  matters for large scratch/IO buffers.

- **Byte-granular bulk operations.** `std::memcpy` for the span append fast path (assuming the source
  does not overlap the buffer), `std::memset` for `fill_capacity` / `fill_size` / `resize`-grow. The
  copy constructor copies only the live `[0, size())` bytes, since the reserved tail is unspecified
  anyway. `operator==` / `operator<=>` are unconditional (`std::byte` is always comparable, yielding
  `std::strong_ordering`). Note `memcpy`/`memset` are not `constexpr`; like the allocation they sit
  behind an emptiness guard, so the `constexpr` annotation still holds for the empty case.

- **Explicit zeroization** (see shared invariants): for the byte buffer,
  `zeroize_remaining_space()` also turns the otherwise *unspecified* reserved tail into
  determinate zeros — pad to an alignment boundary before whole-lane SIMD reads past `size()`,
  and stale heap bytes cannot leak through beyond-size reads.

- **`constant_time_equal(span, span)`** — a namespace-scope helper, deliberately *not* used by the
  container: it OR-accumulates the XORed byte pairs with no data-dependent branch or early exit,
  so its timing depends only on the (normally public) lengths. Use it for secret-dependent
  comparisons — e.g. MAC/tag verification — where `operator==`'s first-mismatch early exit leaks
  the position of the first differing byte through timing; the container's own comparisons stay
  variable-time, per ordinary container semantics.

  The guarantee is *unenforced*, which is a real limitation and not an oversight. Branch-freedom
  holds in the source, but the standard has no notion of timing, so nothing forbids a compiler
  from proving the accumulator is monotone and short-circuiting the loop. Note this cuts the
  opposite way from `zeroize_remaining_space()`, which distrusts the optimizer and pays for
  `memset_explicit` to defeat it. The difference is that a dead-store elision is *routine* —
  compilers do it constantly, so the mitigation earns its cost — whereas short-circuiting an
  OR-accumulation is a transformation no production compiler is known to make, and the available
  defenses (an `asm` barrier, a volatile accumulator) are non-portable, un-`constexpr`, and would
  block the vectorization that currently makes this fast. So the choice is: write the standard
  idiom, verify the codegen, and record the verification. Checked for GCC 16 at `-O3
  -march=native` — the loop vectorizes to a `vpxor`/`vpor` accumulation with a horizontal reduce,
  and every surviving conditional branch tests a size rather than a content byte. That check is
  tied to a compiler and flags, so it is worth repeating if either moves.

- **`constexpr` / `noexcept`** follow `dynamic_fixed_vector` (see above): empty instances are usable
  in constant expressions; capacity `> 0` requires a runtime allocation.

- **Motivating use.** A contiguous `std::byte` buffer aligned like a SIMD lane (e.g. 16 bytes),
  filled incrementally, then read as `std::span<const std::byte>` and handed to intrinsics.

## Edge cases and gotchas

- **Zero capacity is both empty and full.** For the runtime types a default-constructed (or
  zero-capacity) instance satisfies `is_empty()` and `is_full()` simultaneously — correct, since there
  is no element and no remaining space. (`fixed_vector` cannot reach this state; it requires `N > 0`.)

- **`std::byte` is not `std::constructible_from` an `int`.** Direct-initialization of a scoped
  enumeration from an integer (`std::byte b(42)`) is ill-formed, so a `constructible_from`
  constraint would reject the integer arguments that the functional cast `std::byte(42)` in the
  `emplace_back` body handles fine. `aligned_byte_buffer`'s `emplace_back` family is therefore
  constrained by argument *type* instead: at most one argument, of type `std::byte` or an integral
  type. This deliberately rejects floating-point arguments (an expression-validity constraint
  would have accepted `emplace_back(3.99)` and stored `byte{3}`, since the cast follows
  `static_cast` rules) and other enumeration types (cast explicitly, e.g. with
  `std::to_underlying`, if intended). What no constraint can reject is an out-of-range *runtime*
  integer: `emplace_back(256)` stores `byte{0}`.

- **`append_range(span)` assumes the source does not alias the buffer** (it uses `memcpy` in the byte
  buffer). Appending a view over the buffer's own storage into itself is unsupported. The
  assumption reaches past the span overloads themselves: a contiguous range of the element type is
  forwarded to them (see above), so `append_range(rg)` carries the same tag for that case.

## Testing

See `README.md` for the test inventory and the commands to run it. Three of the choices it
describes are deliberate rather than incidental; this is the reasoning behind them.

- **The exit status is the entire contract: silence and 0 on success, a message and non-zero on
  failure.** These programs previously printed a running commentary of container state and
  embedded the expected stdout in a trailing comment block, as a second and coarser net: diffing
  a run against the block caught changes to observable state that no assertion mentioned. It was
  dropped because the cost outgrew that benefit. The block had to be regenerated from a real run
  after every change, which makes it as likely to record a regression as to catch one; it made
  pass/fail mean "exit 0 *and* diff the output", which a `make test` target cannot express; and
  it was noisy in the way that matters least — most of the churn came from `sizeof` and capacity
  numbers that no invariant depends on. What replaces it is coverage: every member, both
  overloads, checked explicitly rather than watched from a distance.

- **Checks are `CHECK`, never `assert`.** `assert` fires `abort()`, which dumps a core file for
  what is only a failed comparison. `CHECK` prints `file:line: function: CHECK failed: <expr>`
  and calls `std::exit(EXIT_FAILURE)`, so a failing test leaves nothing behind to clean up. The
  same reasoning drives `run_tests`, which catches every exception: one escaping `main` would
  reach `terminate()` and abort just the same. `CHECK_THROWS` distinguishes "threw nothing" from
  "threw the wrong type", because those are different defects.

- **`fixed_vector`'s suite drives the container at compile time.** Nearly its whole interface is
  `constexpr`, so a `static_assert` block ahead of `main()` runs a vector through `append_range`,
  `push_back`, `emplace_back`, `try_push_back`, `assign_range`, `resize`, `swap` and
  `zeroize_remaining_space` during constant evaluation. That moves a regression from a failed run
  to a failed build, and buys one check the run-time net cannot: an index outside the storage is
  rejected outright there. `operator[]` is unchecked by design, so at run time `v[10]` on a
  `fixed_vector<int, 5>` reads whatever is past the array and every `CHECK` still passes; in a
  `static_assert` the same expression fails to compile (GCC 16.1.1 rejects it through
  `std::array`'s hardened precondition). The heap-backed types cannot be tested this way, because
  over-aligned allocation is not usable in constant evaluation; their suites reach only the empty
  and zero-capacity members, which is why the compile-time coverage is so lopsided across the
  three.

- **The heap-backed types are also run under AddressSanitizer + UndefinedBehaviorSanitizer.**
  `CHECK`s are blind to the specific mistakes these two can make: a read or write just past the
  block, a leaked buffer, or an aligned `::operator new` paired with the wrong deallocation
  function — the array-`new` trap described above — all leave every check passing and the
  program exiting 0. ASan sees them, and reports that last one as `new-delete-type-mismatch`,
  naming the allocated and the deallocated alignment. `aligned_byte_buffer` adds a wrinkle: it
  reads beyond `size()` on purpose, which is legitimate inside the allocation and must not be
  allowed to shade into straying outside it. Both suites are expected to be clean.
