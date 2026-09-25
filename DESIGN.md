# Design notes

Engineering rationale for the fixed-capacity vector family in this repository.  These notes are
implementation-agnostic and intended for anyone (human or tool) reviewing or extending the code.
See [`README.md`](README.md) for the API overview, a quick start for each container, and the
build instructions.  This file covers only the *why*.

## The family

The family is four related containers, each a resizable sequence with a **fixed capacity** and
trivially destructible elements.  They share an API and a set of invariants, and they differ in
*when* the capacity is fixed and *how* storage is obtained.

| Type | Capacity fixed at | Storage | Alignment |
|------|-------------------|---------|-----------|
| `fixed_vector<T, N, Align>` | compile time (`N`) | in-place `std::array<T, N>` | template param |
| `dynamic_fixed_vector<T, Align>` | run time (constructor) | one aligned heap block | template param |
| `aligned_byte_buffer<Align>` | run time (constructor) | one aligned heap block | template param, element type fixed to `std::byte` |
| `borrowed_byte_buffer` | run time (constructor) | borrowed — non-owning view | none (borrowed memory carries no promise) |

`dynamic_fixed_vector` is the runtime-capacity analogue of `fixed_vector`.
`aligned_byte_buffer` is the `std::byte` specialization of `dynamic_fixed_vector`, kept as a
separate type so it can be simpler and faster.  `borrowed_byte_buffer` is the **non-owning**
counterpart to `aligned_byte_buffer`: the same byte-buffer interface over storage it does not
own.  It allocates nothing, so it drops the whole ownership apparatus (aligned `new`/`delete`,
the `unique_ptr`, deep copy, the `Align` parameter) and keeps only the size-cursor logic.  The
two byte buffers share `equal_constant_time`, factored into `byte_compare.hpp`.

## Shared invariants

