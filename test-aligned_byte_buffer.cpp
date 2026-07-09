/*

gpp test-aligned_byte_buffer.cpp && ./a.out

Sanitized:
g++ -std=gnu++26 -g -fsanitize=address,undefined test-aligned_byte_buffer.cpp -lfmt -o a.san && ./a.san

*/

// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

#include "aligned_byte_buffer.hpp"

#include <fmt/ranges.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <vector>

// Terse byte literals: 1_b, 0xAB_b, ...
constexpr std::byte
operator""_b(unsigned long long v)
{
    return static_cast<std::byte>(v);
}

constexpr std::byte
to_byte(const int i)
{
    return static_cast<std::byte>(i);
}

template <std::size_t A>
void
print_abb(const aligned_byte_buffer<A>& v)
{
    const auto align_off = reinterpret_cast<std::uintptr_t>(v.data()) % A;
    const auto as_ints =
        v.span() | std::views::transform([](std::byte x) { return std::to_integer<unsigned>(x); });
    fmt::println("abb: span={}  size={}  cap={}  remaining={}  align={}  data%align={}  is_empty={}  is_full={}  sizeof={}",
            as_ints, v.size(), v.capacity(), v.remaining_space(), A, align_off,
            v.is_empty(), v.is_full(), sizeof(v));
}

template <typename Buf>
std::vector<int>
to_ivec(const Buf& b)
{
    std::vector<int> out;
    for (const std::byte x : b.span())
        out.push_back(std::to_integer<int>(x));
    return out;
}

template <class Ex, class Fn>
void
expect_throw(const std::string_view label, Fn&& fn)
{
    bool threw = false;
    try
    {
        std::forward<Fn>(fn)();
    }
    catch (const Ex& ex)
    {
        threw = true;
        fmt::println("Caught expected exception ({}): {}", label, ex.what());
    }
    assert(threw);
}

// Compile-time check: empty / zero-capacity instances are usable in constant expressions.
// (The allocating paths are not, since over-aligned allocation is not usable in constant
// evaluation -- so only the non-allocating members are exercised here.)
constexpr bool
constexpr_empty_ok()
{
    aligned_byte_buffer<16> a;    // default ctor (no allocation)
    aligned_byte_buffer<16> b(0); // zero-capacity ctor (no allocation)
    if (!(a.is_empty() && a.size() == 0 && a.remaining_space() == 0))
        return false;
    if (!(b.capacity() == 0 && b.is_full()))
        return false;
    if (a.try_push_back(std::byte{1})) // capacity 0 -> full -> false, must not throw
        return false;
    b.clear();
    b.pop_back();
    b.zeroize_remaining_space(); // no reserved tail on a zero-capacity buffer -> no-op
    aligned_byte_buffer<16> c;
    swap(b, c);
    return b.size() == 0 && b == c;
}
static_assert(constexpr_empty_ok());

