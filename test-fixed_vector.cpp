// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

#include "fixed_vector.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <numeric>
#include <ranges>
#include <span>
#include <stdexcept>
#include <vector>

constexpr auto is_odd = [](const int x) { return x % 2 != 0; };

// Compile-time check: nearly the whole interface is usable in constant expressions.
// Unlike the heap-backed siblings -- whose over-aligned allocation is not usable in constant
// evaluation, so their static_assert can only reach the empty/zero-capacity members --
// fixed_vector's in-place std::array storage imposes no such limit.  A semantic regression in
// any member exercised here therefore fails the compile, not just the run.
constexpr bool
constexpr_api_ok()
{
    fixed_vector<int, 8> v;
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
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
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (!(v.is_full() && v.size() == 8))
        return false;
    if (v.try_push_back(9)) // full -> false, must not throw
        return false;

    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (!(v.front() == 1 && v.back() == 8 && v.at(2) == 3 && v[7] == 8))
        return false;

    v.assign_range({9, 9});
    v.resize(4, 7);
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (!(v.size() == 4 && v[0] == 9 && v[1] == 9 && v[2] == 7 && v[3] == 7))
        return false;

    v.pop_back();
    v.fill_size(1);
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (!(v.size() == 3 && v[0] == 1 && v[2] == 1))
        return false;

    v.clear();
    // Never destroyed: clear() only reset size(), so the elements still read back.
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (!(v.is_empty() && v[0] == 1))
        return false;

    fixed_vector<int, 8> w{4, 5, 6};
    swap(v, w); // hidden friend
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (!(v.size() == 3 && w.is_empty()))
        return false;
    v.swap(w); // member
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
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

// ---- Constructors ----

static void
test_ctor_default()
{
    const fixed_vector<int, 5> v;
    CHECK(v.size() == 0);
    CHECK(v.capacity() == 5);
    CHECK(v.is_empty());
    CHECK(!v.is_full());
    CHECK(v.data() != nullptr); // in-place storage: never null, unlike the heap-backed siblings
}

static void
test_ctor_count()
{
    // Creates count value-initialized elements -- unlike the heap-backed siblings, where X(n)
    // reserves capacity n and starts empty.
    const fixed_vector<int, 5> v(3);
    CHECK(v.size() == 3);
    CHECK(v.capacity() == 5);
    CHECK(to_ivec(v) == std::vector({0, 0, 0}));
}

static void
test_ctor_count_value()
{
    const fixed_vector<int, 5> v(3, 42);
    CHECK(v.size() == 3);
    CHECK(!v.is_full()); // capacity is N, not count
    CHECK(to_ivec(v) == std::vector({42, 42, 42}));
}

static void
test_ctor_span()
{
    constexpr std::array arr{1, 2, 3};
    const fixed_vector<int, 5> v(std::span<const int>{arr});
    CHECK(to_ivec(v) == std::vector({1, 2, 3}));
}

static void
test_ctor_iter_sentinel()
{
    const std::vector src{1, 2, 3, 4};
    const fixed_vector<int, 5> v(src.begin(), src.end());
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 4}));
}

static void
test_ctor_iter_count()
{
    const std::vector src{1, 2, 3, 4};
    const fixed_vector<int, 5> v(src.begin() + 1, 2);
    CHECK(to_ivec(v) == std::vector({2, 3}));
}

static void
test_ctor_init_list()
{
    const fixed_vector<int, 5> v{1, 2, 3};
    CHECK(to_ivec(v) == std::vector({1, 2, 3}));
}

static void
test_ctor_from_range_sized()
{
    const fixed_vector<int, 5> v(std::from_range, std::views::iota(1, 5));
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 4}));
}

static void
test_ctor_from_range_unsized()
{
    // A filter_view is not a sized_range, so there is no up-front size check -- unlike the
    // heap-backed siblings, whose range constructors require forward iterators.
    const fixed_vector<int, 5> v(std::from_range, std::views::iota(1, 10) | std::views::filter(is_odd));
    CHECK(to_ivec(v) == std::vector({1, 3, 5, 7, 9}));
}