These invariants are deliberate and hold across all four types, except where the
`borrowed_byte_buffer` section notes otherwise.  Being non-owning, that type departs on storage
lifetime, on copy/move semantics, and on the "all slots alive up front" point.  It inherits
whatever the borrowed region already held, so the container promises nothing about a read past
`size()`, for a different reason than `aligned_byte_buffer` does (see "Beyond-`size()` reads
expose the caller's bytes" below).

The invariants are these:

- **All capacity slots are alive up front.**  Capacity storage is not raw bytes waiting for
  placement construction.  Every slot holds a live element from the moment the container exists.
- **Elements are never individually destroyed.**  `clear()`, `pop_back()`, and `resize()` only
  adjust the size counter.  This is why the element type is constrained to be trivially
  destructible.  One consequence is that `emplace_back` cannot construct in place, because the
  target slot already holds a live element.  It constructs a temporary from its arguments and
  assigns it into the slot instead, which is equivalent to `push_back(T(args...))`.  It is kept
  for API parity with `std::inplace_vector`.
- **`operator[]` is unchecked and capacity-based.**  It may legitimately access a slot at an
  index `>= size()` (within capacity).  `at()` is the only bounds-checked accessor and throws
  `std::out_of_range`.
- **Capacity overflow throws `std::bad_alloc`** (not `std::length_error`).
- **There are three method families for adding elements:**
  - The checked ones (`push_back` / `emplace_back` / `append_range`) validate, then throw on
    overflow.
  - The `try_*` ones (`[[nodiscard]] bool`) validate, then return `false` on overflow instead of
    throwing.
  - The `unchecked_*` ones assume `!is_full()` or sufficient space.  The checked forms delegate
    to these.
- **`capacity()` reports a real capacity, not a theoretical one.**  For the runtime types it is
  the value passed to the constructor — intentionally *not* a `SIZE_MAX`-style maximum, which is
  what `std::vector::max_size()` reports.  `max_size()` equals `capacity()` in three of the four
  types.  In `fixed_vector` it is `N`, the number of slots, and `capacity()` is a run-time
  window within it that `reserve()` moves (see that section).
- **`append_range` / `assign_range`** are overloaded for span, iterator+sentinel,
  iterator+count, `initializer_list`, and input ranges.  `assign_range` is `clear()` followed by
  `append_range`.  Overloads that can know the source size up front (span, iterator+count,
  `initializer_list`, sized ranges, sized sentinels) validate before writing, so nothing is
  appended when they throw.  Truly unsized sources append element-wise, so an overflowing append
  may add the elements that fit before throwing (or returning `false` from `try_*`).
- **The input-range overload forwards a sized contiguous range of exactly the element type to
  the span overload**, so an ordinary `std::vector<T>` / `std::array<T, N>` lands on the bulk
  copy (`std::ranges::copy`, or `memcpy` in the byte buffer) instead of being appended
  element-wise.  The dispatch is explicit because overload resolution cannot reach that
  conclusion.  For `std::vector<T>` the `R&&` template is an *exact match*, while the span
  overload needs a user-defined conversion.  The template therefore always wins, and without the
  dispatch the bulk path would be reachable only by spelling the span out at the call site,
  which no caller should have to know to do.

  The element type must match exactly.  A contiguous range of some *other* type
  (`std::vector<int>` into a byte buffer) still converts element by element, as it must.
  Sized-but-not-contiguous sources append through `unchecked_emplace_back`, since the up-front
  size check has already covered every element.  A `sized_range` that misreports its size
  therefore trips that family's `!is_full()` assert under `-DDEBUG` rather than throwing.
- **`zeroize_reserved_unused()`** (trivially copyable element types only) sets the object
  representation of the reserved tail `[size(), capacity())` to all-zero bytes without changing
  `size()`.  `clear()` followed by it scrubs the whole container.  The stores are guaranteed to
  happen even when nothing reads the tail afterward, whereas a plain fill before deallocation is
  a dead store the optimizer may elide.

  The zeroing primitive is `::memset_explicit` (C23) or `explicit_bzero` (glibc ≥ 2.25, BSDs)
  when the C library declares one, else a volatile-write fallback.  Availability is detected
  with a requires-expression on a dependent call, because neither function has a feature-test
  macro.  A `__cplusplus` check is useless too, since the C library provides them, not the
  language mode.  glibc declares both under `__USE_MISC`, which the `_GNU_SOURCE` that g++
  defines at every `-std` turns on, so the selection does not move with the language standard.

  **Why the call is unqualified, and why it must stay that way.**  It is `::memset_explicit`
  (C23), not `std::memset_explicit` (C++26), and that is not a shortcut but the only form that
  works.  libstdc++ 16 does not define `std::memset_explicit` at any `-std`, and there is no way
  to ask whether it exists.  [SD-6][sd6] lists no feature-test macro for it.  A
  requires-expression cannot substitute for one either, because a qualified name into a
  namespace that lacks the member is a hard error at template definition rather than a
  substitution failure.

  So `requires { std::memset_explicit(...); }` does not evaluate to `false` and fall through.
  It fails the build, taking the `explicit_bzero` and volatile branches down with it.  The
  unqualified name is the only spelling this detection idiom can probe.  Nothing is lost by it:
  the project targets Linux/glibc, where C23 guarantees `::memset_explicit`, and when libstdc++
  eventually adds the `std::` name it will be a using-declaration for this same function.

  [sd6]: https://isocpp.org/std/standing-documents/sd-6-sg10-feature-test-recommendations

  **Why the headers include `<string.h>` and not just `<cstring>`.**  Both names are declared by
  `<string.h>`, and that include sits alongside the `<cstring>` the qualified `std::memcpy` /
  `std::memset` calls need.  The two are not redundant, and collapsing them to the C++ spelling
  alone is not a modernization.

  Since the probe above must be unqualified, it needs the names in the *global* namespace, which
  `<cstring>` does not supply.  `[cstring]` guarantees only the `std::` names, and neither
  `memset_explicit` nor `explicit_bzero` is among the ones it is required to declare at all (the
  first is C23, the second a glibc/BSD extension).  That they have been reachable through
  `<cstring>` on glibc is an implementation detail of that header including `<string.h>`, not
  something to depend on.

  This is worth stating because the failure mode is silent.  Drop the `<string.h>` on an
  implementation whose `<cstring>` does not re-export, and the requires-expression simply
  evaluates to `false` and takes the volatile-write fallback.  There is no build error and no
  test failure, because all three branches zero the memory correctly and the suites cannot tell
  them apart.  The only casualty is the guarantee, and nothing announces its loss.

  During constant evaluation `fixed_vector` value-assigns the tail instead, since there is no
  memory to scrub at compile time.  The heap types are only ever empty in constant evaluation.

## `fixed_vector<T, N, Align>`

`fixed_vector` is the baseline.  Storage is a value-initialized `std::array<T, N>`, and the
object is self-contained (no heap).  Copy and move are member-wise, and trivial for trivially
copyable `T`, the intended case.  Essentially the entire interface is `constexpr`.

Moving does *not* empty the source.  The defaulted move is member-wise, so for trivially
copyable `T` a moved-from `fixed_vector` is left unchanged, unlike the heap-backed siblings,
where move construction empties the source.  The constraints are `N > 0`, default-initializable,
movable, trivially destructible, and `Align` a power of two and at least `alignof(T)`.

The default `Align`, `std::max(alignof(std::size_t), alignof(T))`, only ever *over*-aligns: even
for small `T` the array is at least word-aligned, and the `alignas` costs nothing because the
adjacent `std::size_t` member already gives the object that alignment.

The `Align >= alignof(T)` bound is **not** needed for correctness here, unlike on the
heap-backed siblings: storage is a real `std::array<T, N>` member, and `alignas` cannot weaken a
type's natural alignment, so the array is suitably aligned even for an under-aligned request.
The bound exists for diagnosis.  A weakened `alignas` is ill-formed ([dcl.align]/5), but GCC
accepts it silently at any warning level (Clang rejects it), so without the constraint
`fixed_vector<int, 8, 1>` compiles, ignores the request, and yields a 4-aligned array — an
intent the container cannot honor, expressed with no diagnostic, in code that builds on only one
compiler.  The constraint turns that into a named constraint failure, and makes the family's
three `requires` clauses consistent.

### Capacity is a run-time window over the `N` slots

`N` sizes the storage, but it is *not* the capacity.  `capacity_` is a `std::size_t` member
starting at `N`, and `reserve()` moves it anywhere in `[0, N]`.  Every space check —
`is_full()`, `reserved_unused()`, the whole append family, `resize()` — already consulted
`capacity()`, so lowering it narrows the window the container will use, while the storage stays
the same `std::array<T, N>`.  `max_size()` remains `static constexpr` and reports `N`.

The window costs almost nothing, and each decision below follows from that:

- **`reserve()` never (de)allocates and is O(1).**  There is no reallocation to perform and no
  element to construct or destroy — the slots on both sides of the capacity line are alive
  either way.  That is what makes it reasonable for `reserve()` to *shrink*, which
  `std::vector::reserve` cannot do (it would have to reallocate, which is what the non-binding
  `shrink_to_fit` is for).
- **Growing leaves the regained slots untouched.**  A slot's value is whatever was last written
  to it, `T{}` if never written — exactly the rule that already governs the reserved tail
  (`fill_capacity(9); resize(2)` leaves `9`s in `[2, capacity())`, which the tests check).
  Nothing is gained by re-value-initializing on grow: it would cost O(delta) on a member whose
  entire point is that it moves a cursor, and it would half-do a job `zeroize_reserved_unused()`
  / `fill_capacity()` already do properly and on request.  Scrubbing is an explicit, separately
  named operation in this library, not a side effect.
- **Shrinking below `size()` truncates `size()`.**  Throwing instead would make `reserve()` the
  only capacity operation with a precondition on `size()`.  The container already shrinks
  non-destructively everywhere else (`clear()`, `pop_back()`, and `resize()` all just move the
  size counter and leave the elements alive), and truncation is that same move.  The elements
  are not lost, only outside the window, and growing the capacity back exposes them again.  That
  is precisely what makes "growing leaves the slots untouched" observable, and it is how the
  tests observe it.
- **`resize()` throws instead of implicitly reserving.**  `capacity()` is the single limit every
  mutator honors.  If `resize(count)` quietly raised it, `is_full()` and `reserved_unused()`
  would no longer bound every path into the container, and there would be two members that
  change the capacity, one of them by accident.  So `resize(count)` with `count > capacity()`
  throws `std::bad_alloc`, like every other capacity overflow, and `reserve()` stays the only
  way to move the line.
- **`zeroize_unreserved()` is a separate member from `zeroize_reserved_unused()`**, covering
  `[capacity(), max_size())` rather than `[size(), capacity())`.  Each name states exactly the
  region it scrubs, and the split is real: after a shrink, the unreserved slots may still hold
  what they held while they were reserved.  The full scrub is `clear()`, then both calls.
  Folding them into one member covering `[size(), max_size())` would give the byte-level cost of
  the whole array to every caller who only wanted the tail.
- **`fill_capacity()` fills exactly `[0, capacity())`.**  It stops at `capacity()`, not
  `max_size()`, because it fills the window, as its name says.  That is why it is
  `std::ranges::fill` over `[0, capacity())` rather than `std::array::fill`.  It also starts at
  0, so the live elements are overwritten along with the reserved tail.  The name does not carry
  that second half (it says how far the fill reaches, not where it begins), and it invites the
  reading "fill the unused part", so the docs on all four types state the range outright.

  The tail-only operation does exist, spelled `resize(capacity(), value)`.  It assigns `value`
  to `[size(), capacity())` and sets `size()` to `capacity()`, which is precisely what a
  `resize_capacity(value)` member would do.  Adding that member would give a second name to an
  existing one-liner, and the name would read like a capacity *changer* next to `reserve()`, the
  member that actually is one.  The three fill regions are covered without it: `fill_size()` for
  `[0, size())`, `resize(capacity(), value)` for `[size(), capacity())`, and `fill_capacity()`
  for both.

`operator[]`'s `\pre i < capacity()` therefore tightens when the capacity is lowered: a slot in
`[capacity(), max_size())` is alive but out of contract, and the `-DDEBUG` assert says so.  That
is the point of stating the precondition in terms of `capacity()` rather than `N`.

The default capacity is `N`, and this is what keeps the feature invisible to existing code: a
program that never calls `reserve()` sees exactly the previous behavior, because every check
that used to read `N` now reads a value that is still `N`.

## `dynamic_fixed_vector<T, Align>`

It has the same shape as `fixed_vector`, but capacity is a constructor argument and storage is a
single over-alignable heap block.

- **Allocation.**  Storage comes from the aligned allocation function
  `::operator new(bytes, std::align_val_t{Align})`, owned by a `std::unique_ptr<T, Deleter>`
  whose stateless deleter calls the matching `::operator delete(p, std::align_val_t{Align})`.
  Every capacity element's lifetime is begun exactly once at construction: the reserve
  constructor value-initializes them (`std::uninitialized_value_construct_n`), while the
  copy/fill/range constructors construct them directly from the source
  (`std::uninitialized_copy_n` / `std::uninitialized_fill_n` / `std::construct_at`), avoiding a
  value-initialize-then-overwrite double write.

  The block is adopted by the `unique_ptr` *before* element construction, so a throwing element
  constructor still frees it.  The constructed elements need no destruction, since `T` is
  trivially destructible.

- **Why not an array `new` expression** (`new (std::align_val_t{Align}) T[n]`).  The
  over-alignment is supplied by the `Align` *template parameter*, not by the type `T`.  A
  `delete[]` expression (and the default deleter of `unique_ptr<T[]>`) selects the aligned vs.
  plain deallocation function from the *type* (whether
  `alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__`).  So for a normally aligned `T` it would call
  the **non-aligned** `operator delete[]`, mismatching the aligned allocation (undefined
  behavior), and there is no syntax to make `delete[]` pass the alignment.

  Array `new` may also insert an implementation-defined element-count cookie that offsets the
  pointer and inflates the request.  Using the raw allocation/deallocation functions with an
  explicit `align_val_t` avoids both problems and cleanly separates *storage* from
  *construction*.

- **Overflow guard.**  Because the allocation size is computed by hand (`capacity * sizeof(T)`),
  the language's array-`new` overflow check does not apply, so `capacity > SIZE_MAX / sizeof(T)`
  is checked explicitly before allocating.

- **`Align >= alignof(T)` is required for correctness.**  The `requires` clause demands it
  (alongside power-of-two).  In `fixed_vector`, `alignas` would keep the array naturally aligned
  regardless, but here nothing else would enforce it.  The block is raw storage from the aligned
  `::operator new`, so a smaller `Align` would begin element lifetimes at an under-aligned
  address, which is undefined behavior.  `aligned_byte_buffer` needs no such constraint because
  `alignof(std::byte)` is 1, so every power of two satisfies it.  That is why its clause reduces
  to `has_single_bit(Align)`.

- **Default alignment.**  `Align` defaults to `std::max(alignof(std::size_t), alignof(T))`,
  matching `fixed_vector`.  The storage is never under-aligned, and word alignment for small `T`
  is free, since any alignment up to `__STDCPP_DEFAULT_NEW_ALIGNMENT__` costs the aligned
  allocator nothing.  (`aligned_byte_buffer` instead defaults to 16, its SIMD-lane motivation.)

- **`data()` asserts alignment to the compiler.**  It returns `std::assume_aligned<Align>(ptr)`
  (guarded for the null/empty case, since `assume_aligned` requires a real aligned pointer) so
  caller loops can vectorize on the known alignment.  `begin() == data()` is aligned, but
  `end() = data() + size()` is not claimed to be.

- **Constructing from a range/iterators requires forward iterators.**  Capacity must be known
  before allocation, so the range and iterator+sentinel *constructors* require forward
  iterators/ranges (capacity = distance).  Input-only sources are handled by constructing with
  an explicit capacity and then calling `append_range` (which still accepts input iterators).

- **Value semantics.**  Copy performs a deep copy (independent buffer).  Move *construction*
  transfers the pointer and leaves the source empty (capacity 0).  Move *assignment* swaps, so
  the source is left holding the target's former buffer until it is destroyed.  Copy and move
  assignment replace the capacity too, while `assign_range` keeps the current capacity and
  throws if the source does not fit.  Copy-and-swap gives assignment a strong guarantee.

- **`constexpr` interface, limited compile-time use.**  The members are annotated `constexpr`
  (matching `fixed_vector`), so empty / zero-capacity instances and the non-allocating members
  are usable in constant expressions.  Instances with capacity `> 0` cannot be constructed at
  compile time, because over-aligned allocation (the raw aligned `operator new`) is not usable
  during constant evaluation.  Those code paths sit behind a `capacity == 0` guard and are
  simply never taken by a constant evaluation.  `noexcept` follows `fixed_vector`'s placement
  (observers, element access, iterators, `clear`, `pop_back`, move, and `swap`), and allocating
  or throwing members are not `noexcept`.

## `aligned_byte_buffer<Align>`

`aligned_byte_buffer` is the `std::byte` specialization of `dynamic_fixed_vector`.  The element
type is fixed, so only the alignment is a template parameter (a power of two, default 16).
Distinct alignments are distinct types.  The API and conventions are unchanged, and the fixed
element type enables these differences:

- **No overflow guard.**  `sizeof(std::byte) == 1`, so the byte count equals the capacity —
  there is no multiplication and nothing to overflow.

- **Reserved capacity is left uninitialized.**  Rather than value-initializing (zeroing) every
  byte up front, the block's object lifetimes are begun with
  `std::start_lifetime_as_array<std::byte>` and the reserved tail is not written.  Bytes that
  enter `size()` (via `push_back` / `append` / `resize` / `fill_*`) are always written first.

  Reading beyond `size()` through `operator[]` therefore yields an **unspecified** byte value,
  which is well-defined, *not* undefined behavior, because `std::byte` (like `unsigned char`) is
  exempt from the indeterminate-value rules.  This trades a strict "beyond-size reads as zero"
  guarantee for avoiding an O(capacity) zeroing at construction, which matters for large
  scratch/IO buffers.

- **Byte-granular bulk operations.**  `std::memcpy` serves the span append fast path (assuming
  the source does not overlap the buffer), and `std::memset` serves `fill_capacity` /
  `fill_size` / `resize`-grow.  The copy constructor copies only the live `[0, size())` bytes,
  since the reserved tail is unspecified anyway.  `operator==` / `operator<=>` are unconditional
  (`std::byte` is always comparable, yielding `std::strong_ordering`).  `memcpy` and `memset`
  are not `constexpr`, but like the allocation they sit behind an emptiness guard, so the
  `constexpr` annotation still holds for the empty case.

- **Explicit zeroization** (see the shared invariants).  For the byte buffer,
  `zeroize_reserved_unused()` also turns the otherwise *unspecified* reserved tail into
  determinate zeros.  That pads to an alignment boundary before whole-lane SIMD reads past
  `size()`, and it keeps stale heap bytes from leaking through beyond-size reads.

- **`equal_constant_time(span, span)`** is a namespace-scope helper in `byte_compare.hpp`,
  deliberately *not* used by the container.  It is shared with `borrowed_byte_buffer`, so it is
  defined once rather than in each byte header.  It is explicitly `inline`, since two
  definitions of a non-inline function across translation units would violate the ODR.

  It OR-accumulates the XORed byte pairs with no data-dependent branch or early exit, so its
  timing depends only on the (normally public) lengths.  Use it for secret-dependent
  comparisons, such as MAC or tag verification, where `operator==`'s first-mismatch early exit
  leaks the position of the first differing byte through timing.  The container's own
  comparisons stay variable-time, per ordinary container semantics.

  The standard has no notion of timing, so branch-freedom in the source is not by itself a
  guarantee: nothing forbids a compiler from proving the accumulator is monotone and
  short-circuiting the loop.  The accumulator is therefore `volatile`, which obliges the
  compiler to perform every accumulation in order.  This is the same distrust of the optimizer
  that `zeroize_reserved_unused()` pays for with `memset_explicit`.

  It costs the vectorization (a `volatile` accumulator forces a load and a store per byte, where
  a plain one would fold into a `vpxor`/`vpor` reduction), and that price is worth paying on the
  tag-sized inputs this is for.  It also costs `constexpr`: a `volatile` access is barred in
  constant evaluation, so the function is a plain `inline` one.

- **`constexpr` / `noexcept`** follow `dynamic_fixed_vector` (see above): empty instances are
  usable in constant expressions, and capacity `> 0` requires a run-time allocation.

- **Motivating use.**  The type exists for a contiguous `std::byte` buffer aligned like a SIMD
  lane (e.g. 16 bytes), filled incrementally, then read as `std::span<const std::byte>` and
  handed to intrinsics.

## `borrowed_byte_buffer`

`borrowed_byte_buffer` is the non-owning byte buffer.  It overlays storage the caller owns (a
stack array, a member object, or a slice of a larger buffer) and offers the same append/cursor
interface without ever allocating.  The whole object is
`{ std::byte* data_, std::size_t capacity_, std::size_t size_ }`.

- **Non-owning, so the ownership machinery is simply gone.**  There is no aligned
  `::operator new`/`delete`, no `unique_ptr`, no deleter, and no overflow guard (nothing is
  allocated), and the destructor is trivial.  The caller is responsible for keeping the borrowed
  storage alive for the buffer's lifetime.  A destroyed source leaves a dangling view, exactly
  as with `std::span` / `std::string_view`.

- **Beyond-`size()` reads expose the caller's bytes.**  `operator[]` is capacity-based here as
  everywhere, so an index in `[size(), capacity())` reads the borrowed region.  Both byte
  buffers are described as yielding an *unspecified* byte there, but they mean different things
  by it, and the difference is the one a caller has to act on.

  In `aligned_byte_buffer` the tail is heap storage that was never written: the value really is
  arbitrary, and only `std::byte`'s exemption from the indeterminate-value rules keeps the read
  out of UB.  Here the tail is memory the caller owns and very likely initialized, so the byte
  is usually perfectly determinate.  It is also the *caller's own data*, which a beyond-size
  read (or an `operator==` after `resize`, or a `span()` handed onward) will disclose.

  The container guarantees nothing about the value in either case.  What differs is that here
  "nothing is guaranteed" is not the same as "nothing is there".  `zeroize_reserved_unused()` is
  the answer when the tail must not leak.

- **Copy and move are shallow, and all six special members are defaulted.**  The type is a
  pointer and two integers, hence trivially copyable — cheap to pass by value.  A copy is a
  second view of the *same* bytes (both observe writes through either), and move is the same
  member-wise copy: a moved-from `borrowed_byte_buffer` still points at its storage rather than
  being emptied.  This is the opposite of the heap-backed siblings (whose move *construction*
  empties the source), and it is the correct default for a view: there is nothing to transfer,
  and a view type that nulled itself on move would surprise.  The `swap` remains as a member +
  hidden friend for API parity.

- **No `Align` parameter.**  `aligned_byte_buffer`'s `Align` exists to over-align *its own*
  allocation and then cash that in through `std::assume_aligned` for vectorized caller loops.
  `borrowed_byte_buffer` allocates nothing and cannot vouch for a caller's alignment, so
  promising one would be unfounded.  `data()` returns the borrowed pointer unadorned.  (A future
  opt-in `Align`, a caller-*asserted* precondition rather than a guarantee, could restore the
  `assume_aligned`, but it is out of scope until a caller needs it.)

- **No `operator=(initializer_list)`.**  The other three types have one, and there
  `v = {1, 2, 3}` can only mean *replace the contents*: the container owns its storage, so an
  assignment has nothing else to do.  On a view the same expression also reads as *rebinding* —
  pointing it at something new, which is what assigning to a `std::span` does.  The two readings
  differ in who gets written to, and guessing wrong silently stores into memory the caller
  merely lent, so the expression is left ill-formed instead.  `assign_range(il)` is the bulk
  store, and names itself.

  Construction closes the other reading: an `initializer_list`'s storage is `const`, so
  `borrowable_range` rejects it and there is no `borrowed_byte_buffer(initializer_list)` to
  rebind from either.

- **Construction starts empty, and `adopting` starts full.**  The value constructors leave
  `size() == 0` and treat the region as scratch to build into, matching
  `aligned_byte_buffer(capacity)`.  Because an overlay sits on memory that *already holds a
  value*, a second mode is genuinely useful: reading bytes already present (a header, a received
  packet, a key).  That is the `adopting` named constructor family, which starts
  `size() == capacity()`.

  The two differ only in where the size cursor begins (the bytes are identical), so this is an
  ergonomic default, not a capability split: `resize(capacity())` reveals the tail from write
  mode, and `clear()` empties from adopt mode.  Named constructors were chosen over a tag type
  for call-site clarity (`borrowed_byte_buffer::adopting(header)`).

- **The borrowing constructor takes a contiguous range, not a `std::span<T>`.**  A
  `std::span<T>` *parameter* forces the caller to write `std::span{arr}`: implicit array→span
  conversion does not happen during template argument deduction, and even a CTAD'd
  `std::span{arr}` has a *static* extent that will not bind a `std::span<T, dynamic_extent>`
  parameter.  A `contiguous_range` parameter binds `borrowed_byte_buffer{arr}` bare, for any
  `std::array` / `std::vector` / `std::span` / C array / `std::string`.  It is constrained by
  the `borrowable_range` concept to reject what would be unsound:
  - **A concept, at namespace scope, rather than a `bool` trait inside the class.**  A
    `constexpr bool` variable template instantiates *every* operand of its initializer (it is
    one expression, not a short-circuiting constraint), so `range_value_t<R>` would hard-error
    for a non-range `R` such as the `T*` overload's argument.  Concept conjunction
    short-circuits, so `contiguous_range` as the first clause keeps `range_value_t` from ever
    being formed for a non-range.  A concept cannot be a class member, hence the forward
    declaration of `borrowed_byte_buffer` above it.
  - **The source must not be `borrowed_byte_buffer` itself.**  This is required for correctness,
    not cosmetics.  Without the exclusion, constructing from a *non-`const`*
    `borrowed_byte_buffer` lvalue would prefer the range template over the copy constructor (it
    binds a less-cv-qualified reference, [over.ics.rank]) and reinterpret the source's own
    bytes.  The exclusion makes copy/move win.
  - **The elements must be trivially copyable and non-`const`.**  The object representation is
    what is written and later read back, and the view writes through it (a `const` source cannot
    back a mutable byte view).
  - **The source must be a `borrowed_range` *or* an lvalue.**  An rvalue owning container (a
    temporary `std::vector`) would leave a dangling view, so only non-owning rvalues
    (`std::span`) and lvalues are accepted.

- **The single-object constructor takes a forwarding reference, not `T*`.**  Overlaying one
  object (`borrowed_byte_buffer{&obj}`, capacity `sizeof(*&obj)`) needs a pointer constructor,
  but a plain `T*` parameter is, by partial ordering, *more specialized* than the range
  constructor's `R&&`, so a C array would decay to it and overlay only its first element.
  Deducing the parameter from the un-decayed argument and requiring `std::is_pointer` (the
  `borrowable_object_ptr` concept) rejects arrays (they reach only the range constructor) while
  still accepting a genuine object pointer.

- **`data()` is not guaranteed null exactly when `capacity()` is 0.**  `aligned_byte_buffer`
  guarantees that (its `allocate_` returns null for capacity 0), but here a caller can borrow a
  zero-length region at a non-null address.  The mutating members still index `data()` only
  under `!is_full()` / `!is_empty()` / `i < capacity()`, each of which implies `capacity() > 0`.
  By the constructor precondition that the source points to at least `capacity()` writable
  bytes, that in turn implies a non-null, indexable block.  So the reasoning the members rely on
  holds, but it rests on the constructor's precondition rather than on an allocation guarantee.

- **Only one of the four construction preconditions is checkable.**  The constructors all
  require that the source really provide `capacity()` bytes, but only the range constructor is
  handed something that knows its own extent, so only it can assert
  `capacity() <= std::span{r}.size_bytes()` under `-DDEBUG`, and `adopting(R&&, capacity)`
  inherits that check by delegating to it.  A `void*` plus a count, and the single-object
  pointer, carry no size the container can compare against.  Their tags stay promises, and a
  caller who overstates the capacity gets a heap overflow that ASan reports instead.  This is
  the only `-DDEBUG` assert in the family that guards a constructor rather than an accessor or a
  mutator.

- **`constexpr` limits.**  Forming a byte view over an object requires a `reinterpret_cast`,
  which is barred in constant evaluation, so the borrowing constructors are not
  `constexpr`-usable and are left un-annotated.  Only the default (empty) instance and the
  non-borrowing members work in constant expressions, verified by a
  `static_assert(constexpr_empty_ok())` as with the heap types.  The members are `noexcept`
  where the heap types' are, plus the borrowing constructors themselves (they cannot throw,
  since they allocate nothing).