int main()
{
    // ---- Constructors ----

    {
        fmt::println("\n# Default constructor");
        const aligned_byte_buffer<16> v;
        print_abb(v);
        assert(v.size() == 0);
        assert(v.capacity() == 0);
        assert(v.is_empty());
        assert(v.data() == nullptr);
    }

    {
        fmt::println("\n# Capacity constructor (reserve, empty; tail left uninitialized)");
        const aligned_byte_buffer<16> v(64);
        print_abb(v);
        assert(v.size() == 0);
        assert(v.capacity() == 64);
        assert(v.remaining_space() == 64);
        assert(v.is_empty() && !v.is_full());
    }

    {
        fmt::println("\n# Capacity + value constructor (filled to capacity)");
        const aligned_byte_buffer<16> v(3, 0x42_b);
        print_abb(v);
        assert(v.size() == 3 && v.capacity() == 3 && v.is_full());
        assert(to_ivec(v) == std::vector({0x42, 0x42, 0x42}));
    }

    {
        fmt::println("\n# std::span constructor");
        constexpr std::array arr{1_b, 2_b, 3_b};
        const aligned_byte_buffer<16> v(std::span<const std::byte>{arr});
        print_abb(v);
        assert(v.capacity() == 3);
        assert(to_ivec(v) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# Forward iterator + sentinel constructor");
        const std::vector src{1_b, 2_b, 3_b, 4_b};
        const aligned_byte_buffer<16> v(src.begin(), src.end());
        print_abb(v);
        assert(v.capacity() == 4);
        assert(to_ivec(v) == std::vector({1, 2, 3, 4}));
    }

    {
        fmt::println("\n# Iterator + count constructor");
        const std::vector src{1_b, 2_b, 3_b, 4_b};
        const aligned_byte_buffer<16> v(src.begin() + 1, 2);
        print_abb(v);
        assert(v.capacity() == 2);
        assert(to_ivec(v) == std::vector({2, 3}));
    }

    {
        fmt::println("\n# initializer_list constructor");
        const aligned_byte_buffer<16> v{1_b, 2_b, 3_b};
        print_abb(v);
        assert(v.capacity() == 3);
        assert(to_ivec(v) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# from_range constructor");
        const auto rg = std::views::iota(1, 5) | std::views::transform(to_byte);
        const aligned_byte_buffer<16> v(std::from_range, rg);
        print_abb(v);
        assert(v.capacity() == 4);
        assert(to_ivec(v) == std::vector({1, 2, 3, 4}));
    }

    {
        fmt::println("\n# initializer_list assignment operator");
        aligned_byte_buffer<16> v(5);
        v = {7_b, 8_b, 9_b};
        print_abb(v);
        assert(v.capacity() == 5); // capacity unchanged by assign
        assert(to_ivec(v) == std::vector({7, 8, 9}));
    }

    // ---- Copy / move / swap ----

    {
        fmt::println("\n# Copy constructor (deep copy of the live [0,size) bytes)");
        aligned_byte_buffer<16> a(8);
        a.append_range({1_b, 2_b, 3_b});
        aligned_byte_buffer<16> b = a;
        print_abb(b);
        assert(a.data() != b.data()); // independent buffers
        assert(b.capacity() == 8);
        assert(to_ivec(a) == to_ivec(b));
        a[0] = 99_b;
        assert(b[0] == 1_b); // mutation of a does not affect b
    }

    {
        fmt::println("\n# Move constructor (source emptied)");
        aligned_byte_buffer<16> a{1_b, 2_b, 3_b};
        const std::byte* const orig = a.data();
        aligned_byte_buffer<16> b = std::move(a);
        print_abb(b);
        assert(b.data() == orig); // buffer transferred, not reallocated
        assert(to_ivec(b) == std::vector({1, 2, 3}));
        assert(a.size() == 0 && a.capacity() == 0 && a.data() == nullptr);
    }

    {
        fmt::println("\n# Copy assignment (replaces capacity too)");
        aligned_byte_buffer<16> a{1_b, 2_b, 3_b, 4_b, 5_b};
        aligned_byte_buffer<16> b(1);
        b = a;
        print_abb(b);
        assert(b.capacity() == 5);
        assert(to_ivec(b) == to_ivec(a));
        assert(a.data() != b.data());
    }

    {
        fmt::println("\n# Move assignment");
        aligned_byte_buffer<16> a{4_b, 5_b, 6_b};
        const std::byte* const orig = a.data();
        aligned_byte_buffer<16> b(1);
        b = std::move(a);
        print_abb(b);
        assert(b.data() == orig);
        assert(to_ivec(b) == std::vector({4, 5, 6}));
    }

    {
        fmt::println("\n# swap");
        aligned_byte_buffer<16> a{1_b, 2_b};
        aligned_byte_buffer<16> b{7_b, 8_b, 9_b};
        swap(a, b);
        print_abb(a);
        print_abb(b);
        assert(to_ivec(a) == std::vector({7, 8, 9}));
        assert(to_ivec(b) == std::vector({1, 2}));
    }

    // ---- Observers ----

    {
        fmt::println("\n# capacity()/max_size() report the runtime capacity (not SIZE_MAX)");
        const aligned_byte_buffer<16> v(10);
        assert(v.capacity() == 10 && v.max_size() == 10);
        assert(v.max_size() != std::numeric_limits<std::size_t>::max());
        fmt::println("capacity={} max_size={}", v.capacity(), v.max_size());
    }

    {
        fmt::println("\n# size()/remaining_space()/is_empty()/is_full()");
        aligned_byte_buffer<16> v(3);
        assert(v.is_empty() && !v.is_full() && v.size() == 0 && v.remaining_space() == 3);
        v.push_back(1_b);
        assert(!v.is_empty() && !v.is_full() && v.size() == 1 && v.remaining_space() == 2);
        v.push_back(2_b);
        v.push_back(3_b);
        assert(!v.is_empty() && v.is_full() && v.size() == 3 && v.remaining_space() == 0);
        print_abb(v);
    }

    // ---- Modifiers ----

    {
        fmt::println("\n# clear() (does not change capacity)");
        aligned_byte_buffer<16> v{1_b, 2_b, 3_b};
        v.clear();
        print_abb(v);
        assert(v.is_empty() && v.capacity() == 3);
    }

    {
        fmt::println("\n# resize() grow / shrink / single-arg");
        aligned_byte_buffer<16> v(5);
        v.resize(3, 7_b);
        assert(to_ivec(v) == std::vector({7, 7, 7}));
        v.resize(1); // shrink
        assert(to_ivec(v) == std::vector({7}));
        v.resize(4); // grow with std::byte{} == 0
        assert(to_ivec(v) == std::vector({7, 0, 0, 0}));
        print_abb(v);
    }

    {
        fmt::println("\n# pop_back() (incl. pop on empty)");
        aligned_byte_buffer<16> v{1_b, 2_b, 3_b};
        v.pop_back();
        assert(to_ivec(v) == std::vector({1, 2}));
        v.pop_back();
        v.pop_back();
        v.pop_back(); // no-op on empty
        assert(v.is_empty());
        print_abb(v);
    }

    {
        fmt::println("\n# push_back() / emplace_back() (emplace from an int)");
        aligned_byte_buffer<16> v(4);
        const std::byte x = 10_b;
        v.push_back(x);        // const&
        v.push_back(20_b);     // rvalue
        v.emplace_back(30);    // int -> std::byte via functional cast
        print_abb(v);
        assert(to_ivec(v) == std::vector({10, 20, 30}));
    }

    {
        fmt::println("\n# unchecked_push_back()/unchecked_emplace_back()");
        aligned_byte_buffer<16> v(3);
        v.unchecked_emplace_back(1);   // int
        v.unchecked_push_back(2_b);
        v.unchecked_emplace_back(3_b); // byte
        print_abb(v);
        assert(to_ivec(v) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# try_push_back()/try_emplace_back() (success then failure)");
        aligned_byte_buffer<16> v(2);
        assert(v.try_push_back(1_b));
        assert(v.try_emplace_back(2));
        assert(!v.try_push_back(3_b));   // full -> false, no throw
        assert(!v.try_emplace_back(4));
        print_abb(v);
        assert(to_ivec(v) == std::vector({1, 2}));
    }

    {
        fmt::println("\n# fill_capacity()/fill_size()");
        aligned_byte_buffer<16> v(5);
        v.append_range({1_b, 2_b, 3_b});
        v.fill_size(9_b); // only the live [0,size)
        print_abb(v);
        assert(to_ivec(v) == std::vector({9, 9, 9}));
        assert(!v.is_full());
        v.fill_capacity(4_b); // whole capacity, size := capacity
        print_abb(v);
        assert(to_ivec(v) == std::vector({4, 4, 4, 4, 4}));
        assert(v.is_full());
    }

    {
        fmt::println("\n# zeroize_remaining_space() (zero the reserved tail)");
        aligned_byte_buffer<16> v(8);
        v.append_range({1_b, 2_b, 3_b});
        v.zeroize_remaining_space(); // [size, capacity) is now zero; size unchanged
        assert(v.size() == 3 && v.capacity() == 8);
        assert(to_ivec(v) == std::vector({1, 2, 3}));
        // The tail bytes are now determinate (zero), so they may be asserted.
        for (std::size_t i = v.size(); i < v.capacity(); ++i)
            assert(v[i] == 0_b);
        print_abb(v);
        // Scrub the whole buffer: clear() + zeroize_remaining_space() (non-elidable stores).
        v.clear();
        v.zeroize_remaining_space();
        assert(v.is_empty() && v.capacity() == 8);
        for (std::size_t i = 0; i < v.capacity(); ++i)
            assert(v[i] == 0_b);
        print_abb(v);
    }

    // ---- append_range / try_append_range / assign_range ----

    {
        fmt::println("\n# append_range() overloads");
        constexpr std::array tail{4_b, 5_b};
        const std::vector more{6_b, 7_b};

        aligned_byte_buffer<16> v(12);
        v.append_range({1_b, 2_b, 3_b});                             // initializer_list
        v.append_range(std::span<const std::byte>{tail});           // span
        v.append_range(more.begin(), more.end());                   // iterator + sentinel
        v.append_range(more.begin(), std::size_t{1});               // iterator + count -> 6
        v.append_range(std::views::iota(8, 10) | std::views::transform(to_byte)); // range -> 8,9
        print_abb(v);
        assert(to_ivec(v) == std::vector({1, 2, 3, 4, 5, 6, 7, 6, 8, 9}));
    }

    {
        fmt::println("\n# try_append_range() (success then failure)");
        aligned_byte_buffer<16> v(4);
        constexpr std::array a{1_b, 2_b};
        assert(v.try_append_range(std::span<const std::byte>{a}));
        assert(v.try_append_range({3_b, 4_b}));
        assert(!v.try_append_range({5_b, 6_b}));   // would overflow -> false
        assert(!v.try_append_range(std::views::iota(0, 3) | std::views::transform(to_byte)));
        const std::vector vsrc{5_b, 6_b};
        assert(!v.try_append_range(vsrc.begin(), vsrc.end())); // sized sentinel: checked up front
        print_abb(v);
        assert(to_ivec(v) == std::vector({1, 2, 3, 4}));
    }

    {
        fmt::println("\n# assign_range() overloads (clear + append)");
        constexpr std::array arr{5_b, 6_b};
        const std::vector src{7_b, 8_b, 9_b};

        aligned_byte_buffer<16> v(6);
        v.append_range({1_b, 2_b, 3_b});
        v.assign_range(std::span<const std::byte>{arr});
        assert(to_ivec(v) == std::vector({5, 6}));
        v.assign_range(src.begin(), src.end());
        assert(to_ivec(v) == std::vector({7, 8, 9}));
        v.assign_range(src.begin(), std::size_t{2});
        assert(to_ivec(v) == std::vector({7, 8}));
        v.assign_range({1_b, 1_b});
        assert(to_ivec(v) == std::vector({1, 1}));
        v.assign_range(std::views::iota(10, 13) | std::views::transform(to_byte));
        assert(to_ivec(v) == std::vector({10, 11, 12}));
        print_abb(v);
    }

    // ---- Element access ----

    {
        fmt::println("\n# span() / operator std::span / data()");
        aligned_byte_buffer<16> v{1_b, 2_b, 3_b, 4_b};
        const std::span<const std::byte> s1 = v.span();
        const auto s2 = static_cast<std::span<std::byte>>(v);
        assert(s1.size() == 4 && s2.size() == 4);
        assert(v.data() == s1.data() && v.data() == s2.data());
        fmt::println("span bytes = {}",
                s1 | std::views::transform([](std::byte x) { return std::to_integer<unsigned>(x); }));
    }

    {
        fmt::println("\n# front()/back()");
        aligned_byte_buffer<16> v{10_b, 20_b, 30_b};
        assert(v.front() == 10_b && v.back() == 30_b);
        v.front() = 11_b;
        v.back() = 31_b;
        print_abb(v);
        assert(to_ivec(v) == std::vector({11, 20, 31}));
    }

    {
        fmt::println("\n# operator[] within size (beyond size is unspecified but not UB)");
        aligned_byte_buffer<16> v(5);
        v.append_range({11_b, 22_b, 33_b});
        assert(v[0] == 11_b && v[2] == 33_b);
        v[1] = 99_b;
        assert(v[1] == 99_b);
        // Exercise (but do not assert) a read at an index >= size(), within capacity.
        // For std::byte this is well-defined; the value is unspecified.
        const auto probe = std::to_integer<unsigned>(v[v.capacity() - 1]);
        (void)probe;
        fmt::println("v[0]={} v[1]={} v[2]={}",
                std::to_integer<unsigned>(v[0]), std::to_integer<unsigned>(v[1]),
                std::to_integer<unsigned>(v[2]));
    }

    {
        fmt::println("\n# at() valid then out_of_range");
        aligned_byte_buffer<16> v{1_b, 2_b, 3_b};
        assert(v.at(0) == 1_b && v.at(2) == 3_b);
        v.at(1) = 99_b;
        assert(v.at(1) == 99_b);
        expect_throw<std::out_of_range>("at(3)", [&] { (void)v.at(3); });
    }

    {
        fmt::println("\n# forward iteration (begin/end, cbegin/cend)");
        aligned_byte_buffer<16> v{1_b, 2_b, 3_b};
        int sum = 0;
        for (const std::byte e : v)
            sum += std::to_integer<int>(e);
        assert(sum == 6);
        assert(to_ivec(v) == std::vector({1, 2, 3}));
        assert(v.cbegin() == v.begin() && v.cend() == v.end());
    }

    {
        fmt::println("\n# reverse iteration (rbegin/rend, crbegin/crend)");
        aligned_byte_buffer<16> v{1_b, 2_b, 3_b};
        std::vector<int> rev;
        for (auto it = v.rbegin(); it != v.rend(); ++it)
            rev.push_back(std::to_integer<int>(*it));
        assert(rev == std::vector({3, 2, 1}));
        assert(v.crbegin() != v.crend());
        *v.rbegin() = 30_b; // back
        assert(to_ivec(v) == std::vector({1, 2, 30}));
    }

    // ---- Comparisons ----

    {
        fmt::println("\n# operator== / operator<=> (capacity is not part of equality)");
        aligned_byte_buffer<16> a(10);
        a.append_range({1_b, 2_b, 3_b});
        aligned_byte_buffer<16> b{1_b, 2_b, 3_b}; // capacity 3
        assert(a == b);                            // equal contents, different capacity
        assert(a.capacity() != b.capacity());

        aligned_byte_buffer<16> c{1_b, 2_b, 4_b};
        assert(a != c);
        assert(a < c);
        assert(c > a);
        assert((a <=> b) == std::strong_ordering::equal);
        fmt::println("a==b:{} a<c:{}", a == b, a < c);
    }

    // ---- Custom alignment / SIMD buffer (the motivating use case) ----

    {
        fmt::println("\n# Over-alignment honored for several Align values");
        const auto check_align = []<std::size_t A>()
        {
            aligned_byte_buffer<A> buf(64);
            buf.resize(A); // make it non-empty
            const auto off = reinterpret_cast<std::uintptr_t>(buf.data()) % A;
            fmt::println("Align={:>3}: data%Align={}", A, off);
            assert(off == 0);
        };
        check_align.template operator()<16>();
        check_align.template operator()<32>();
        check_align.template operator()<64>();
    }

    {
        fmt::println("\n# std::byte buffer -> std::span<const std::byte> for SIMD");
        aligned_byte_buffer<16> buf(1024);
        assert(buf.capacity() == 1024 && buf.is_empty());
        for (int i = 0; i < 16; ++i)
            buf.push_back(to_byte(i));
        const std::span<const std::byte> lane = buf.span();
        assert(lane.size() == 16);
        assert(reinterpret_cast<std::uintptr_t>(lane.data()) % 16 == 0);
        // On a NEON target this span feeds a load directly, e.g.:
        //   const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(lane.data()));
        print_abb(buf);
        assert(std::to_integer<unsigned>(lane[0]) == 0);
        assert(std::to_integer<unsigned>(lane[15]) == 15);
    }

    // ---- Overflow -> std::bad_alloc ----

    {
        fmt::println("\n# Capacity overflow throws std::bad_alloc");
        expect_throw<std::bad_alloc>("push_back when full", [] {
            aligned_byte_buffer<16> v(2);
            v.push_back(1_b);
            v.push_back(2_b);
            v.push_back(3_b);
        });
        expect_throw<std::bad_alloc>("emplace_back when full", [] {
            aligned_byte_buffer<16> v(1);
            v.emplace_back(1);
            v.emplace_back(2);
        });
        expect_throw<std::bad_alloc>("append_range(span) overflow", [] {
            aligned_byte_buffer<16> v(2);
            constexpr std::array a{1_b, 2_b, 3_b};
            v.append_range(std::span<const std::byte>{a});
        });
        expect_throw<std::bad_alloc>("resize beyond capacity", [] {
            aligned_byte_buffer<16> v(2);
            v.resize(3);
        });
        expect_throw<std::bad_alloc>("assign_range beyond capacity", [] {
            aligned_byte_buffer<16> v(2);
            v.assign_range({1_b, 2_b, 3_b});
        });
    }

    fmt::println("\nAll assertions passed.");
    return 0;
}

/*

Output:


# Default constructor
abb: span=[]  size=0  cap=0  remaining=0  align=16  data%align=0  is_empty=true  is_full=true  sizeof=24

# Capacity constructor (reserve, empty; tail left uninitialized)
abb: span=[]  size=0  cap=64  remaining=64  align=16  data%align=0  is_empty=true  is_full=false  sizeof=24

# Capacity + value constructor (filled to capacity)
abb: span=[66, 66, 66]  size=3  cap=3  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24

# std::span constructor
abb: span=[1, 2, 3]  size=3  cap=3  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24

# Forward iterator + sentinel constructor
abb: span=[1, 2, 3, 4]  size=4  cap=4  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24

# Iterator + count constructor
abb: span=[2, 3]  size=2  cap=2  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24

# initializer_list constructor
abb: span=[1, 2, 3]  size=3  cap=3  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24

# from_range constructor
abb: span=[1, 2, 3, 4]  size=4  cap=4  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24

# initializer_list assignment operator
abb: span=[7, 8, 9]  size=3  cap=5  remaining=2  align=16  data%align=0  is_empty=false  is_full=false  sizeof=24

# Copy constructor (deep copy of the live [0,size) bytes)
abb: span=[1, 2, 3]  size=3  cap=8  remaining=5  align=16  data%align=0  is_empty=false  is_full=false  sizeof=24

# Move constructor (source emptied)
abb: span=[1, 2, 3]  size=3  cap=3  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24

# Copy assignment (replaces capacity too)
abb: span=[1, 2, 3, 4, 5]  size=5  cap=5  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24

# Move assignment
abb: span=[4, 5, 6]  size=3  cap=3  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24

# swap
abb: span=[7, 8, 9]  size=3  cap=3  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24
abb: span=[1, 2]  size=2  cap=2  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24

# capacity()/max_size() report the runtime capacity (not SIZE_MAX)
capacity=10 max_size=10

# size()/remaining_space()/is_empty()/is_full()
abb: span=[1, 2, 3]  size=3  cap=3  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24

# clear() (does not change capacity)
abb: span=[]  size=0  cap=3  remaining=3  align=16  data%align=0  is_empty=true  is_full=false  sizeof=24

# resize() grow / shrink / single-arg
abb: span=[7, 0, 0, 0]  size=4  cap=5  remaining=1  align=16  data%align=0  is_empty=false  is_full=false  sizeof=24

# pop_back() (incl. pop on empty)
abb: span=[]  size=0  cap=3  remaining=3  align=16  data%align=0  is_empty=true  is_full=false  sizeof=24

# push_back() / emplace_back() (emplace from an int)
abb: span=[10, 20, 30]  size=3  cap=4  remaining=1  align=16  data%align=0  is_empty=false  is_full=false  sizeof=24

# unchecked_push_back()/unchecked_emplace_back()
abb: span=[1, 2, 3]  size=3  cap=3  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24

# try_push_back()/try_emplace_back() (success then failure)
abb: span=[1, 2]  size=2  cap=2  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24

# fill_capacity()/fill_size()
abb: span=[9, 9, 9]  size=3  cap=5  remaining=2  align=16  data%align=0  is_empty=false  is_full=false  sizeof=24
abb: span=[4, 4, 4, 4, 4]  size=5  cap=5  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24

# zeroize_remaining_space() (zero the reserved tail)
abb: span=[1, 2, 3]  size=3  cap=8  remaining=5  align=16  data%align=0  is_empty=false  is_full=false  sizeof=24
abb: span=[]  size=0  cap=8  remaining=8  align=16  data%align=0  is_empty=true  is_full=false  sizeof=24

# append_range() overloads
abb: span=[1, 2, 3, 4, 5, 6, 7, 6, 8, 9]  size=10  cap=12  remaining=2  align=16  data%align=0  is_empty=false  is_full=false  sizeof=24

# try_append_range() (success then failure)
abb: span=[1, 2, 3, 4]  size=4  cap=4  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24

# assign_range() overloads (clear + append)
abb: span=[10, 11, 12]  size=3  cap=6  remaining=3  align=16  data%align=0  is_empty=false  is_full=false  sizeof=24

# span() / operator std::span / data()
span bytes = [1, 2, 3, 4]

# front()/back()
abb: span=[11, 20, 31]  size=3  cap=3  remaining=0  align=16  data%align=0  is_empty=false  is_full=true  sizeof=24

# operator[] within size (beyond size is unspecified but not UB)
v[0]=11 v[1]=99 v[2]=33

# at() valid then out_of_range
Caught expected exception (at(3)): aligned_byte_buffer: index >= size

# forward iteration (begin/end, cbegin/cend)

# reverse iteration (rbegin/rend, crbegin/crend)

# operator== / operator<=> (capacity is not part of equality)
a==b:true a<c:true

# Over-alignment honored for several Align values
Align= 16: data%Align=0
Align= 32: data%Align=0
Align= 64: data%Align=0

# std::byte buffer -> std::span<const std::byte> for SIMD
abb: span=[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]  size=16  cap=1024  remaining=1008  align=16  data%align=0  is_empty=false  is_full=false  sizeof=24

# Capacity overflow throws std::bad_alloc
Caught expected exception (push_back when full): std::bad_alloc
Caught expected exception (emplace_back when full): std::bad_alloc
Caught expected exception (append_range(span) overflow): std::bad_alloc
Caught expected exception (resize beyond capacity): std::bad_alloc
Caught expected exception (assign_range beyond capacity): std::bad_alloc

All assertions passed.

*/
