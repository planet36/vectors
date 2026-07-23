# vectors

A header-only C++ library of **fixed-capacity vectors**: resizable sequences that never
reallocate, never grow past a capacity fixed when the container is created, and never
individually destroy an element.

Each header is standalone — drop it in an include path and `#include` it. There is no package
manifest, and using the library needs no build system; the `Makefile` here only builds and runs
the tests.

## The containers

| Header | Type | Capacity fixed at | Storage |
|---|---|---|---|
| `fixed_vector.hpp` | `fixed_vector<T, N, Align>` | compile time (`N`) | in-place `std::array<T, N>`, no heap |
| `dynamic_fixed_vector.hpp` | `dynamic_fixed_vector<T, Align>` | run time (constructor) | one over-alignable heap block |
| `aligned_byte_buffer.hpp` | `aligned_byte_buffer<Align>` | run time (constructor) | one over-alignable heap block |
| `borrowed_byte_buffer.hpp` | `borrowed_byte_buffer` | run time (constructor) | **borrowed** — overlays storage it does not own |

`dynamic_fixed_vector` is the runtime-capacity analogue of `fixed_vector`. `aligned_byte_buffer`
is its `std::byte` specialization, kept as a separate type so it can be simpler and faster: the
element type is fixed, so only the alignment is a template parameter, bulk operations drop to
`memcpy` / `memset`, and the reserved tail is left uninitialized rather than zeroed.
`borrowed_byte_buffer` is the **non-owning** counterpart to `aligned_byte_buffer`: the same
byte-buffer API over memory the caller owns — it overlays an array, object, or span, never
allocates or frees, and copies shallowly (a second view of the same bytes). It has no `Align`
parameter, since it makes no promise about borrowed memory's alignment.

All four share one append/access API. Learn one and you know the others; the two byte buffers add
a free `constant_time_equal` (in `byte_compare.hpp`, which they both include).

## Requirements

- **GCC 16** with `-std=c++23` (the headers use `std::from_range`, `std::start_lifetime_as_array`,
  ranges, and concepts).
- No third-party libraries — the headers and their tests need only the standard library.

## Quick start

### `fixed_vector` — capacity known at compile time

```cpp
#include "fixed_vector.hpp"

fixed_vector<int, 8> v{1, 2, 3};   // size 3, capacity 8

v.push_back(4);                    // throws std::bad_alloc if full
if (!v.try_push_back(5))           // returns false instead of throwing
    handle_full();
v.append_range(std::array{6, 7});  // sized source: all-or-nothing

assert(v.size() == 7);
assert(v.at(6) == 7);              // at() throws std::out_of_range

v.clear();                         // size becomes 0; the elements stay alive
assert(v[0] == 1);                 // still readable — operator[] is capacity-based
```

Essentially the whole interface is `constexpr`, so a `fixed_vector` works as compile-time scratch
space:

```cpp
constexpr int sum_first_n(const int n)
{
    fixed_vector<int, 16> v;
    for (int i = 1; i <= n; ++i)
        v.push_back(i);

    int total = 0;
    for (const int e : v)
        total += e;
    return total;
}
static_assert(sum_first_n(5) == 15);
```

### `dynamic_fixed_vector` — capacity known at run time

```cpp
#include "dynamic_fixed_vector.hpp"

dynamic_fixed_vector<float, 32> v(n);   // capacity n, size 0, storage 32-byte aligned

v.push_back(1.5F);
v.append_range(src);                    // src is any range of float
```

The one-argument constructor **reserves** capacity and starts empty — unlike `fixed_vector`, where
`fixed_vector<T, N>(count)` creates `count` elements. `Align` may exceed `alignof(T)`, and `data()`
applies `std::assume_aligned<Align>` so caller loops can vectorize on it.

### `aligned_byte_buffer` — a SIMD-aligned byte buffer

```cpp
#include "aligned_byte_buffer.hpp"

aligned_byte_buffer<16> buf(4096);   // 16-byte aligned, reserved tail left uninitialized

buf.append_range(chunk);             // memcpy fast path
buf.push_back(std::byte{0xFF});
process(buf.span());                 // hand off to intrinsics

buf.zeroize_reserved_unused();       // make the tail determinate zeros (lane padding)
buf.clear();
buf.zeroize_reserved_unused();       // clear() + zeroize = scrub everything
```