static void
test_assign_init_list()
{
    fixed_vector<int, 5> v(5, 1);
    v = {7, 8, 9};
    CHECK(to_ivec(v) == std::vector({7, 8, 9}));
}

// ---- Copy / move / swap ----

static void
test_copy_ctor()
{
    fixed_vector<int, 5> a{1, 2, 3};
    const fixed_vector<int, 5> b = a;
    CHECK(to_ivec(a) == to_ivec(b));
    a[0] = 99;
    CHECK(b[0] == 1); // mutation of a does not affect b
}

static void
test_move_ctor()
{
    fixed_vector<int, 5> a{1, 2, 3};
    const fixed_vector<int, 5> b = std::move(a);
    CHECK(to_ivec(b) == std::vector({1, 2, 3}));
    // Copy and move are member-wise (defaulted): for a trivially copyable T a moved-from
    // fixed_vector is left unchanged -- unlike the heap-backed siblings, where move
    // construction transfers the buffer and leaves the source empty.
    // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved,clang-analyzer-cplusplus.Move)
    CHECK(a.size() == 3);
    CHECK(to_ivec(a) == std::vector({1, 2, 3}));
}

static void
test_copy_assign()
{
    const fixed_vector<int, 5> a{1, 2, 3, 4, 5};
    fixed_vector<int, 5> b;
    b = a;
    CHECK(to_ivec(b) == to_ivec(a));
}

static void
test_move_assign()
{
    fixed_vector<int, 5> a{4, 5, 6};
    fixed_vector<int, 5> b;
    b = std::move(a);
    CHECK(to_ivec(b) == std::vector({4, 5, 6}));
}

static void
test_swap()
{
    fixed_vector<int, 5> a{1, 2};
    fixed_vector<int, 5> b{7, 8, 9};
    swap(a, b); // hidden friend
    CHECK(to_ivec(a) == std::vector({7, 8, 9}));
    CHECK(to_ivec(b) == std::vector({1, 2}));
    a.swap(b); // member
    CHECK(to_ivec(a) == std::vector({1, 2}));
    CHECK(to_ivec(b) == std::vector({7, 8, 9}));
}

static void
test_swap_exchanges_all_slots()
{
    // swap exchanges all max_size() slots, not just the live elements.
    fixed_vector<int, 5> a;
    a.fill_capacity(4); // every slot, including the tail, holds 4
    a.resize(2);
    fixed_vector<int, 5> b{9};
    swap(a, b);
    CHECK(b[4] == 4); // b received a's tail slot
    CHECK(a[4] == 0); // a received b's value-initialized tail
}

// ---- Observers ----

static void
test_capacity_max_size()
{
    const fixed_vector<int, 5> v{1, 2, 3};
    CHECK(v.capacity() == 5);
    CHECK(v.max_size() == 5);
    // Static: callable with no object.
    CHECK(fixed_vector<int, 5>::capacity() == 5);
    CHECK(fixed_vector<int, 5>::max_size() == 5);
}

static void
test_size_remaining_space_is_empty_is_full()
{
    fixed_vector<int, 3> v;
    CHECK(v.is_empty());
    CHECK(!v.is_full());
    CHECK(v.size() == 0);
    CHECK(v.remaining_space() == 3);
    v.push_back(1);
    CHECK(!v.is_empty());
    CHECK(!v.is_full());
    CHECK(v.size() == 1);
    CHECK(v.remaining_space() == 2);
    v.push_back(2);
    v.push_back(3);
    CHECK(v.is_full());
    CHECK(v.size() == 3);
    CHECK(v.remaining_space() == 0);
}

// ---- Modifiers ----

