# Design notes

Engineering rationale for the fixed-capacity vector family in this repository. These notes are
implementation-agnostic and intended for anyone (human or tool) reviewing or extending the code.

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
- **`zeroize_remaining_space()`** (trivially copyable element types only) sets the object
  representation of the reserved tail `[size(), capacity())` to all-zero bytes without changing
  `size()`; `clear()` followed by it scrubs the whole container. The stores are guaranteed to
  happen even when nothing reads the tail afterward — a plain fill before deallocation is a dead
  store the optimizer may elide. The zeroing primitive is `memset_explicit` (C23 / C++26) or
  `explicit_bzero` (glibc ≥ 2.25, BSDs) when the C library declares one, else a volatile-write
  fallback; availability is detected with a requires-expression on a dependent call — neither
  function has a feature-test macro, and a `__cplusplus` check is useless (GCC 16 reports
  `202400L` even for `-std=c++26`, and it is the C library, not the language mode, that provides
  these functions; glibc declares both even at `-std=c++23`). During constant evaluation
  `fixed_vector` value-assigns the tail instead (there is no memory to scrub at compile time);
  the heap types are only ever empty in constant evaluation.

## `fixed_vector<T, N, Align>`

The baseline. Storage is a value-initialized `std::array<T, N>`; the object is self-contained (no
heap), copy/move are trivial, and essentially the entire interface is `constexpr`. Constraints:
`N > 0`, default-initializable, movable, trivially destructible, `Align` a power of two.

The default `Align`, `std::max(alignof(std::size_t), alignof(T))`, only ever *over*-aligns: even
for small `T` the array is at least word-aligned, and the `alignas` costs nothing because the
adjacent `std::size_t` member already gives the object that alignment.

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
  buffer). Appending a view over the buffer's own storage into itself is unsupported.

## Testing

There is no test framework. Each type has a standalone program that exercises every member function
with `assert`s and prints observable state; the program's expected stdout is embedded in a trailing
comment block so output regressions are catchable by diffing even when the asserts do not cover them.
A test passes when it exits 0. Because the runtime types hand-manage aligned memory (and the byte
buffer intentionally reads partially-uninitialized storage), the tests are also run under
AddressSanitizer + UndefinedBehaviorSanitizer; both are expected to be clean.