For secret-dependent data, compare with the free function rather than `operator==`, whose
first-mismatch early exit leaks the position of the first differing byte through timing:

```cpp
if (constant_time_equal(tag, expected))   // no data-dependent branch or early exit
    accept();
```

### `borrowed_byte_buffer` — a byte buffer over memory it does not own

```cpp
#include "borrowed_byte_buffer.hpp"

std::array<std::byte, 64> storage;      // memory owned elsewhere

borrowed_byte_buffer buf{storage};      // overlays it: size 0, capacity 64
buf.append_range(header);               // the writes land in `storage`
buf.push_back(std::byte{0xFF});
process(buf.span());                    // hand the live bytes to intrinsics
```

Construction borrows a pointer or any writable contiguous range (`std::array`, `std::vector`,
`std::span`, a C array, `std::string`) — passed bare, no `std::span{...}` wrapper — and starts
**empty**, treating the region as scratch to build into. To instead read bytes *already present*
in the region, use the `adopting` named constructors, which start full (`size() == capacity()`):

```cpp
const auto view = borrowed_byte_buffer::adopting(storage);   // size == capacity
if (constant_time_equal(view.span(), expected))
    accept();
```

It owns nothing, so copy and move are shallow — the copy views the same bytes, and a moved-from
buffer is left pointing at the same storage, not emptied. The caller keeps the borrowed storage
alive for the buffer's lifetime.

## What makes these different from `std::inplace_vector`

These are deliberate departures from standard-container semantics, not oversights. The short
version:

- **Every capacity slot holds a live element from construction onward.** Capacity is not raw
  storage awaiting placement-new. (The exception is the byte buffers: `aligned_byte_buffer` leaves
  its reserved tail uninitialized and `borrowed_byte_buffer` inherits whatever the borrowed region
  held — either way a read past `size()` gives an *unspecified* `std::byte`, well-defined, not UB.)
- **Elements are never individually destroyed.** `clear()`, `pop_back()`, and `resize()` only
  adjust the size counter, so the element type must be trivially destructible (enforced by a
  `requires` clause). A consequence: `emplace_back` cannot construct in place — the slot is already
  occupied — so it builds a temporary and assigns it, equivalent to `push_back(T(args...))`.
- **`operator[]` is unchecked and capacity-based.** An index in `[size(), capacity())` legitimately
  reads a live element. `at()` is the only bounds-checked accessor.
- **Capacity overflow throws `std::bad_alloc`**, not `std::length_error`.

`DESIGN.md` explains the reasoning behind each of these, plus the allocation strategy, the
alignment defaults, the `constexpr` limits of the heap-backed types, and the edge cases worth
knowing (zero capacity is both empty and full; `append_range(span)` assumes no aliasing).

## API at a glance

Common to all four types:

| Group | Members |
|---|---|
| Capacity | `size`, `capacity`, `max_size`, `reserved_unused`, `is_empty`, `is_full` |
| Access | `operator[]` (unchecked), `at` (throws), `front`, `back`, `data`, `span` |
| Iterators | `begin` / `end` / `rbegin` / `rend` and the `c`-prefixed forms |
| Add | `push_back`, `emplace_back`, `append_range` |
| Add without throwing | `try_push_back`, `try_emplace_back`, `try_append_range` — `[[nodiscard]] bool` |
| Add without checking | `unchecked_push_back`, `unchecked_emplace_back` — assume `!is_full()` |
| Remove / resize | `clear`, `pop_back`, `resize` — none destroy elements |
| Bulk | `fill_capacity`, `fill_size`, `assign_range`, `zeroize_reserved_unused` |
| Compare | `operator==`, `operator<=>` — gated on the element type supporting them |

The three owning types build and fill their storage through the same constructors (reserve,
fill, span, iterator pair, `initializer_list`, range). `borrowed_byte_buffer` differs only in
construction: it has no reserve/fill/range *element-copying* constructors — it is built over
existing memory (a pointer or a contiguous range) and its `adopting` named constructors start it
full — but every member above behaves identically once constructed.

`append_range` and `assign_range` are each overloaded for a span, iterator + sentinel,
iterator + count, `initializer_list`, and an arbitrary input range; `assign_range` is `clear()`
followed by `append_range`. Overloads that can know the source size up front validate before
writing and are all-or-nothing; truly unsized sources append element-wise and may leave the
elements that fit in place before throwing (or returning `false`).