static void
test_clear()
{
    fixed_vector<int, 5> v{1, 2, 3};
    v.clear();
    CHECK(v.is_empty());
    CHECK(v.capacity() == 5);
    // clear() only resets size(); operator[] is capacity-based, so the former elements still
    // read back.
    CHECK(v[0] == 1);
    CHECK(v[1] == 2);
    CHECK(v[2] == 3);
}

static void
test_resize()
{
    fixed_vector<int, 5> v;
    v.resize(3, 7); // grow with a value
    CHECK(to_ivec(v) == std::vector({7, 7, 7}));
    v.resize(1); // shrink, no destruction
    CHECK(to_ivec(v) == std::vector({7}));
    v.resize(4); // grow with T{} == 0
    CHECK(to_ivec(v) == std::vector({7, 0, 0, 0}));
}

static void
test_pop_back()
{
    fixed_vector<int, 5> v{1, 2, 3};
    v.pop_back();
    CHECK(to_ivec(v) == std::vector({1, 2}));
    CHECK(v[2] == 3); // not destroyed, just outside size()
    v.pop_back();
    v.pop_back();
    v.pop_back(); // pop on empty is a no-op
    CHECK(v.is_empty());
}

static void
test_push_back()
{
    fixed_vector<int, 5> v;
    const int x = 10;
    v.push_back(x);  // const&
    v.push_back(20); // &&
    CHECK(to_ivec(v) == std::vector({10, 20}));
}

static void
test_emplace_back()
{
    fixed_vector<int, 2> v;
    v.emplace_back(5);
    v.emplace_back(6);
    CHECK(to_ivec(v) == std::vector({5, 6}));
}

static void
test_unchecked_push_back_unchecked_emplace_back()
{
    fixed_vector<int, 4> v;
    v.unchecked_emplace_back(1);
    const int x = 2;
    v.unchecked_push_back(x); // const&
    v.unchecked_push_back(3); // &&
    int y = 4;
    v.unchecked_push_back(std::move(y)); // &&
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 4}));
    CHECK(v.is_full());
}

static void
test_try_push_back_try_emplace_back()
{
    fixed_vector<int, 3> v;
    const int x = 1;
    CHECK(v.try_push_back(x)); // const&
    CHECK(v.try_push_back(2)); // &&
    CHECK(v.try_emplace_back(3));
    CHECK(v.is_full());
    // Full -> false, no throw.
    CHECK(!v.try_push_back(x)); // const&
    CHECK(!v.try_push_back(4)); // &&
    CHECK(!v.try_emplace_back(5));
    CHECK(to_ivec(v) == std::vector({1, 2, 3}));
}

static void
test_fill_capacity_fill_size()
{
    fixed_vector<int, 5> v;
    v.append_range({1, 2, 3});
    v.fill_size(9); // only the live [0,size)
    CHECK(to_ivec(v) == std::vector({9, 9, 9}));
    CHECK(!v.is_full());
    v.fill_capacity(4); // whole capacity, size := max_size()
    CHECK(to_ivec(v) == std::vector({4, 4, 4, 4, 4}));
    CHECK(v.is_full());
}

static void
test_zeroize_remaining_space()
{
    fixed_vector<int, 5> v;
    v.fill_capacity(9);
    v.resize(2); // the tail slots [2, 5) still hold 9
    v.zeroize_remaining_space();
    CHECK(v.size() == 2);
    CHECK(to_ivec(v) == std::vector({9, 9}));
    // operator[] is capacity-based: the tail is now zero
    for (std::size_t i = v.size(); i < v.max_size(); ++i)
        CHECK(v[i] == 0);
    // Scrub the whole array: clear() + zeroize_remaining_space() (non-elidable stores).
    v.clear();
    v.zeroize_remaining_space();
    CHECK(v.is_empty());
    for (std::size_t i = 0; i < v.max_size(); ++i)
        CHECK(v[i] == 0);
}

// ---- append_range / try_append_range / assign_range ----

