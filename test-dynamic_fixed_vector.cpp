/*

gpp test-dynamic_fixed_vector.cpp && ./a.out

Sanitized:
g++ -std=gnu++26 -g -fsanitize=address,undefined test-dynamic_fixed_vector.cpp -lfmt -o a.san && ./a.san

*/

// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

#include "dynamic_fixed_vector.hpp"

#include <fmt/ranges.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <vector>

template <typename T, std::size_t A>
void
print_dfv(const dynamic_fixed_vector<T, A>& v)
{
    const auto align_off = reinterpret_cast<std::uintptr_t>(v.data()) % A;
    if constexpr (fmt::is_formattable<T>::value)
    {
        fmt::println("dfv: span={}  size={}  cap={}  remaining={}  align={}  data%align={}  is_empty={}  is_full={}  sizeof={}",
                v.span(), v.size(), v.capacity(), v.remaining_space(), A, align_off,
                v.is_empty(), v.is_full(), sizeof(v));
    }
    else
    {
        // e.g. std::byte is not directly fmt-formattable; show it as integers.
        const auto as_ints =
            v.span() | std::views::transform([](const T& x) { return std::to_integer<unsigned>(x); });
        fmt::println("dfv: span={}  size={}  cap={}  remaining={}  align={}  data%align={}  is_empty={}  is_full={}  sizeof={}",
                as_ints, v.size(), v.capacity(), v.remaining_space(), A, align_off,
                v.is_empty(), v.is_full(), sizeof(v));
    }
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

template <typename V>
std::vector<int>
to_ivec(const V& v)
{
    return std::vector<int>(std::from_range, v.span());
}

// Compile-time check: empty / zero-capacity instances are usable in constant expressions.
// (The allocating paths are not, since over-aligned allocation is not usable in constant
// evaluation -- so only the non-allocating members are exercised here.)
constexpr bool
constexpr_empty_ok()
{
    dynamic_fixed_vector<int> a;    // default ctor (no allocation)
    dynamic_fixed_vector<int> b(0); // zero-capacity ctor (no allocation)
    if (!(a.is_empty() && a.size() == 0 && a.remaining_space() == 0))
        return false;
    if (!(b.capacity() == 0 && b.is_full()))
        return false;
    if (a.try_push_back(1)) // capacity 0 -> full -> false, must not throw
        return false;
    b.clear();
    b.pop_back();
    b.zeroize_remaining_space(); // no reserved tail on a zero-capacity vector -> no-op
    dynamic_fixed_vector<int> c;
    swap(b, c);
    return b.size() == 0 && b == c;
}
static_assert(constexpr_empty_ok());

int main()
{
    using namespace std::literals;

    // ---- Constructors ----

    {
        fmt::println("\n# Default constructor");
        const dynamic_fixed_vector<int> v;
        print_dfv(v);
        assert(v.size() == 0);
        assert(v.capacity() == 0);
        assert(v.is_empty());
        assert(v.data() == nullptr);
    }

    {
        fmt::println("\n# Capacity constructor (reserve, empty)");
        const dynamic_fixed_vector<int> v(5);
        print_dfv(v);
        assert(v.size() == 0);
        assert(v.capacity() == 5);
        assert(v.remaining_space() == 5);
        assert(v.is_empty());
        assert(!v.is_full());
    }

    {
        fmt::println("\n# Capacity + value constructor (filled to capacity)");
        const dynamic_fixed_vector<int> v(3, 42);
        print_dfv(v);
        assert(v.size() == 3);
        assert(v.capacity() == 3);
        assert(v.is_full());
        assert(to_ivec(v) == std::vector({42, 42, 42}));
    }

    {
        fmt::println("\n# std::span constructor");
        constexpr std::array arr{1, 2, 3};
        const dynamic_fixed_vector<int> v(std::span<const int>{arr});
        print_dfv(v);
        assert(v.capacity() == 3);
        assert(to_ivec(v) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# Forward iterator + sentinel constructor");
        const std::vector src{1, 2, 3, 4};
        const dynamic_fixed_vector<int> v(src.begin(), src.end());
        print_dfv(v);
        assert(v.capacity() == 4);
        assert(to_ivec(v) == std::vector({1, 2, 3, 4}));
    }

    {
        fmt::println("\n# Iterator + count constructor");
        const std::vector src{1, 2, 3, 4};
        const dynamic_fixed_vector<int> v(src.begin() + 1, 2);
        print_dfv(v);
        assert(v.capacity() == 2);
        assert(to_ivec(v) == std::vector({2, 3}));
    }

    {
        fmt::println("\n# initializer_list constructor");
        const dynamic_fixed_vector<int> v{1, 2, 3};
        print_dfv(v);
        assert(v.capacity() == 3);
        assert(to_ivec(v) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# from_range constructor");
        const dynamic_fixed_vector<int> v(std::from_range, std::views::iota(1, 5));
        print_dfv(v);
        assert(v.capacity() == 4);
        assert(to_ivec(v) == std::vector({1, 2, 3, 4}));
    }

    {
        fmt::println("\n# initializer_list assignment operator");
        dynamic_fixed_vector<int> v(5);
        v = {7, 8, 9};
        print_dfv(v);
        assert(v.capacity() == 5); // capacity unchanged by assign
        assert(to_ivec(v) == std::vector({7, 8, 9}));
    }

    // ---- Copy / move / swap ----

    {
        fmt::println("\n# Copy constructor (deep copy)");
        dynamic_fixed_vector<int> a(4);
        a.append_range({1, 2, 3});
        dynamic_fixed_vector<int> b = a;
        print_dfv(b);
        assert(a.data() != b.data()); // independent buffers
        assert(b.capacity() == 4);
        assert(to_ivec(a) == to_ivec(b));
        a[0] = 99;
        assert(b[0] == 1); // mutation of a does not affect b
    }

    {
        fmt::println("\n# Move constructor (source emptied)");
        dynamic_fixed_vector<int> a{1, 2, 3};
        const int* const orig = a.data();
        dynamic_fixed_vector<int> b = std::move(a);
        print_dfv(b);
        assert(b.data() == orig); // buffer transferred, not reallocated
        assert(to_ivec(b) == std::vector({1, 2, 3}));
        assert(a.size() == 0 && a.capacity() == 0 && a.data() == nullptr);
    }

    {
        fmt::println("\n# Copy assignment (replaces capacity too)");
        dynamic_fixed_vector<int> a{1, 2, 3, 4, 5};
        dynamic_fixed_vector<int> b(1);
        b = a;
        print_dfv(b);
        assert(b.capacity() == 5);
        assert(to_ivec(b) == to_ivec(a));
        assert(a.data() != b.data());
    }

    {
        fmt::println("\n# Move assignment");
        dynamic_fixed_vector<int> a{4, 5, 6};
        const int* const orig = a.data();
        dynamic_fixed_vector<int> b(1);
        b = std::move(a);
        print_dfv(b);
        assert(b.data() == orig);
        assert(to_ivec(b) == std::vector({4, 5, 6}));
    }

    {
        fmt::println("\n# swap");
        dynamic_fixed_vector<int> a{1, 2};
        dynamic_fixed_vector<int> b{7, 8, 9};
        swap(a, b);
        print_dfv(a);
        print_dfv(b);
        assert(to_ivec(a) == std::vector({7, 8, 9}));
        assert(to_ivec(b) == std::vector({1, 2}));
    }

    // ---- Observers ----

    {
        fmt::println("\n# capacity()/max_size() report the runtime capacity (not SIZE_MAX)");
        const dynamic_fixed_vector<int> v(10);
        assert(v.capacity() == 10);
        assert(v.max_size() == 10);
        assert(v.max_size() != std::numeric_limits<std::size_t>::max());
        fmt::println("capacity={} max_size={}", v.capacity(), v.max_size());
    }

    {
        fmt::println("\n# size()/remaining_space()/is_empty()/is_full()");
        dynamic_fixed_vector<int> v(3);
        assert(v.is_empty() && !v.is_full() && v.size() == 0 && v.remaining_space() == 3);
        v.push_back(1);
        assert(!v.is_empty() && !v.is_full() && v.size() == 1 && v.remaining_space() == 2);
        v.push_back(2);
        v.push_back(3);
        assert(!v.is_empty() && v.is_full() && v.size() == 3 && v.remaining_space() == 0);
        print_dfv(v);
    }

    // ---- Modifiers ----

    {
        fmt::println("\n# clear() (does not change capacity)");
        dynamic_fixed_vector<int> v{1, 2, 3};
        v.clear();
        print_dfv(v);
        assert(v.is_empty() && v.capacity() == 3);
    }

    {
        fmt::println("\n# resize() grow / shrink / single-arg");
        dynamic_fixed_vector<int> v(5);
        v.resize(3, 7);
        assert(to_ivec(v) == std::vector({7, 7, 7}));
        v.resize(1); // shrink, no destruction
        assert(to_ivec(v) == std::vector({7}));
        v.resize(4); // grow with T{} == 0
        assert(to_ivec(v) == std::vector({7, 0, 0, 0}));
        print_dfv(v);
    }

    {
        fmt::println("\n# pop_back() (incl. pop on empty)");
        dynamic_fixed_vector<int> v{1, 2, 3};
        v.pop_back();
        assert(to_ivec(v) == std::vector({1, 2}));
        v.pop_back();
        v.pop_back();
        v.pop_back(); // pop on empty is a no-op
        assert(v.is_empty());
        print_dfv(v);
    }

    {
        fmt::println("\n# push_back() lvalue & rvalue");
        dynamic_fixed_vector<int> v(3);
        const int x = 10;
        v.push_back(x);      // const&
        v.push_back(20);     // &&
        print_dfv(v);
        assert(to_ivec(v) == std::vector({10, 20}));
    }

    {
        fmt::println("\n# emplace_back()");
        dynamic_fixed_vector<int> v(2);
        v.emplace_back(5);
        v.emplace_back(6);
        print_dfv(v);
        assert(to_ivec(v) == std::vector({5, 6}));
    }

    {
        fmt::println("\n# unchecked_push_back()/unchecked_emplace_back()");
        dynamic_fixed_vector<int> v(3);
        v.unchecked_emplace_back(1);
        v.unchecked_push_back(2);
        int y = 3;
        v.unchecked_push_back(std::move(y));
        print_dfv(v);
        assert(to_ivec(v) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# try_push_back()/try_emplace_back() (success then failure)");
        dynamic_fixed_vector<int> v(2);
        assert(v.try_push_back(1));
        assert(v.try_emplace_back(2));
        assert(!v.try_push_back(3));   // full -> false, no throw
        assert(!v.try_emplace_back(4));
        print_dfv(v);
        assert(to_ivec(v) == std::vector({1, 2}));
    }

    {
        fmt::println("\n# fill_capacity()/fill_size()");
        dynamic_fixed_vector<int> v(5);
        v.append_range({1, 2, 3});
        v.fill_size(9); // only the live [0,size)
        print_dfv(v);
        assert(to_ivec(v) == std::vector({9, 9, 9}));
        assert(!v.is_full());
        v.fill_capacity(4); // whole capacity, size := capacity
        print_dfv(v);
        assert(to_ivec(v) == std::vector({4, 4, 4, 4, 4}));
        assert(v.is_full());
    }

    {
        fmt::println("\n# zeroize_remaining_space() (zero the reserved tail)");
        dynamic_fixed_vector<int> v(5);
        v.fill_capacity(9);
        v.resize(2); // the tail slots [2, 5) still hold 9
        v.zeroize_remaining_space();
        assert(v.size() == 2 && v.capacity() == 5);
        assert(to_ivec(v) == std::vector({9, 9}));
        // operator[] is capacity-based: the tail is now zero
        for (std::size_t i = v.size(); i < v.capacity(); ++i)
            assert(v[i] == 0);
        print_dfv(v);
        // Scrub the whole buffer: clear() + zeroize_remaining_space() (non-elidable stores).
        v.clear();
        v.zeroize_remaining_space();
        assert(v.is_empty());
        for (std::size_t i = 0; i < v.capacity(); ++i)
            assert(v[i] == 0);
        print_dfv(v);
    }

    // ---- append_range / try_append_range / assign_range ----

    {
        fmt::println("\n# append_range() overloads");
        constexpr std::array tail{4, 5};
        const std::vector more{6, 7};

        dynamic_fixed_vector<int> v(12);
        v.append_range({1, 2, 3});                                 // initializer_list
        v.append_range(std::span<const int>{tail});               // span
        v.append_range(more.begin(), more.end());                 // iterator + sentinel
        v.append_range(more.begin(), std::size_t{1});             // iterator + count -> 6
        v.append_range(std::views::iota(8, 10));                  // range -> 8,9
        print_dfv(v);
        assert(to_ivec(v) == std::vector({1, 2, 3, 4, 5, 6, 7, 6, 8, 9}));
    }

    {
        fmt::println("\n# try_append_range() (success then failure)");
        dynamic_fixed_vector<int> v(4);
        constexpr std::array a{1, 2};
        assert(v.try_append_range(std::span<const int>{a}));
        assert(v.try_append_range({3, 4}));
        assert(!v.try_append_range({5, 6}));            // would overflow -> false
        assert(!v.try_append_range(std::views::iota(0, 3)));
        print_dfv(v);
        assert(to_ivec(v) == std::vector({1, 2, 3, 4}));
    }

    {
        fmt::println("\n# assign_range() overloads (clear + append)");
        constexpr std::array arr{5, 6};
        const std::vector src{7, 8, 9};

        dynamic_fixed_vector<int> v(6);
        v.append_range({1, 2, 3});
        v.assign_range(std::span<const int>{arr});
        assert(to_ivec(v) == std::vector({5, 6}));
        v.assign_range(src.begin(), src.end());
        assert(to_ivec(v) == std::vector({7, 8, 9}));
        v.assign_range(src.begin(), std::size_t{2});
        assert(to_ivec(v) == std::vector({7, 8}));
        v.assign_range({1, 1});
        assert(to_ivec(v) == std::vector({1, 1}));
        v.assign_range(std::views::iota(10, 13));
        assert(to_ivec(v) == std::vector({10, 11, 12}));
        print_dfv(v);
    }

    // ---- Element access ----

    {
        fmt::println("\n# span() / operator std::span / data()");
        dynamic_fixed_vector<int> v{1, 2, 3, 4};
        const std::span<const int> s1 = v.span();
        const auto s2 = static_cast<std::span<int>>(v);
        assert(s1.size() == 4 && s2.size() == 4);
        assert(v.data() == s1.data() && v.data() == s2.data());
        fmt::println("span={}", s1);
    }

    {
        fmt::println("\n# front()/back()");
        dynamic_fixed_vector<int> v{10, 20, 30};
        assert(v.front() == 10 && v.back() == 30);
        v.front() = 11;
        v.back() = 31;
        print_dfv(v);
        assert(to_ivec(v) == std::vector({11, 20, 31}));
    }

    {
        fmt::println("\n# operator[] incl. reading beyond size within capacity");
        dynamic_fixed_vector<int> v(5); // all 5 value-initialized to 0
        v.append_range({11, 22, 33});
        assert(v[0] == 11 && v[2] == 33);
        // index 3 and 4 are >= size() but < capacity(): value-initialized storage.
        assert(v[3] == 0 && v[4] == 0);
        fmt::println("v[0]={} v[2]={} v[4] (beyond size)={}", v[0], v[2], v[4]);
    }

    {
        fmt::println("\n# at() valid then out_of_range");
        dynamic_fixed_vector<int> v{1, 2, 3};
        assert(v.at(0) == 1 && v.at(2) == 3);
        v.at(1) = 99;
        assert(v.at(1) == 99);
        expect_throw<std::out_of_range>("at(3)", [&] { (void)v.at(3); });
    }

    {
        fmt::println("\n# forward iteration (begin/end, cbegin/cend)");
        dynamic_fixed_vector<int> v{1, 2, 3};
        int sum = 0;
        for (const int e : v)
            sum += e;
        assert(sum == 6);
        assert(std::vector<int>(v.cbegin(), v.cend()) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# reverse iteration (rbegin/rend, crbegin/crend)");
        dynamic_fixed_vector<int> v{1, 2, 3};
        assert(std::vector<int>(v.rbegin(), v.rend()) == std::vector({3, 2, 1}));
        assert(std::vector<int>(v.crbegin(), v.crend()) == std::vector({3, 2, 1}));
        *v.rbegin() = 30; // back
        assert(to_ivec(v) == std::vector({1, 2, 30}));
    }

    // ---- Comparisons ----

    {
        fmt::println("\n# operator== / operator<=> (capacity is not part of equality)");
        dynamic_fixed_vector<int> a(10);
        a.append_range({1, 2, 3});
        dynamic_fixed_vector<int> b{1, 2, 3}; // capacity 3
        assert(a == b);                       // equal contents, different capacity
        assert(a.capacity() != b.capacity());

        dynamic_fixed_vector<int> c{1, 2, 4};
        assert(a != c);
        assert(a < c);
        assert(c > a);
        assert((a <=> b) == std::strong_ordering::equal);
        fmt::println("a==b:{} a<c:{}", a == b, a < c);
    }

    // ---- Custom alignment / byte buffer (the motivating use case) ----

    {
        fmt::println("\n# Over-alignment honored for several Align values");
        const auto check_align = []<std::size_t A>()
        {
            dynamic_fixed_vector<std::byte, A> buf(64);
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
        dynamic_fixed_vector<std::byte, 16> buf(1024);
        assert(buf.capacity() == 1024 && buf.is_empty());
        for (int i = 0; i < 16; ++i)
            buf.push_back(static_cast<std::byte>(i));
        const std::span<const std::byte> lane = buf.span();
        assert(lane.size() == 16);
        assert(reinterpret_cast<std::uintptr_t>(lane.data()) % 16 == 0);
        // On a NEON target this span feeds a load directly, e.g.:
        //   const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(lane.data()));
        print_dfv(buf);
        assert(std::to_integer<unsigned>(lane[0]) == 0);
        assert(std::to_integer<unsigned>(lane[15]) == 15);
    }

    // ---- Overflow -> std::bad_alloc ----

    {
        fmt::println("\n# Capacity overflow throws std::bad_alloc");
        expect_throw<std::bad_alloc>("push_back when full", [] {
            dynamic_fixed_vector<int> v(2);
            v.push_back(1);
            v.push_back(2);
            v.push_back(3);
        });
        expect_throw<std::bad_alloc>("emplace_back when full", [] {
            dynamic_fixed_vector<int> v(1);
            v.emplace_back(1);
            v.emplace_back(2);
        });
        expect_throw<std::bad_alloc>("append_range(span) overflow", [] {
            dynamic_fixed_vector<int> v(2);
            constexpr std::array a{1, 2, 3};
            v.append_range(std::span<const int>{a});
        });
        expect_throw<std::bad_alloc>("resize beyond capacity", [] {
            dynamic_fixed_vector<int> v(2);
            v.resize(3);
        });
        expect_throw<std::bad_alloc>("assign_range beyond capacity", [] {
            dynamic_fixed_vector<int> v(2);
            v.assign_range({1, 2, 3});
        });
    }

    fmt::println("\nAll assertions passed.");
    return 0;
}

/*

Output:


# Default constructor
dfv: span=[]  size=0  cap=0  remaining=0  align=4  data%align=0  is_empty=true  is_full=true  sizeof=24

# Capacity constructor (reserve, empty)
dfv: span=[]  size=0  cap=5  remaining=5  align=4  data%align=0  is_empty=true  is_full=false  sizeof=24

# Capacity + value constructor (filled to capacity)
dfv: span=[42, 42, 42]  size=3  cap=3  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# std::span constructor
dfv: span=[1, 2, 3]  size=3  cap=3  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# Forward iterator + sentinel constructor
dfv: span=[1, 2, 3, 4]  size=4  cap=4  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# Iterator + count constructor
dfv: span=[2, 3]  size=2  cap=2  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# initializer_list constructor
dfv: span=[1, 2, 3]  size=3  cap=3  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# from_range constructor
dfv: span=[1, 2, 3, 4]  size=4  cap=4  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# initializer_list assignment operator
dfv: span=[7, 8, 9]  size=3  cap=5  remaining=2  align=4  data%align=0  is_empty=false  is_full=false  sizeof=24

# Copy constructor (deep copy)
dfv: span=[1, 2, 3]  size=3  cap=4  remaining=1  align=4  data%align=0  is_empty=false  is_full=false  sizeof=24

# Move constructor (source emptied)
dfv: span=[1, 2, 3]  size=3  cap=3  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# Copy assignment (replaces capacity too)
dfv: span=[1, 2, 3, 4, 5]  size=5  cap=5  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# Move assignment
dfv: span=[4, 5, 6]  size=3  cap=3  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# swap
dfv: span=[7, 8, 9]  size=3  cap=3  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24
dfv: span=[1, 2]  size=2  cap=2  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# capacity()/max_size() report the runtime capacity (not SIZE_MAX)
capacity=10 max_size=10

# size()/remaining_space()/is_empty()/is_full()
dfv: span=[1, 2, 3]  size=3  cap=3  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# clear() (does not change capacity)
dfv: span=[]  size=0  cap=3  remaining=3  align=4  data%align=0  is_empty=true  is_full=false  sizeof=24

# resize() grow / shrink / single-arg
dfv: span=[7, 0, 0, 0]  size=4  cap=5  remaining=1  align=4  data%align=0  is_empty=false  is_full=false  sizeof=24

# pop_back() (incl. pop on empty)
dfv: span=[]  size=0  cap=3  remaining=3  align=4  data%align=0  is_empty=true  is_full=false  sizeof=24

# push_back() lvalue & rvalue
dfv: span=[10, 20]  size=2  cap=3  remaining=1  align=4  data%align=0  is_empty=false  is_full=false  sizeof=24

# emplace_back()
dfv: span=[5, 6]  size=2  cap=2  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# unchecked_push_back()/unchecked_emplace_back()
dfv: span=[1, 2, 3]  size=3  cap=3  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# try_push_back()/try_emplace_back() (success then failure)
dfv: span=[1, 2]  size=2  cap=2  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# fill_capacity()/fill_size()
dfv: span=[9, 9, 9]  size=3  cap=5  remaining=2  align=4  data%align=0  is_empty=false  is_full=false  sizeof=24
dfv: span=[4, 4, 4, 4, 4]  size=5  cap=5  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# zeroize_remaining_space() (zero the reserved tail)
dfv: span=[9, 9]  size=2  cap=5  remaining=3  align=4  data%align=0  is_empty=false  is_full=false  sizeof=24
dfv: span=[]  size=0  cap=5  remaining=5  align=4  data%align=0  is_empty=true  is_full=false  sizeof=24

# append_range() overloads
dfv: span=[1, 2, 3, 4, 5, 6, 7, 6, 8, 9]  size=10  cap=12  remaining=2  align=4  data%align=0  is_empty=false  is_full=false  sizeof=24

# try_append_range() (success then failure)
dfv: span=[1, 2, 3, 4]  size=4  cap=4  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# assign_range() overloads (clear + append)
dfv: span=[10, 11, 12]  size=3  cap=6  remaining=3  align=4  data%align=0  is_empty=false  is_full=false  sizeof=24

# span() / operator std::span / data()
span=[1, 2, 3, 4]

# front()/back()
dfv: span=[11, 20, 31]  size=3  cap=3  remaining=0  align=4  data%align=0  is_empty=false  is_full=true  sizeof=24

# operator[] incl. reading beyond size within capacity
v[0]=11 v[2]=33 v[4] (beyond size)=0

# at() valid then out_of_range
Caught expected exception (at(3)): dynamic_fixed_vector: index >= size

# forward iteration (begin/end, cbegin/cend)

# reverse iteration (rbegin/rend, crbegin/crend)

# operator== / operator<=> (capacity is not part of equality)
a==b:true a<c:true

# Over-alignment honored for several Align values
Align= 16: data%Align=0
Align= 32: data%Align=0
Align= 64: data%Align=0

# std::byte buffer -> std::span<const std::byte> for SIMD
dfv: span=[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]  size=16  cap=1024  remaining=1008  align=16  data%align=0  is_empty=false  is_full=false  sizeof=24

# Capacity overflow throws std::bad_alloc
Caught expected exception (push_back when full): std::bad_alloc
Caught expected exception (emplace_back when full): std::bad_alloc
Caught expected exception (append_range(span) overflow): std::bad_alloc
Caught expected exception (resize beyond capacity): std::bad_alloc
Caught expected exception (assign_range beyond capacity): std::bad_alloc

All assertions passed.

*/