Passing a contiguous range of the element type — a `std::vector<T>`, a `std::array<T, N>`, a
span — gets the bulk copy (`memcpy` in the byte buffer) without the call site doing anything
special; the source must not overlap the container's own storage. Anything else appends
element-wise.

`zeroize_reserved_unused()` zeroizes `[size(), capacity())` with stores the optimizer may not
elide — `memset_explicit` / `explicit_bzero` when the C library declares one, otherwise volatile
writes.

## Building and running the tests

There is no test framework and no runner. Each type has one standalone program that exercises
every member, and the exit status is the whole contract: **a test passes when it prints nothing
and exits 0**. On the first failed check it prints one line to stderr and exits non-zero
immediately:

```
test-fixed_vector.cpp:536: test_at: CHECK failed: v.at(1) == 99
```

```sh
make        # build the test programs -- both variants (see below)
make test   # build if needed, then run them
```

`make test` applies that same contract to the suite: it prints nothing and exits 0 when every
program passes, and stops at the first one that doesn't. It runs the tests twice, once per build
variant — see below for why both. Each program is self-contained, so building one by hand works
too:

```sh
g++ -std=c++23 test-fixed_vector.cpp -o test-fixed_vector && ./test-fixed_vector
```

| Program | Covers |
|---|---|
| `test-fixed_vector.cpp` | `fixed_vector` |
| `test-dynamic_fixed_vector.cpp` | `dynamic_fixed_vector` |
| `test-aligned_byte_buffer.cpp` | `aligned_byte_buffer` |
| `test-borrowed_byte_buffer.cpp` | `borrowed_byte_buffer` |

All four share `test_utils.hpp`, which holds `CHECK` / `CHECK_THROWS` / `run_tests` and a few
helpers. Each program is a list of `test_<member>()` functions called from `main()`, one per
member, so `main()` doubles as the coverage checklist. Nothing calls `abort()` — a failing test
exits, it does not dump core.

`test-fixed_vector.cpp` also drives the container through a `static_assert` block ahead of
`main()`, so a regression there fails the compile rather than the run; the heap-backed suites can
only do that for their empty and zero-capacity cases. See `DESIGN.md` for why.

### The two build variants

`make test` runs the suite twice, because neither build subsumes the other. The variants are two
sets of binaries rather than two targets — `make` builds both, `make test` runs both:

- **release** — `test-*`, built at `-O3 -flto -march=native`.
- **debug** — `test-*.debug`, built with asserts, libstdc++ debug mode, fortified string ops,
  and ASan/UBSan.

The **debug** build catches what the release build hides. The two heap-backed types hand-manage
aligned memory, the byte buffers intentionally read partially-uninitialized (or borrowed)
storage, and `borrowed_byte_buffer` forms its view through a `reinterpret_cast`, so the
sanitizers have real work to do — and the asserts check preconditions the release build ignores
outright.

The **release** build catches what `-Og` structurally cannot, because the optimizer only *acts*
on the code's promises once it is optimizing. `-Og` leaves `-fstrict-aliasing` off (it turns on
at `-O2`), and it never cashes in `data()`'s `assume_aligned<Align>` — a caller loop over
`data()` compiles to no SIMD at all at `-Og`, while `-O3 -march=native` emits an aligned
`vmovdqa` that faults the moment that promise is untrue. Testing only the debug build would
leave the library's whole reason for `assume_aligned` unexercised.

Both are expected to be clean. The two variants' binaries are named differently, so they coexist
rather than shadowing each other and neither build ever silently serves the other's stale binary.

`-DDEBUG` turns on the headers' own precondition checks: every `\pre` the headers document and
can check cheaply — `!is_full()` for the `unchecked_*` family, `!is_empty()` for `front`/`back`,
`i < capacity()` for `operator[]` — becomes an `assert` that aborts on a violation. They check
the *documented* contract, not `std::vector`'s: `operator[]` asserts `i < capacity()`, so
reading a live element past `size()` stays legal. Each assert is inside `#if defined(DEBUG)`,
so a release build has no trace of one.

The headers sit next to the tests, so the commands above work as written.

## License

Mozilla Public License 2.0 (`SPDX-License-Identifier: MPL-2.0`), declared in each file's header.