static void
test_append_range()
{
    constexpr std::array tail{4, 5};
    const std::vector more{6, 7};

    fixed_vector<int, 12> v;
    v.append_range({1, 2, 3});                    // initializer_list
    v.append_range(std::span<const int>{tail});   // span
    v.append_range(more.begin(), more.end());     // iterator + sentinel
    v.append_range(more.begin(), std::size_t{1}); // iterator + count -> 6
    v.append_range(std::views::iota(8, 10));      // range -> 8,9
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 4, 5, 6, 7, 6, 8, 9}));
}

static void
test_append_range_unsized_partial()
{
    // No up-front size check is possible for an unsized source, so the elements that fit are
    // appended before std::bad_alloc is thrown (the sized overloads are all-or-nothing).
    fixed_vector<int, 4> v;
    v.append_range({1, 2});
    CHECK_THROWS(std::bad_alloc, v.append_range(std::views::iota(1, 10) | std::views::filter(is_odd)));
    CHECK(to_ivec(v) == std::vector({1, 2, 1, 3})); // partially appended before the throw
}

static void
test_try_append_range()
{
    constexpr std::array a{1, 2};
    const std::vector more{5, 6};

    fixed_vector<int, 4> v;
    CHECK(v.try_append_range(std::span<const int>{a}));       // span
    CHECK(v.try_append_range({3, 4}));                        // initializer_list
    CHECK(!v.try_append_range({5, 6}));                       // would overflow -> false
    CHECK(!v.try_append_range(std::views::iota(0, 3)));       // sized range: checked up front
    CHECK(!v.try_append_range(more.begin(), more.end()));     // sized sentinel: checked up front
    CHECK(!v.try_append_range(more.begin(), std::size_t{2})); // iterator + count
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 4}));           // nothing appended by the failures
}

static void
test_try_append_range_unsized_partial()
{
    fixed_vector<int, 4> v;
    v.append_range({1, 2});
    // filter_view is not sized: the elements that fit land before false is returned.
    CHECK(!v.try_append_range(std::views::iota(1, 10) | std::views::filter(is_odd)));
    CHECK(to_ivec(v) == std::vector({1, 2, 1, 3}));
}

static void
test_assign_range()
{
    constexpr std::array arr{5, 6};
    const std::vector src{7, 8, 9};

    fixed_vector<int, 6> v;
    v.append_range({1, 2, 3});
    v.assign_range(std::span<const int>{arr}); // span
    CHECK(to_ivec(v) == std::vector({5, 6}));
    v.assign_range(src.begin(), src.end()); // iterator + sentinel
    CHECK(to_ivec(v) == std::vector({7, 8, 9}));
    v.assign_range(src.begin(), std::size_t{2}); // iterator + count
    CHECK(to_ivec(v) == std::vector({7, 8}));
    v.assign_range({1, 1}); // initializer_list
    CHECK(to_ivec(v) == std::vector({1, 1}));
    v.assign_range(std::views::iota(10, 13)); // range
    CHECK(to_ivec(v) == std::vector({10, 11, 12}));
}

// ---- Element access ----

static void
test_span_and_data()
{
    fixed_vector<int, 5> v{1, 2, 3, 4};
    const std::span<const int> s1 = v.span();
    const auto s2 = static_cast<std::span<int>>(v); // operator std::span<T>
    CHECK(s1.size() == 4);
    CHECK(s2.size() == 4);
    CHECK(v.data() == s1.data());
    CHECK(v.data() == s2.data());
}

static void
test_front_back()
{
    fixed_vector<int, 5> v{10, 20, 30};
    CHECK(v.front() == 10);
    CHECK(v.back() == 30);
    v.front() = 11;
    v.back() = 31;
    CHECK(to_ivec(v) == std::vector({11, 20, 31}));
}

static void
test_operator_index()
{
    fixed_vector<int, 5> v; // all 5 slots value-initialized to 0
    v.append_range({11, 22, 33});
    CHECK(v[0] == 11);
    CHECK(v[2] == 33);
    v[1] = 99;
    CHECK(v[1] == 99);
    // Indexes 3 and 4 are >= size() but < capacity(): live, value-initialized elements.
    // Deterministic here, unlike aligned_byte_buffer, whose reserved tail is unspecified.
    CHECK(v[3] == 0);
    CHECK(v[4] == 0);
}