## Edge cases and gotchas

- **Zero capacity is both empty and full.**  For the runtime types a default-constructed (or
  zero-capacity) instance satisfies `is_empty()` and `is_full()` simultaneously — correct, since
  there is no element and no remaining space.  `fixed_vector` cannot be *constructed* that way
  (it requires `N > 0`), but `reserve(0)` reaches the same state, with all `N` slots alive and
  unreserved.

- **`std::byte` is not `std::constructible_from` an `int`.**  Direct-initialization of a scoped
  enumeration from an integer (`std::byte b(42)`) is ill-formed, so a `constructible_from`
  constraint would reject the integer arguments that the functional cast `std::byte(42)` in the
  `emplace_back` body handles fine.  `aligned_byte_buffer`'s `emplace_back` family is therefore
  constrained by argument *type* instead: at most one argument, of type `std::byte` or an
  integral type.

  This deliberately rejects floating-point arguments (an expression-validity constraint would
  have accepted `emplace_back(3.99)` and stored `byte{3}`, since the cast follows `static_cast`
  rules) and other enumeration types (cast explicitly, e.g. with `std::to_underlying`, if
  intended).  What no constraint can reject is an out-of-range *runtime* integer:
  `emplace_back(256)` stores `byte{0}`.

- **`append_range(span)` assumes the source does not alias the buffer** (it uses `memcpy` in the
  byte buffer).  Appending a view over the buffer's own storage into itself is unsupported.  The
  assumption reaches past the span overloads themselves: a contiguous range of the element type
  is forwarded to them (see above), so `append_range(rg)` carries the same tag for that case.

