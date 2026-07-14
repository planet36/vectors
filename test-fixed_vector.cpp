/*

gpp test-fixed_vector.cpp && ./a.out

*/

// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

#include "fixed_vector.hpp"

#include <fmt/ranges.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <ranges>
#include <string_view>
#include <vector>

constexpr auto is_odd = [](const int x) { return x % 2 != 0; };

template <typename T, std::size_t N, std::size_t A>
void
print_fv(const fixed_vector<T, N, A>& v)
{
    const auto align_off = reinterpret_cast<std::uintptr_t>(v.data()) % A;
    if constexpr (fmt::is_formattable<T>::value)
    {
        fmt::println("fv: span={}  size={}  cap={}  remaining={}  align={}  data%align={}  is_empty={}  is_full={}  sizeof={}",
                v.span(), v.size(), v.capacity(), v.remaining_space(), A, align_off,
                v.is_empty(), v.is_full(), sizeof(v));
    }
    else
    {
        // e.g. std::byte is not directly fmt-formattable; show it as integers.
        const auto as_ints =
            v.span() | std::views::transform([](const T& x) { return std::to_integer<unsigned>(x); });
        fmt::println("fv: span={}  size={}  cap={}  remaining={}  align={}  data%align={}  is_empty={}  is_full={}  sizeof={}",
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

// Compile-time check: nearly the whole interface is usable in constant expressions.
// Unlike the heap-backed siblings -- whose over-aligned allocation is not usable in constant
// evaluation, so their static_assert can only reach the empty/zero-capacity members --
// fixed_vector's in-place std::array storage imposes no such limit.  A semantic regression in
// any member exercised here therefore fails the compile, not just the run.
constexpr bool
constexpr_api_ok()
{
    fixed_vector<int, 8> v;
    if (!(v.is_empty() && v.size() == 0 && v.remaining_space() == 8))
        return false;

    v.append_range({1, 2, 3});
    v.push_back(4);
    v.emplace_back(5);
    if (!v.try_push_back(6))
        return false;
    if (!v.try_emplace_back(7))
        return false;
    v.unchecked_push_back(8);
    if (!(v.is_full() && v.size() == 8))
        return false;
    if (v.try_push_back(9)) // full -> false, must not throw
        return false;

    if (!(v.front() == 1 && v.back() == 8 && v.at(2) == 3 && v[7] == 8))
        return false;

    v.assign_range({9, 9});
    v.resize(4, 7);
    if (!(v.size() == 4 && v[0] == 9 && v[1] == 9 && v[2] == 7 && v[3] == 7))
        return false;

    v.pop_back();
    v.fill_size(1);
    if (!(v.size() == 3 && v[0] == 1 && v[2] == 1))
        return false;

    v.clear();
    // Never destroyed: clear() only reset size(), so the elements still read back.
    if (!(v.is_empty() && v[0] == 1))
        return false;

    fixed_vector<int, 8> w{4, 5, 6};
    swap(v, w); // hidden friend
    if (!(v.size() == 3 && w.is_empty()))
        return false;
    v.swap(w); // member
    if (!(w.size() == 3 && v.is_empty()))
        return false;

    return true;
}
static_assert(constexpr_api_ok());

// Compile-time check: zeroize_remaining_space() is usable in constant expressions (the
// runtime explicit-zeroing path is replaced by value-assignment during constant evaluation).
constexpr bool
constexpr_zeroize_ok()
{
    fixed_vector<int, 5> v;
    v.fill_capacity(9);
    v.resize(2); // the tail slots [2, 5) still hold 9
    v.zeroize_remaining_space();
    // operator[] is capacity-based: the tail is now zero
    return v.size() == 2 && v[0] == 9 && v[1] == 9 && v[2] == 0 && v[3] == 0 && v[4] == 0;
}
static_assert(constexpr_zeroize_ok());

// capacity()/max_size() are static here -- callable with no object, unlike the heap-backed
// siblings, where they are non-static and report the runtime capacity.
static_assert(fixed_vector<int, 5>::capacity() == 5);
static_assert(fixed_vector<int, 5>::max_size() == 5);

// Align defaults to max(alignof(std::size_t), alignof(T)) and is honored by the storage.
static_assert(alignof(fixed_vector<int, 5>) == alignof(std::size_t));
static_assert(alignof(fixed_vector<std::byte, 64, 32>) == 32);

// Contiguous-range conformance (what lets the std algorithms below work on it).
static_assert(std::ranges::contiguous_range<fixed_vector<int, 5>>);
static_assert(std::ranges::sized_range<fixed_vector<int, 5>>);

int main()
{
    // ---- Constructors ----

    {
        fmt::println("\n# Default constructor");
        const fixed_vector<int, 5> v;
        print_fv(v);
        assert(v.size() == 0);
        assert(v.capacity() == 5);
        assert(v.is_empty());
        assert(!v.is_full());
        assert(v.data() != nullptr); // in-place storage: never null, unlike the heap-backed siblings
    }

    {
        fmt::println("\n# Count constructor (creates count value-initialized elements)");
        // Unlike the heap-backed siblings, where X(n) reserves capacity n and starts empty.
        const fixed_vector<int, 5> v(3);
        print_fv(v);
        assert(v.size() == 3);
        assert(v.capacity() == 5);
        assert(to_ivec(v) == std::vector({0, 0, 0}));
    }

    {
        fmt::println("\n# Count + value constructor");
        const fixed_vector<int, 5> v(3, 42);
        print_fv(v);
        assert(v.size() == 3);
        assert(!v.is_full()); // capacity is N, not count
        assert(to_ivec(v) == std::vector({42, 42, 42}));
    }

    {
        fmt::println("\n# std::span constructor");
        constexpr std::array arr{1, 2, 3};
        const fixed_vector<int, 5> v(std::span<const int>{arr});
        print_fv(v);
        assert(to_ivec(v) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# Iterator + sentinel constructor");
        const std::vector src{1, 2, 3, 4};
        const fixed_vector<int, 5> v(src.begin(), src.end());
        print_fv(v);
        assert(to_ivec(v) == std::vector({1, 2, 3, 4}));
    }

    {
        fmt::println("\n# Iterator + count constructor");
        const std::vector src{1, 2, 3, 4};
        const fixed_vector<int, 5> v(src.begin() + 1, 2);
        print_fv(v);
        assert(to_ivec(v) == std::vector({2, 3}));
    }

    {
        fmt::println("\n# initializer_list constructor");
        const fixed_vector<int, 5> v{1, 2, 3};
        print_fv(v);
        assert(to_ivec(v) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# from_range constructor (sized range)");
        const fixed_vector<int, 5> v(std::from_range, std::views::iota(1, 5));
        print_fv(v);
        assert(to_ivec(v) == std::vector({1, 2, 3, 4}));
    }

    {
        fmt::println("\n# from_range constructor (unsized range, appended element-wise)");
        // A filter_view is not a sized_range, so there is no up-front size check -- unlike the
        // heap-backed siblings, whose range constructors require forward iterators.
        const fixed_vector<int, 5> v(std::from_range,
                std::views::iota(1, 10) | std::views::filter(is_odd));
        print_fv(v);
        assert(to_ivec(v) == std::vector({1, 3, 5, 7, 9}));
    }

    {
        fmt::println("\n# initializer_list assignment operator");
        fixed_vector<int, 5> v(5, 1);
        v = {7, 8, 9};
        print_fv(v);
        assert(to_ivec(v) == std::vector({7, 8, 9}));
    }

    // ---- Copy / move / swap ----

    {
        fmt::println("\n# Copy constructor");
        fixed_vector<int, 5> a{1, 2, 3};
        fixed_vector<int, 5> b = a;
        print_fv(b);
        assert(to_ivec(a) == to_ivec(b));
        a[0] = 99;
        assert(b[0] == 1); // mutation of a does not affect b
    }

    {
        fmt::println("\n# Move constructor (source is NOT emptied)");
        fixed_vector<int, 5> a{1, 2, 3};
        const fixed_vector<int, 5> b = std::move(a);
        print_fv(b);
        assert(to_ivec(b) == std::vector({1, 2, 3}));
        // Copy and move are member-wise (defaulted): for a trivially copyable T a moved-from
        // fixed_vector is left unchanged -- unlike the heap-backed siblings, where move
        // construction transfers the buffer and leaves the source empty.
        assert(a.size() == 3);
        assert(to_ivec(a) == std::vector({1, 2, 3}));
        print_fv(a);
    }

    {
        fmt::println("\n# Copy assignment");
        const fixed_vector<int, 5> a{1, 2, 3, 4, 5};
        fixed_vector<int, 5> b;
        b = a;
        print_fv(b);
        assert(to_ivec(b) == to_ivec(a));
    }

    {
        fmt::println("\n# Move assignment");
        fixed_vector<int, 5> a{4, 5, 6};
        fixed_vector<int, 5> b;
        b = std::move(a);
        print_fv(b);
        assert(to_ivec(b) == std::vector({4, 5, 6}));
    }

    {
        fmt::println("\n# swap (hidden friend + member)");
        fixed_vector<int, 5> a{1, 2};
        fixed_vector<int, 5> b{7, 8, 9};
        swap(a, b); // hidden friend
        print_fv(a);
        print_fv(b);
        assert(to_ivec(a) == std::vector({7, 8, 9}));
        assert(to_ivec(b) == std::vector({1, 2}));
        a.swap(b); // member
        assert(to_ivec(a) == std::vector({1, 2}));
        assert(to_ivec(b) == std::vector({7, 8, 9}));
    }

    {
        fmt::println("\n# swap exchanges all max_size() slots, not just the live elements");
        fixed_vector<int, 5> a;
        a.fill_capacity(4); // every slot, including the tail, holds 4
        a.resize(2);
        fixed_vector<int, 5> b{9};
        swap(a, b);
        print_fv(a);
        print_fv(b);
        assert(b[4] == 4); // b received a's tail slot
        assert(a[4] == 0); // a received b's value-initialized tail
    }

    // ---- Observers ----

    {
        fmt::println("\n# capacity()/max_size() are static (no object needed)");
        const fixed_vector<int, 5> v{1, 2, 3};
        assert(v.capacity() == 5 && v.max_size() == 5);
        assert((fixed_vector<int, 5>::capacity() == 5)); // extra parens: assert is a macro
        assert((fixed_vector<int, 5>::max_size() == 5));
        fmt::println("capacity={} max_size={}", v.capacity(), v.max_size());
    }

    {
        fmt::println("\n# size()/remaining_space()/is_empty()/is_full()");
        fixed_vector<int, 3> v;
        assert(v.is_empty() && !v.is_full() && v.size() == 0 && v.remaining_space() == 3);
        v.push_back(1);
        assert(!v.is_empty() && !v.is_full() && v.size() == 1 && v.remaining_space() == 2);
        v.push_back(2);
        v.push_back(3);
        assert(!v.is_empty() && v.is_full() && v.size() == 3 && v.remaining_space() == 0);
        print_fv(v);
    }

    // ---- Modifiers ----

    {
        fmt::println("\n# clear() only resets size(); the elements stay alive");
        fixed_vector<int, 5> v{1, 2, 3};
        v.clear();
        print_fv(v);
        assert(v.is_empty() && v.capacity() == 5);
        // operator[] is capacity-based, so the former elements still read back.
        assert(v[0] == 1 && v[1] == 2 && v[2] == 3);
    }

    {
        fmt::println("\n# resize() grow / shrink / single-arg");
        fixed_vector<int, 5> v;
        v.resize(3, 7);
        assert(to_ivec(v) == std::vector({7, 7, 7}));
        v.resize(1); // shrink, no destruction
        assert(to_ivec(v) == std::vector({7}));
        v.resize(4); // grow with T{} == 0
        assert(to_ivec(v) == std::vector({7, 0, 0, 0}));
        print_fv(v);
    }

    {
        fmt::println("\n# pop_back() (incl. pop on empty; the popped element stays alive)");
        fixed_vector<int, 5> v{1, 2, 3};
        v.pop_back();
        assert(to_ivec(v) == std::vector({1, 2}));
        assert(v[2] == 3); // not destroyed, just outside size()
        v.pop_back();
        v.pop_back();
        v.pop_back(); // pop on empty is a no-op
        assert(v.is_empty());
        print_fv(v);
    }

    {
        fmt::println("\n# push_back() lvalue & rvalue");
        fixed_vector<int, 5> v;
        const int x = 10;
        v.push_back(x);  // const&
        v.push_back(20); // &&
        print_fv(v);
        assert(to_ivec(v) == std::vector({10, 20}));
    }

    {
        fmt::println("\n# emplace_back()");
        fixed_vector<int, 2> v;
        v.emplace_back(5);
        v.emplace_back(6);
        print_fv(v);
        assert(to_ivec(v) == std::vector({5, 6}));
    }

    {
        fmt::println("\n# unchecked_push_back()/unchecked_emplace_back()");
        fixed_vector<int, 3> v;
        v.unchecked_emplace_back(1);
        v.unchecked_push_back(2);
        int y = 3;
        v.unchecked_push_back(std::move(y));
        print_fv(v);
        assert(to_ivec(v) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# try_push_back()/try_emplace_back() (success then failure)");
        fixed_vector<int, 2> v;
        assert(v.try_push_back(1));
        assert(v.try_emplace_back(2));
        assert(!v.try_push_back(3)); // full -> false, no throw
        assert(!v.try_emplace_back(4));
        print_fv(v);
        assert(to_ivec(v) == std::vector({1, 2}));
    }

    {
        fmt::println("\n# fill_capacity()/fill_size()");
        fixed_vector<int, 5> v;
        v.append_range({1, 2, 3});
        v.fill_size(9); // only the live [0,size)
        print_fv(v);
        assert(to_ivec(v) == std::vector({9, 9, 9}));
        assert(!v.is_full());
        v.fill_capacity(4); // whole capacity, size := max_size()
        print_fv(v);
        assert(to_ivec(v) == std::vector({4, 4, 4, 4, 4}));
        assert(v.is_full());
    }

    {
        fmt::println("\n# zeroize_remaining_space() (zero the reserved tail)");
        fixed_vector<int, 5> v;
        v.fill_capacity(9);
        v.resize(2); // the tail slots [2, 5) still hold 9
        v.zeroize_remaining_space();
        assert(v.size() == 2);
        assert(to_ivec(v) == std::vector({9, 9}));
        // operator[] is capacity-based: the tail is now zero
        for (std::size_t i = v.size(); i < v.max_size(); ++i)
            assert(v[i] == 0);
        print_fv(v);
        // Scrub the whole array: clear() + zeroize_remaining_space() (non-elidable stores).
        v.clear();
        v.zeroize_remaining_space();
        assert(v.is_empty());
        for (std::size_t i = 0; i < v.max_size(); ++i)
            assert(v[i] == 0);
        print_fv(v);
    }

    // ---- append_range / try_append_range / assign_range ----

    {
        fmt::println("\n# append_range() overloads");
        constexpr std::array tail{4, 5};
        const std::vector more{6, 7};

        fixed_vector<int, 12> v;
        v.append_range({1, 2, 3});                   // initializer_list
        v.append_range(std::span<const int>{tail});  // span
        v.append_range(more.begin(), more.end());    // iterator + sentinel
        v.append_range(more.begin(), std::size_t{1}); // iterator + count -> 6
        v.append_range(std::views::iota(8, 10));     // range -> 8,9
        print_fv(v);
        assert(to_ivec(v) == std::vector({1, 2, 3, 4, 5, 6, 7, 6, 8, 9}));
    }

    {
        fmt::println("\n# append_range() with an unsized source appends element-wise");
        // No up-front size check is possible, so the elements that fit are appended before
        // std::bad_alloc is thrown (the sized overloads above are all-or-nothing).
        fixed_vector<int, 4> v;
        v.append_range({1, 2});
        expect_throw<std::bad_alloc>("append_range(filter_view) overflow", [&] {
            v.append_range(std::views::iota(1, 10) | std::views::filter(is_odd));
        });
        print_fv(v);
        assert(to_ivec(v) == std::vector({1, 2, 1, 3})); // partially appended before the throw
    }

    {
        fmt::println("\n# try_append_range() overloads (success then failure)");
        constexpr std::array a{1, 2};
        const std::vector more{5, 6};

        fixed_vector<int, 4> v;
        assert(v.try_append_range(std::span<const int>{a}));       // span
        assert(v.try_append_range({3, 4}));                        // initializer_list
        assert(!v.try_append_range({5, 6}));                       // would overflow -> false
        assert(!v.try_append_range(std::views::iota(0, 3)));       // sized range: checked up front
        assert(!v.try_append_range(more.begin(), more.end()));     // sized sentinel: checked up front
        assert(!v.try_append_range(more.begin(), std::size_t{2})); // iterator + count
        print_fv(v);
        assert(to_ivec(v) == std::vector({1, 2, 3, 4})); // nothing appended by the failures
    }

    {
        fmt::println("\n# try_append_range() with an unsized source may partially append");
        fixed_vector<int, 4> v;
        v.append_range({1, 2});
        // filter_view is not sized: the elements that fit land before false is returned.
        assert(!v.try_append_range(std::views::iota(1, 10) | std::views::filter(is_odd)));
        print_fv(v);
        assert(to_ivec(v) == std::vector({1, 2, 1, 3}));
    }

    {
        fmt::println("\n# assign_range() overloads (clear + append)");
        constexpr std::array arr{5, 6};
        const std::vector src{7, 8, 9};

        fixed_vector<int, 6> v;
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
        print_fv(v);
    }

    // ---- Element access ----

    {
        fmt::println("\n# span() / operator std::span / data()");
        fixed_vector<int, 5> v{1, 2, 3, 4};
        const std::span<const int> s1 = v.span();
        const auto s2 = static_cast<std::span<int>>(v);
        assert(s1.size() == 4 && s2.size() == 4);
        assert(v.data() == s1.data() && v.data() == s2.data());
        fmt::println("span={}", s1);
    }

    {
        fmt::println("\n# front()/back()");
        fixed_vector<int, 5> v{10, 20, 30};
        assert(v.front() == 10 && v.back() == 30);
        v.front() = 11;
        v.back() = 31;
        print_fv(v);
        assert(to_ivec(v) == std::vector({11, 20, 31}));
    }

    {
        fmt::println("\n# operator[] incl. reading beyond size within capacity");
        fixed_vector<int, 5> v; // all 5 slots value-initialized to 0
        v.append_range({11, 22, 33});
        assert(v[0] == 11 && v[2] == 33);
        v[1] = 99;
        assert(v[1] == 99);
        // Indexes 3 and 4 are >= size() but < capacity(): live, value-initialized elements.
        // Deterministic here, unlike aligned_byte_buffer, whose reserved tail is unspecified.
        assert(v[3] == 0 && v[4] == 0);
        fmt::println("v[0]={} v[1]={} v[4] (beyond size)={}", v[0], v[1], v[4]);
    }

    {
        fmt::println("\n# at() valid then out_of_range");
        fixed_vector<int, 5> v{1, 2, 3};
        assert(v.at(0) == 1 && v.at(2) == 3);
        v.at(1) = 99;
        assert(v.at(1) == 99);
        // at() is size-checked, so an index the unchecked operator[] would happily read throws.
        expect_throw<std::out_of_range>("at(3)", [&] { (void)v.at(3); });
    }

    {
        fmt::println("\n# const accessors (the const overloads)");
        const fixed_vector<int, 5> v{1, 2, 3};
        assert(v.front() == 1 && v.back() == 3);
        assert(v[2] == 3 && v.at(2) == 3);
        assert(v.data() != nullptr && v.span().size() == 3);
        assert(std::vector<int>(v.begin(), v.end()) == std::vector({1, 2, 3}));
        assert(std::vector<int>(v.rbegin(), v.rend()) == std::vector({3, 2, 1}));
        const auto s = static_cast<std::span<const int>>(v); // operator std::span<const T>
        assert(s.size() == 3);
        print_fv(v);
        expect_throw<std::out_of_range>("const at(3)", [&] { (void)v.at(3); });
    }

    // ---- Iterators ----

    {
        fmt::println("\n# forward iteration (begin/end, cbegin/cend)");
        fixed_vector<int, 5> v{1, 2, 3};
        int sum = 0;
        for (const int e : v)
            sum += e;
        assert(sum == 6);
        assert(std::vector<int>(v.cbegin(), v.cend()) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# reverse iteration (rbegin/rend, crbegin/crend)");
        fixed_vector<int, 5> v{1, 2, 3};
        assert(std::vector<int>(v.rbegin(), v.rend()) == std::vector({3, 2, 1}));
        assert(std::vector<int>(v.crbegin(), v.crend()) == std::vector({3, 2, 1}));
        *v.rbegin() = 30; // back
        assert(to_ivec(v) == std::vector({1, 2, 30}));
    }

    {
        fmt::println("\n# std algorithms over the iterators");
        fixed_vector<int, 10> v{5, 2, 8, 1, 9, 3};
        std::ranges::sort(v);
        assert(to_ivec(v) == std::vector({1, 2, 3, 5, 8, 9}));
        const auto it = std::ranges::find(v, 8);
        assert(it != v.end() && std::distance(v.begin(), it) == 4);
        assert(std::accumulate(v.begin(), v.end(), 0) == 28);
        fixed_vector<int, 10> w(v.size());
        std::ranges::transform(v, w.begin(), [](const int x) { return x * 2; });
        print_fv(v);
        print_fv(w);
        assert(to_ivec(w) == std::vector({2, 4, 6, 10, 16, 18}));
    }

    // ---- Comparisons ----

    {
        fmt::println("\n# operator== / operator<=> (only the live [0,size) elements)");
        // N is part of the type, so comparison is between same-capacity vectors only.
        const fixed_vector<int, 5> a{1, 2, 3};
        fixed_vector<int, 5> b{1, 2, 3};
        const fixed_vector<int, 5> c{1, 2, 4};
        const fixed_vector<int, 5> d{1, 2};
        assert(a == b);
        assert(a != c);
        assert(a < c);
        assert(c > a);
        assert(d < a); // a prefix compares less
        assert((a <=> b) == std::strong_ordering::equal);

        // The unused tail slots differ but take no part in the comparison.
        b.fill_capacity(1);
        b.assign_range({1, 2, 3});
        assert(b[4] == 1 && a[4] == 0);
        assert(a == b);
        fmt::println("a==b:{} a<c:{} d<a:{}", a == b, a < c, d < a);
    }

    // ---- Custom alignment ----

    {
        fmt::println("\n# Align honored for several values (alignas on the array storage)");
        const auto check_align = []<std::size_t A>()
        {
            fixed_vector<std::byte, 64, A> buf;
            buf.resize(A); // make it non-empty
            const auto off = reinterpret_cast<std::uintptr_t>(buf.data()) % A;
            fmt::println("Align={:>3}: alignof={:>3}  data%Align={}  sizeof={}",
                    A, alignof(decltype(buf)), off, sizeof(buf));
            assert(off == 0);
            assert(alignof(decltype(buf)) == A);
        };
        check_align.template operator()<16>();
        check_align.template operator()<32>();
        check_align.template operator()<64>();
    }

    {
        fmt::println("\n# std::byte storage -> std::span<const std::byte> for SIMD");
        fixed_vector<std::byte, 1024, 16> buf;
        assert(buf.capacity() == 1024 && buf.is_empty());
        for (int i = 0; i < 16; ++i)
            buf.push_back(static_cast<std::byte>(i));
        const std::span<const std::byte> lane = buf.span();
        assert(lane.size() == 16);
        assert(reinterpret_cast<std::uintptr_t>(lane.data()) % 16 == 0);
        // On a NEON target this span feeds a load directly, e.g.:
        //   const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(lane.data()));
        print_fv(buf);
        assert(std::to_integer<unsigned>(lane[0]) == 0);
        assert(std::to_integer<unsigned>(lane[15]) == 15);
    }

    // ---- Overflow -> std::bad_alloc ----

    {
        fmt::println("\n# Capacity overflow throws std::bad_alloc");
        // The count constructor creates count elements, so it can overflow N -- unlike the
        // heap-backed siblings, where X(n) reserves capacity n and cannot.
        expect_throw<std::bad_alloc>("count constructor beyond capacity", [] {
            const fixed_vector<int, 5> v(6);
            (void)v;
        });
        expect_throw<std::bad_alloc>("count+value constructor beyond capacity", [] {
            const fixed_vector<int, 5> v(6, 42);
            (void)v;
        });
        expect_throw<std::bad_alloc>("span constructor beyond capacity", [] {
            constexpr std::array a{1, 2, 3, 4, 5, 6};
            const fixed_vector<int, 5> v(std::span<const int>{a});
            (void)v;
        });
        expect_throw<std::bad_alloc>("initializer_list constructor beyond capacity", [] {
            const fixed_vector<int, 5> v{1, 2, 3, 4, 5, 6};
            (void)v;
        });
        expect_throw<std::bad_alloc>("push_back when full", [] {
            fixed_vector<int, 2> v{1, 2};
            v.push_back(3);
        });
        expect_throw<std::bad_alloc>("emplace_back when full", [] {
            fixed_vector<int, 1> v{1};
            v.emplace_back(2);
        });
        expect_throw<std::bad_alloc>("append_range(span) overflow", [] {
            fixed_vector<int, 2> v;
            constexpr std::array a{1, 2, 3};
            v.append_range(std::span<const int>{a});
        });
        expect_throw<std::bad_alloc>("resize beyond capacity", [] {
            fixed_vector<int, 2> v;
            v.resize(3);
        });
        expect_throw<std::bad_alloc>("assign_range beyond capacity", [] {
            fixed_vector<int, 2> v;
            v.assign_range({1, 2, 3});
        });
    }

    fmt::println("\nAll assertions passed.");
    return 0;
}


/*

Output:


# Default constructor
fv: span=[]  size=0  cap=5  remaining=5  align=8  data%align=0  is_empty=true  is_full=false  sizeof=32

# Count constructor (creates count value-initialized elements)
fv: span=[0, 0, 0]  size=3  cap=5  remaining=2  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# Count + value constructor
fv: span=[42, 42, 42]  size=3  cap=5  remaining=2  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# std::span constructor
fv: span=[1, 2, 3]  size=3  cap=5  remaining=2  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# Iterator + sentinel constructor
fv: span=[1, 2, 3, 4]  size=4  cap=5  remaining=1  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# Iterator + count constructor
fv: span=[2, 3]  size=2  cap=5  remaining=3  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# initializer_list constructor
fv: span=[1, 2, 3]  size=3  cap=5  remaining=2  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# from_range constructor (sized range)
fv: span=[1, 2, 3, 4]  size=4  cap=5  remaining=1  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# from_range constructor (unsized range, appended element-wise)
fv: span=[1, 3, 5, 7, 9]  size=5  cap=5  remaining=0  align=8  data%align=0  is_empty=false  is_full=true  sizeof=32

# initializer_list assignment operator
fv: span=[7, 8, 9]  size=3  cap=5  remaining=2  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# Copy constructor
fv: span=[1, 2, 3]  size=3  cap=5  remaining=2  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# Move constructor (source is NOT emptied)
fv: span=[1, 2, 3]  size=3  cap=5  remaining=2  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32
fv: span=[1, 2, 3]  size=3  cap=5  remaining=2  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# Copy assignment
fv: span=[1, 2, 3, 4, 5]  size=5  cap=5  remaining=0  align=8  data%align=0  is_empty=false  is_full=true  sizeof=32

# Move assignment
fv: span=[4, 5, 6]  size=3  cap=5  remaining=2  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# swap (hidden friend + member)
fv: span=[7, 8, 9]  size=3  cap=5  remaining=2  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32
fv: span=[1, 2]  size=2  cap=5  remaining=3  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# swap exchanges all max_size() slots, not just the live elements
fv: span=[9]  size=1  cap=5  remaining=4  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32
fv: span=[4, 4]  size=2  cap=5  remaining=3  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# capacity()/max_size() are static (no object needed)
capacity=5 max_size=5

# size()/remaining_space()/is_empty()/is_full()
fv: span=[1, 2, 3]  size=3  cap=3  remaining=0  align=8  data%align=0  is_empty=false  is_full=true  sizeof=24

# clear() only resets size(); the elements stay alive
fv: span=[]  size=0  cap=5  remaining=5  align=8  data%align=0  is_empty=true  is_full=false  sizeof=32

# resize() grow / shrink / single-arg
fv: span=[7, 0, 0, 0]  size=4  cap=5  remaining=1  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# pop_back() (incl. pop on empty; the popped element stays alive)
fv: span=[]  size=0  cap=5  remaining=5  align=8  data%align=0  is_empty=true  is_full=false  sizeof=32

# push_back() lvalue & rvalue
fv: span=[10, 20]  size=2  cap=5  remaining=3  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# emplace_back()
fv: span=[5, 6]  size=2  cap=2  remaining=0  align=8  data%align=0  is_empty=false  is_full=true  sizeof=16

# unchecked_push_back()/unchecked_emplace_back()
fv: span=[1, 2, 3]  size=3  cap=3  remaining=0  align=8  data%align=0  is_empty=false  is_full=true  sizeof=24

# try_push_back()/try_emplace_back() (success then failure)
fv: span=[1, 2]  size=2  cap=2  remaining=0  align=8  data%align=0  is_empty=false  is_full=true  sizeof=16

# fill_capacity()/fill_size()
fv: span=[9, 9, 9]  size=3  cap=5  remaining=2  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32
fv: span=[4, 4, 4, 4, 4]  size=5  cap=5  remaining=0  align=8  data%align=0  is_empty=false  is_full=true  sizeof=32

# zeroize_remaining_space() (zero the reserved tail)
fv: span=[9, 9]  size=2  cap=5  remaining=3  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32
fv: span=[]  size=0  cap=5  remaining=5  align=8  data%align=0  is_empty=true  is_full=false  sizeof=32

# append_range() overloads
fv: span=[1, 2, 3, 4, 5, 6, 7, 6, 8, 9]  size=10  cap=12  remaining=2  align=8  data%align=0  is_empty=false  is_full=false  sizeof=56

# append_range() with an unsized source appends element-wise
Caught expected exception (append_range(filter_view) overflow): std::bad_alloc
fv: span=[1, 2, 1, 3]  size=4  cap=4  remaining=0  align=8  data%align=0  is_empty=false  is_full=true  sizeof=24

# try_append_range() overloads (success then failure)
fv: span=[1, 2, 3, 4]  size=4  cap=4  remaining=0  align=8  data%align=0  is_empty=false  is_full=true  sizeof=24

# try_append_range() with an unsized source may partially append
fv: span=[1, 2, 1, 3]  size=4  cap=4  remaining=0  align=8  data%align=0  is_empty=false  is_full=true  sizeof=24

# assign_range() overloads (clear + append)
fv: span=[10, 11, 12]  size=3  cap=6  remaining=3  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# span() / operator std::span / data()
span=[1, 2, 3, 4]

# front()/back()
fv: span=[11, 20, 31]  size=3  cap=5  remaining=2  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32

# operator[] incl. reading beyond size within capacity
v[0]=11 v[1]=99 v[4] (beyond size)=0

# at() valid then out_of_range
Caught expected exception (at(3)): fixed_vector: index >= size

# const accessors (the const overloads)
fv: span=[1, 2, 3]  size=3  cap=5  remaining=2  align=8  data%align=0  is_empty=false  is_full=false  sizeof=32
Caught expected exception (const at(3)): fixed_vector: index >= size

# forward iteration (begin/end, cbegin/cend)

# reverse iteration (rbegin/rend, crbegin/crend)

# std algorithms over the iterators
fv: span=[1, 2, 3, 5, 8, 9]  size=6  cap=10  remaining=4  align=8  data%align=0  is_empty=false  is_full=false  sizeof=48
fv: span=[2, 4, 6, 10, 16, 18]  size=6  cap=10  remaining=4  align=8  data%align=0  is_empty=false  is_full=false  sizeof=48

# operator== / operator<=> (only the live [0,size) elements)
a==b:true a<c:true d<a:true

# Align honored for several values (alignas on the array storage)
Align= 16: alignof= 16  data%Align=0  sizeof=80
Align= 32: alignof= 32  data%Align=0  sizeof=96
Align= 64: alignof= 64  data%Align=0  sizeof=128

# std::byte storage -> std::span<const std::byte> for SIMD
fv: span=[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]  size=16  cap=1024  remaining=1008  align=16  data%align=0  is_empty=false  is_full=false  sizeof=1040

# Capacity overflow throws std::bad_alloc
Caught expected exception (count constructor beyond capacity): std::bad_alloc
Caught expected exception (count+value constructor beyond capacity): std::bad_alloc
Caught expected exception (span constructor beyond capacity): std::bad_alloc
Caught expected exception (initializer_list constructor beyond capacity): std::bad_alloc
Caught expected exception (push_back when full): std::bad_alloc
Caught expected exception (emplace_back when full): std::bad_alloc
Caught expected exception (append_range(span) overflow): std::bad_alloc
Caught expected exception (resize beyond capacity): std::bad_alloc
Caught expected exception (assign_range beyond capacity): std::bad_alloc

All assertions passed.

*/