static void
test_at()
{
    fixed_vector<int, 5> v{1, 2, 3};
    CHECK(v.at(0) == 1);
    CHECK(v.at(2) == 3);
    v.at(1) = 99;
    CHECK(v.at(1) == 99);
    // at() is size-checked, so an index the unchecked operator[] would happily read throws.
    CHECK_THROWS(std::out_of_range, (void)v.at(3));
}

static void
test_const_accessors()
{
    const fixed_vector<int, 5> v{1, 2, 3};
    CHECK(v.front() == 1);
    CHECK(v.back() == 3);
    CHECK(v[2] == 3);
    CHECK(v.at(2) == 3);
    CHECK(v.data() != nullptr);
    CHECK(v.span().size() == 3);
    CHECK(std::vector<int>(v.begin(), v.end()) == std::vector({1, 2, 3}));
    CHECK(std::vector<int>(v.rbegin(), v.rend()) == std::vector({3, 2, 1}));
    const auto s = static_cast<std::span<const int>>(v); // operator std::span<const T>
    CHECK(s.size() == 3);
    CHECK_THROWS(std::out_of_range, (void)v.at(3));
}

// ---- Iterators ----

static void
test_forward_iteration()
{
    fixed_vector<int, 5> v{1, 2, 3};
    int sum = 0;
    for (const int e : v)
        sum += e;
    CHECK(sum == 6);
    CHECK(std::vector<int>(v.begin(), v.end()) == std::vector({1, 2, 3}));
    CHECK(std::vector<int>(v.cbegin(), v.cend()) == std::vector({1, 2, 3}));
    *v.begin() = 10;
    CHECK(to_ivec(v) == std::vector({10, 2, 3}));
}

static void
test_reverse_iteration()
{
    fixed_vector<int, 5> v{1, 2, 3};
    CHECK(std::vector<int>(v.rbegin(), v.rend()) == std::vector({3, 2, 1}));
    CHECK(std::vector<int>(v.crbegin(), v.crend()) == std::vector({3, 2, 1}));
    *v.rbegin() = 30; // back
    CHECK(to_ivec(v) == std::vector({1, 2, 30}));
}

static void
test_std_algorithms()
{
    fixed_vector<int, 10> v{5, 2, 8, 1, 9, 3};
    std::ranges::sort(v);
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 5, 8, 9}));
    auto* const it = std::ranges::find(v, 8);
    CHECK(it != v.end());
    CHECK(std::distance(v.begin(), it) == 4);
    CHECK(std::accumulate(v.begin(), v.end(), 0) == 28);
    fixed_vector<int, 10> w(v.size());
    std::ranges::transform(v, w.begin(), [](const int x) { return x * 2; });
    CHECK(to_ivec(w) == std::vector({2, 4, 6, 10, 16, 18}));
}

// ---- Comparisons ----

static void
test_comparisons()
{
    // N is part of the type, so comparison is between same-capacity vectors only.
    const fixed_vector<int, 5> a{1, 2, 3};
    fixed_vector<int, 5> b{1, 2, 3};
    const fixed_vector<int, 5> c{1, 2, 4};
    const fixed_vector<int, 5> d{1, 2};
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a < c);
    CHECK(c > a);
    CHECK(d < a); // a prefix compares less
    CHECK((a <=> b) == std::strong_ordering::equal);
    CHECK((d <=> a) == std::strong_ordering::less);

    // Only the live [0,size) elements take part: the unused tail slots differ but are ignored.
    b.fill_capacity(1);
    b.assign_range({1, 2, 3});
    CHECK(b[4] == 1);
    CHECK(a[4] == 0);
    CHECK(a == b);
}

// ---- Custom alignment ----

