# vectors

A header-only C++ library of **fixed-capacity vectors**: resizable sequences that never
reallocate, never grow past a capacity fixed when the container is created, and never
individually destroy an element.

Each header is standalone — drop it in an include path and `#include` it. There is no build
system and no package manifest.

## The containers

| Header | Type | Capacity fixed at | Storage |
|---|---|---|---|
| `fixed_vector.hpp` | `fixed_vector<T, N, Align>` | compile time (`N`) | in-place `std::array<T, N>`, no heap |
| `dynamic_fixed_vector.hpp` | `dynamic_fixed_vector<T, Align>` | run time (constructor) | one over-alignable heap block |
| `aligned_byte_buffer.hpp` | `aligned_byte_buffer<Align>` | run time (constructor) | one over-alignable heap block |

`dynamic_fixed_vector` is the runtime-capacity analogue of `fixed_vector`. `aligned_byte_buffer`
is its `std::byte` specialization, kept as a separate type so it can be simpler and faster: the
element type is fixed, so only the alignment is a template parameter, bulk operations drop to
`memcpy` / `memset`, and the reserved tail is left uninitialized rather than zeroed.

All three share one API. Learn one and you know the others.

## Requirements

- **GCC 16** with `-std=gnu++26` (the headers use `std::from_range`, `std::start_lifetime_as_array`,
  ranges, and concepts).
- The **{fmt}** library — needed only by the test programs, not by the headers themselves.

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

buf.zeroize_remaining_space();       // make the tail determinate zeros (lane padding)
buf.clear();
buf.zeroize_remaining_space();       // clear() + zeroize = scrub everything
```

For secret-dependent data, compare with the free function rather than `operator==`, whose
first-mismatch early exit leaks the position of the first differing byte through timing:

```cpp
if (constant_time_equal(tag, expected))   // no data-dependent branch or early exit
    accept();
```

## What makes these different from `std::inplace_vector`

These are deliberate departures from standard-container semantics, not oversights. The short
version:

- **Every capacity slot holds a live element from construction onward.** Capacity is not raw
  storage awaiting placement-new. (The exception: `aligned_byte_buffer` leaves its reserved tail
  uninitialized, which is well-defined for `std::byte` — reads past `size()` give an *unspecified*
  byte, not UB.)
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

Common to all three types:

| Group | Members |
|---|---|
| Capacity | `size`, `capacity`, `max_size`, `remaining_space`, `is_empty`, `is_full` |
| Access | `operator[]` (unchecked), `at` (throws), `front`, `back`, `data`, `span` |
| Iterators | `begin` / `end` / `rbegin` / `rend` and the `c`-prefixed forms |
| Add | `push_back`, `emplace_back`, `append_range` |
| Add without throwing | `try_push_back`, `try_emplace_back`, `try_append_range` — `[[nodiscard]] bool` |
| Add without checking | `unchecked_push_back`, `unchecked_emplace_back` — assume `!is_full()` |
| Remove / resize | `clear`, `pop_back`, `resize` — none destroy elements |
| Bulk | `fill_capacity`, `fill_size`, `assign_range`, `zeroize_remaining_space` |
| Compare | `operator==`, `operator<=>` — gated on the element type supporting them |

`append_range` and `assign_range` are each overloaded for a span, iterator + sentinel,
iterator + count, `initializer_list`, and an arbitrary input range; `assign_range` is `clear()`
followed by `append_range`. Overloads that can know the source size up front validate before
writing and are all-or-nothing; truly unsized sources append element-wise and may leave the
elements that fit in place before throwing (or returning `false`).

`zeroize_remaining_space()` zeroizes `[size(), capacity())` with stores the optimizer may not
elide — `memset_explicit` / `explicit_bzero` when the C library declares one, otherwise volatile
writes.

## Building and running the tests

There is no test framework and no runner. Each type has one standalone program that exercises
every member with `assert`s and prints observable state; **a test passes when it exits 0**. Each
file also embeds its expected stdout in a trailing comment block, so diffing the output catches
regressions the asserts miss.

```sh
g++ -std=gnu++26 test-fixed_vector.cpp -lfmt -o a.out && ./a.out
```

| Program | Covers |
|---|---|
| `test-fixed_vector.cpp` | `fixed_vector` |
| `test-dynamic_fixed_vector.cpp` | `dynamic_fixed_vector` |
| `test-aligned_byte_buffer.cpp` | `aligned_byte_buffer` |

Nearly all of `fixed_vector` is `constexpr`, so its suite additionally drives the container
through a `static_assert` block before `main()` — those checks fail the compile rather than the
run. The heap-backed types cannot do this (over-aligned allocation is not constant-evaluable), so
their suites only `static_assert` the empty/zero-capacity cases.

The two heap-backed types hand-manage aligned memory, and the byte buffer intentionally reads
partially-uninitialized storage, so also run their tests under sanitizers — both are expected to
be clean:

```sh
g++ -std=gnu++26 -g -fsanitize=address,undefined test-aligned_byte_buffer.cpp -lfmt -o a.san && ./a.san
```

Each test file's leading comment shows the command it was built with, which refers to `gpp` — a
personal wrapper for `g++ $CPPFLAGS $CXXFLAGS "$@" -lfmt`, where `$CXXFLAGS` already sets
`-std=gnu++26` and a large warning set. The headers sit next to the tests, so the plain `g++`
commands above work as written.

## License

Mozilla Public License 2.0 (`SPDX-License-Identifier: MPL-2.0`), declared in each file's header.