- **A `borrowed_byte_buffer` outliving its storage dangles.**  It owns nothing, so it is the
  caller's job to keep the borrowed region alive — the same contract as `std::span`.
  Construction rejects the one case it *can* catch (an rvalue owning container, which would
  dangle immediately), but cannot see a later-freed source.  The shallow copy compounds this:
  every copy views the same storage, so all of them dangle together.

## Precondition checks under `-DDEBUG`

Every `\pre` tag in the headers is a promise the caller makes.  `-DDEBUG` (which the Makefile
sets for the `test-*.debug` binaries only) turns the subset of those promises that can be
checked cheaply into `assert`s.  The checked set is the intersection of two conditions: the
header already documents the precondition with a `\pre`, and the container can decide it in O(1)
from state it already holds.  Each assert sits in a `#if defined(DEBUG)` block, so a release
build contains no `__assert_fail` at all.

- **The `\pre` tags are the spec, and the asserts only enforce them.**  An assert with no
  matching tag, or one stricter than the tag it enforces, puts the code at odds with the
  documented contract — and the contract is what callers program against.  `operator[]` is the
  case where that matters most: it asserts `i < capacity()` and deliberately **not**
  `i < size()`, because reading a live element at an index at or past `size()` is a designed
  capability of this family (see "`operator[]` is unchecked and capacity-based" above) that all
  four suites exercise on purpose.  The tighter assert would contradict the design and fail the
  tests.  In `fixed_vector` the bound is the *current* `capacity()`, so a `reserve()` shrink
  leaves live slots outside the contract.