static void
test_alignment()
{
    // Align honored for several values (alignas on the array storage).
    const auto check_align = []<std::size_t A>()
    {
        fixed_vector<std::byte, 64, A> buf;
        buf.resize(A); // make it non-empty
        CHECK(is_aligned(buf.data(), A));
        CHECK(alignof(decltype(buf)) == A);
    };
    check_align.template operator()<16>();
    check_align.template operator()<32>();
    check_align.template operator()<64>();
}

static void
test_byte_storage_for_simd()
{
    fixed_vector<std::byte, 1024, 16> buf;
    CHECK(buf.capacity() == 1024);
    CHECK(buf.is_empty());
    for (int i = 0; i < 16; ++i)
        buf.push_back(to_byte(i));
    const std::span<const std::byte> lane = buf.span();
    CHECK(lane.size() == 16);
    CHECK(is_aligned(lane.data(), 16));
    // On a NEON target this span feeds a load directly, e.g.:
    //   const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(lane.data()));
    CHECK(std::to_integer<unsigned>(lane[0]) == 0);
    CHECK(std::to_integer<unsigned>(lane[15]) == 15);
}

// ---- Overflow -> std::bad_alloc ----

static void
test_overflow_throws_bad_alloc()
{
    static constexpr std::array too_many{1, 2, 3, 4, 5, 6};

    // The count constructor creates count elements, so it can overflow N -- unlike the
    // heap-backed siblings, where X(n) reserves capacity n and cannot.
    CHECK_THROWS(std::bad_alloc, const fixed_vector<int, 5> v(6); (void)v);
    CHECK_THROWS(std::bad_alloc, const fixed_vector<int, 5> v(6, 42); (void)v);
    CHECK_THROWS(std::bad_alloc, const fixed_vector<int, 5> v(std::span<const int>{too_many}); (void)v);
    CHECK_THROWS(std::bad_alloc, const fixed_vector<int, 5> v{1, 2, 3, 4, 5, 6}; (void)v);
    CHECK_THROWS(std::bad_alloc, fixed_vector<int, 2> v{1, 2}; v.push_back(3));
    CHECK_THROWS(std::bad_alloc, fixed_vector<int, 1> v{1}; v.emplace_back(2));
    CHECK_THROWS(std::bad_alloc, fixed_vector<int, 2> v; v.append_range(std::span<const int>{too_many}));
    CHECK_THROWS(std::bad_alloc, fixed_vector<int, 2> v; v.resize(3));
    CHECK_THROWS(std::bad_alloc, fixed_vector<int, 2> v; v.assign_range({1, 2, 3}));
}

int
main() // NOLINT(bugprone-exception-escape)
{
    return run_tests([] {
        test_ctor_default();
        test_ctor_count();
        test_ctor_count_value();
        test_ctor_span();
        test_ctor_iter_sentinel();
        test_ctor_iter_count();
        test_ctor_init_list();
        test_ctor_from_range_sized();
        test_ctor_from_range_unsized();
        test_assign_init_list();

        test_copy_ctor();
        test_move_ctor();
        test_copy_assign();
        test_move_assign();
        test_swap();
        test_swap_exchanges_all_slots();

        test_capacity_max_size();
        test_size_remaining_space_is_empty_is_full();

        test_clear();
        test_resize();
        test_pop_back();
        test_push_back();
        test_emplace_back();
        test_unchecked_push_back_unchecked_emplace_back();
        test_try_push_back_try_emplace_back();
        test_fill_capacity_fill_size();
        test_zeroize_remaining_space();

        test_append_range();
        test_append_range_unsized_partial();
        test_try_append_range();
        test_try_append_range_unsized_partial();
        test_assign_range();

        test_span_and_data();
        test_front_back();
        test_operator_index();
        test_at();
        test_const_accessors();

        test_forward_iteration();
        test_reverse_iteration();
        test_std_algorithms();

        test_comparisons();

        test_alignment();
        test_byte_storage_for_simd();

        test_overflow_throws_bad_alloc();
    });
}