- **A precondition documented on one overload is documented on both.**  `front`, `back`, and
  `operator[]` state the `\pre` on the non-`const` overload and give the `const` twin a
  `/// \copydoc front()`, so the generated docs for an overload that asserts a precondition
  never show it blank.  `data()` and the `adopting` family follow the same convention.
- **The checks live at the leaf.**  `unchecked_push_back` carries no assert of its own.  Its
  `!is_full()` check is in the `unchecked_emplace_back` it delegates to, so one assert catches
  violations arriving through either overload.  `adopting(R&&, capacity)` inherits the range
  constructor's check the same way.
- **The unasserted `\pre` tags stay promises for reasons, not by omission.**  The non-overlap
  tags on the `span` overloads (and on the range overloads, which carry them for the contiguous
  case they forward) would need exactly the runtime aliasing check those bulk paths exist to
  avoid.  "`[first, last)` is a valid range" is not checkable *by the container* at all.  And
  `borrowed_byte_buffer`'s pointer-based construction tags describe memory the container has no
  way to measure.  Only its range constructor's version is checkable, because a range carries
  its own size (see that section above).
- **`_GLIBCXX_DEBUG` covers range validity from the other side**, whenever the iterators come
  from a standard container — which is how the tests supply them.  It earns its place on that
  alone: an invalid iterator yields a garbage distance, which the up-front capacity check
  reports as `std::bad_alloc`, sending the reader after a capacity bug that does not exist.
  Debug mode names the real defect instead ("attempt to copy a singular iterator", "iterators
  from different sequences"), and plain ASan does not catch it at all.
- **The asserts are `constexpr`-clean.**  An assert whose condition holds is fine during
  constant evaluation, so `test-fixed_vector.cpp`'s `static_assert` blocks still compile under
  `-DDEBUG`.
- **`DEBUG` is an ODR hazard, which is part of why the debug build gets its own binaries.**
  Like `NDEBUG`, it changes the definition of inline and template functions, so a `-DDEBUG`
  translation unit must not be linked against a non-`DEBUG` one.  `_GLIBCXX_DEBUG` is the same
  hazard one level down (it swaps in `__debug::vector`), and it is safe here only because each
  test is a single TU.
- **`-fhardened` was considered and rejected.**  GCC declines to apply its own `_FORTIFY_SOURCE`
  / `_GLIBCXX_ASSERTIONS` when those are set explicitly, as they are here, and warns that it
  has.  What remains (PIE, relro, cf-protection, and stack-protector) hardens a shipped binary
  rather than finding bugs in a test run, and ASan already reports a smashed stack more
  precisely.

## Testing

See [`README.md`](README.md) for the test inventory and the commands to run it.  The choices it
describes are deliberate rather than incidental, and this is the reasoning behind them.

- **The exit status is the entire contract: silence and 0 on success, a message and non-zero on
  failure.**  These programs previously printed a running commentary of container state and
  embedded the expected stdout in a trailing comment block, as a second and coarser net: diffing
  a run against the block caught changes to observable state that no assertion mentioned.

  It was dropped because the cost outgrew that benefit.  The block had to be regenerated from a
  real run after every change, which made it as likely to record a regression as to catch one.
  It made pass/fail mean "exit 0 *and* diff the output", which a `make test` target cannot
  express.  And it was noisy in the way that matters least, since most of the churn came from
  `sizeof` and capacity numbers that no invariant depends on.  What replaces it is coverage:
  every member, both overloads, checked explicitly rather than watched from a distance.

- **Checks are `CHECK`, never `assert`.**  `assert` fires `abort()`, which dumps a core file for
  what is only a failed comparison.  `CHECK` prints `file:line: function: CHECK failed: <expr>`
  and calls `std::exit(EXIT_FAILURE)`, so a failing test leaves nothing behind to clean up.  The
  same reasoning drives `run_tests`, which catches every exception: one escaping `main` would
  reach `terminate()` and abort just the same.  `CHECK_THROWS` distinguishes "threw nothing"
  from "threw the wrong type", because those are different defects.

- **`fixed_vector`'s suite drives the container at compile time.**  Nearly its whole interface
  is `constexpr`, so a `static_assert` block ahead of `main()` drives a vector through the whole
  append family (`append_range`, `push_back`, `emplace_back`, and the `try_*` and `unchecked_*`
  forms), the accessors, `assign_range`, `resize`, `fill_size`, `pop_back`, `clear`, and `swap`
  (both the member and the hidden friend).  Two further blocks cover `fill_capacity`,
  `zeroize_reserved_unused`, `reserve`, and `zeroize_unreserved`, all during constant
  evaluation.

  That moves a regression from a failed run to a failed build, and it is the only net that
  rejects an index outside the storage without aborting.  `operator[]` is unchecked by design,
  so in the release build `v[10]` on a `fixed_vector<int, 5>` reads whatever is past the array
  and every `CHECK` still passes.  The debug build catches it, but through the `-DDEBUG`
  `i < capacity()` assert, which means `abort()`.  In a `static_assert` the same expression
  simply fails to compile (GCC 16.1.1 rejects it through `std::array`'s hardened precondition).

  The heap-backed types cannot be tested this way, because over-aligned allocation is not usable
  in constant evaluation.  Their suites reach only the empty and zero-capacity members, which is
  why the compile-time coverage is so lopsided across the three.

- **The heap-backed types are also run under AddressSanitizer + UndefinedBehaviorSanitizer.**
  `CHECK`s are blind to the specific mistakes these two can make: a read or write just past the
  block, a leaked buffer, or an aligned `::operator new` paired with the wrong deallocation
  function (the array-`new` trap described above) all leave every check passing and the
  program exiting 0.  ASan sees them, and reports that last one as `new-delete-type-mismatch`,
  naming the allocated and the deallocated alignment.  `aligned_byte_buffer` adds a wrinkle: it
  reads beyond `size()` on purpose, which is legitimate inside the allocation and must not be
  allowed to shade into straying outside it.  Both suites are expected to be clean.

- **`borrowed_byte_buffer` needs the sanitizers for different reasons.**  It never allocates, so
  there is no leak or bad-free to find, but it reads beyond `size()` into the borrowed tail
  (ASan confirms those reads stay inside the backing object) and forms its view through a
  `reinterpret_cast` to `std::byte*` (the release build's `-fstrict-aliasing`, off at `-Og`,
  exercises that path under optimization).

  Its suite also carries the type's view-specific behavior (shallow-copy aliasing, non-emptying
  move, and overlaying a typed object).  Because its construction is heavily constrained, the
  suite adds a block of `static_assert`s that pin the construction contract at compile time:
  rvalue owners, `const` elements, and non-pointer non-ranges are rejected, and bare containers
  and object pointers are accepted.
