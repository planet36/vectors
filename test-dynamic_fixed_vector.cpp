// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

#include "dynamic_fixed_vector.hpp"
#include "test_utils.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <vector>

// Compile-time check: empty / zero-capacity instances are usable in constant expressions.
// (The allocating paths are not, since over-aligned allocation is not usable in constant
// evaluation -- so only the non-allocating members are exercised here.)
constexpr bool
constexpr_empty_ok()
{
    dynamic_fixed_vector<int> a;    // default ctor (no allocation)
    dynamic_fixed_vector<int> b(0); // zero-capacity ctor (no allocation)

    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (!(a.is_empty() && a.size() == 0 && a.remaining_space() == 0))
        return false;
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
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

// The range / iterator-sentinel constructors require forward iterators: capacity has to be
// computed up front.  An input-only source is rejected at compile time; it goes through
// X(capacity) + append_range instead (see test_append_range_input_iterators).
static_assert(std::constructible_from<dynamic_fixed_vector<int>, std::from_range_t,
                                      std::vector<int>>);
static_assert(!std::constructible_from<dynamic_fixed_vector<int>, std::from_range_t,
                                       std::ranges::istream_view<int>>);

// ---- Constructors ----

static void
test_ctor_default()
{
    const dynamic_fixed_vector<int> v;
    CHECK(v.size() == 0);
    CHECK(v.capacity() == 0);
    CHECK(v.is_empty());
    CHECK(v.data() == nullptr); // no allocation, unlike fixed_vector's in-place storage
}

static void
test_ctor_capacity()
{
    // X(n) reserves capacity n and starts empty -- unlike fixed_vector, where X(count) creates
    // count elements.
    const dynamic_fixed_vector<int> v(5);
    CHECK(v.size() == 0);
    CHECK(v.capacity() == 5);
    CHECK(v.remaining_space() == 5);
    CHECK(v.is_empty());
    CHECK(!v.is_full());
}

static void
test_ctor_capacity_value()
{
    const dynamic_fixed_vector<int> v(3, 42);
    CHECK(v.size() == 3);
    CHECK(v.capacity() == 3);
    CHECK(v.is_full());
    CHECK(to_ivec(v) == std::vector({42, 42, 42}));
}

static void
test_ctor_span()
{
    constexpr std::array arr{1, 2, 3};
    const dynamic_fixed_vector<int> v(std::span<const int>{arr});
    CHECK(v.capacity() == 3);
    CHECK(to_ivec(v) == std::vector({1, 2, 3}));
}

static void
test_ctor_iter_sentinel()
{
    const std::vector src{1, 2, 3, 4};
    const dynamic_fixed_vector<int> v(src.begin(), src.end());
    CHECK(v.capacity() == 4);
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 4}));
}

static void
test_ctor_iter_count()
{
    const std::vector src{1, 2, 3, 4};
    const dynamic_fixed_vector<int> v(src.begin() + 1, 2);
    CHECK(v.capacity() == 2);
    CHECK(to_ivec(v) == std::vector({2, 3}));
}

static void
test_ctor_init_list()
{
    const dynamic_fixed_vector<int> v{1, 2, 3};
    CHECK(v.capacity() == 3);
    CHECK(to_ivec(v) == std::vector({1, 2, 3}));
}

static void
test_ctor_from_range()
{
    const dynamic_fixed_vector<int> v(std::from_range, std::views::iota(1, 5));
    CHECK(v.capacity() == 4);
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 4}));
}

static void
test_assign_init_list()
{
    dynamic_fixed_vector<int> v(5);
    v = {7, 8, 9};
    CHECK(v.capacity() == 5); // capacity unchanged by assign
    CHECK(to_ivec(v) == std::vector({7, 8, 9}));
}

// ---- Copy / move / swap ----

static void
test_copy_ctor()
{
    dynamic_fixed_vector<int> a(4);
    a.append_range({1, 2, 3});
    const dynamic_fixed_vector<int> b = a;
    CHECK(a.data() != b.data()); // deep copy: independent buffers
    CHECK(b.capacity() == 4);
    CHECK(to_ivec(a) == to_ivec(b));
    a[0] = 99;
    CHECK(b[0] == 1); // mutation of a does not affect b
}

static void
test_move_ctor()
{
    dynamic_fixed_vector<int> a{1, 2, 3};
    const int* const orig = a.data();
    const dynamic_fixed_vector<int> b = std::move(a);
    CHECK(b.data() == orig); // buffer transferred, not reallocated
    CHECK(to_ivec(b) == std::vector({1, 2, 3}));
    // Move construction leaves the source empty -- unlike fixed_vector, whose defaulted
    // member-wise move leaves a trivially copyable source unchanged.
    CHECK(a.size() == 0);
    CHECK(a.capacity() == 0);
    CHECK(a.data() == nullptr);
}

static void
test_copy_assign()
{
    const dynamic_fixed_vector<int> a{1, 2, 3, 4, 5};
    dynamic_fixed_vector<int> b(1);
    b = a;
    CHECK(b.capacity() == 5); // capacity replaced too
    CHECK(to_ivec(b) == to_ivec(a));
    CHECK(a.data() != b.data());
}

static void
test_move_assign()
{
    dynamic_fixed_vector<int> a{4, 5, 6};
    const int* const a_buf = a.data();
    dynamic_fixed_vector<int> b(1);
    const int* const b_buf = b.data();
    b = std::move(a);
    CHECK(b.data() == a_buf); // buffer transferred, not reallocated
    CHECK(b.capacity() == 3); // capacity replaced too
    CHECK(to_ivec(b) == std::vector({4, 5, 6}));
    // Move assignment swaps: the source keeps the target's former buffer until it is destroyed,
    // rather than being left empty as after move construction.
    CHECK(a.data() == b_buf);
    CHECK(a.capacity() == 1);
}

static void
test_swap()
{
    dynamic_fixed_vector<int> a{1, 2};
    dynamic_fixed_vector<int> b{7, 8, 9};
    swap(a, b); // hidden friend
    CHECK(to_ivec(a) == std::vector({7, 8, 9}));
    CHECK(to_ivec(b) == std::vector({1, 2}));
    CHECK(a.capacity() == 3); // capacity travels with the buffer
    CHECK(b.capacity() == 2);
    a.swap(b); // member
    CHECK(to_ivec(a) == std::vector({1, 2}));
    CHECK(to_ivec(b) == std::vector({7, 8, 9}));
}

// ---- Observers ----

static void
test_data_null_iff_capacity_zero()
{
    // The class invariant the \pre !is_full() / !is_empty() members rely on to reach the
    // storage without re-checking data() for null.  One direction is free (the throwing
    // ::operator new never returns null), but "capacity 0 -> null" is not: ::operator new(0)
    // returns a *non-null* block, so allocate_raw_'s early return is the only thing making it
    // true.  Cover each structurally distinct way to reach capacity 0, not every permutation.
    const std::vector<int> src{1, 2, 3};
    const std::vector<int> empty;

    { const dynamic_fixed_vector<int> v;              CHECK(data_null_iff_empty(v)); } // never allocates
    { const dynamic_fixed_vector<int> v(0);           CHECK(data_null_iff_empty(v)); } // the early return
    { const dynamic_fixed_vector<int> v(3);           CHECK(data_null_iff_empty(v)); } // real allocation
    { const dynamic_fixed_vector<int> v(0, 7);        CHECK(data_null_iff_empty(v)); }
    { const dynamic_fixed_vector<int> v(std::span<const int>{});      CHECK(data_null_iff_empty(v)); }
    { const dynamic_fixed_vector<int> v(empty.begin(), empty.end());  CHECK(data_null_iff_empty(v)); }
    { const dynamic_fixed_vector<int> v(std::from_range, empty);      CHECK(data_null_iff_empty(v)); }

    // Capacity 0 reached by transfer rather than by construction.
    {
        dynamic_fixed_vector<int> a(3);
        const dynamic_fixed_vector<int> b = std::move(a);
        CHECK(data_null_iff_empty(a)); // moved-from: null unique_ptr, capacity 0
        CHECK(data_null_iff_empty(b));
    }
    {
        dynamic_fixed_vector<int> a(3);
        dynamic_fixed_vector<int> b; // capacity 0
        b = std::move(a);
        CHECK(data_null_iff_empty(a)); // swap gave the source the target's former (empty) state
        CHECK(data_null_iff_empty(b));
    }
    {
        const dynamic_fixed_vector<int> a; // capacity 0
        dynamic_fixed_vector<int> b(3);
        b = a; // copy assignment replaces capacity, so b becomes empty
        CHECK(data_null_iff_empty(b));
    }
    {
        dynamic_fixed_vector<int> a(3);
        dynamic_fixed_vector<int> b;
        swap(a, b);
        CHECK(data_null_iff_empty(a));
        CHECK(data_null_iff_empty(b));
    }

    // Members that change size() must not disturb it.
    {
        dynamic_fixed_vector<int> v(3);
        v.clear();
        CHECK(data_null_iff_empty(v)); // clear() does not release the block
        v.resize(2);
        CHECK(data_null_iff_empty(v));
        v.fill_capacity(1);
        CHECK(data_null_iff_empty(v));
    }

    // The invariant must survive a throw: assign_range keeps the current capacity, so this
    // overflows a capacity-0 vector and must leave it consistent.
    {
        dynamic_fixed_vector<int> v;
        CHECK_THROWS(std::bad_alloc, v.assign_range({1, 2}));
        CHECK(data_null_iff_empty(v));
    }
}

static void
test_capacity_max_size()
{
    // Non-static and reporting the runtime capacity, deliberately not a SIZE_MAX-ish value like
    // std::vector::max_size().
    const dynamic_fixed_vector<int> v(10);
    CHECK(v.capacity() == 10);
    CHECK(v.max_size() == 10);
    CHECK(v.max_size() != std::numeric_limits<std::size_t>::max());
}

static void
test_size_remaining_space_is_empty_is_full()
{
    dynamic_fixed_vector<int> v(3);
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
    dynamic_fixed_vector<int> v{1, 2, 3};
    v.clear();
    CHECK(v.is_empty());
    CHECK(v.capacity() == 3); // clear() does not change capacity
    // clear() only resets size(); operator[] is capacity-based, so the elements still read back.
    CHECK(v[0] == 1);
    CHECK(v[2] == 3);
}

static void
test_resize()
{
    dynamic_fixed_vector<int> v(5);
    v.resize(3, 7); // grow with a value
    CHECK(to_ivec(v) == std::vector({7, 7, 7}));
    v.resize(1); // shrink, no destruction
    CHECK(to_ivec(v) == std::vector({7}));
    v.resize(4); // grow with T{} == 0
    CHECK(to_ivec(v) == std::vector({7, 0, 0, 0}));
    CHECK(v.capacity() == 5); // resize never changes capacity
}

static void
test_pop_back()
{
    dynamic_fixed_vector<int> v{1, 2, 3};
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
    dynamic_fixed_vector<int> v(3);
    const int x = 10;
    v.push_back(x);  // const&
    v.push_back(20); // &&
    CHECK(to_ivec(v) == std::vector({10, 20}));
}

static void
test_emplace_back()
{
    dynamic_fixed_vector<int> v(2);
    v.emplace_back(5);
    v.emplace_back(6);
    CHECK(to_ivec(v) == std::vector({5, 6}));
}

static void
test_unchecked_push_back_unchecked_emplace_back()
{
    dynamic_fixed_vector<int> v(4);
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
    dynamic_fixed_vector<int> v(3);
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
    dynamic_fixed_vector<int> v(5);
    v.append_range({1, 2, 3});
    v.fill_size(9); // only the live [0,size)
    CHECK(to_ivec(v) == std::vector({9, 9, 9}));
    CHECK(!v.is_full());
    v.fill_capacity(4); // whole capacity, size := capacity
    CHECK(to_ivec(v) == std::vector({4, 4, 4, 4, 4}));
    CHECK(v.is_full());
}

static void
test_zeroize_remaining_space()
{
    dynamic_fixed_vector<int> v(5);
    v.fill_capacity(9);
    v.resize(2); // the tail slots [2, 5) still hold 9
    v.zeroize_remaining_space();
    CHECK(v.size() == 2);
    CHECK(v.capacity() == 5);
    CHECK(to_ivec(v) == std::vector({9, 9}));
    // operator[] is capacity-based: the tail is now zero
    for (std::size_t i = v.size(); i < v.capacity(); ++i)
        CHECK(v[i] == 0);
    // Scrub the whole buffer: clear() + zeroize_remaining_space() (non-elidable stores).
    v.clear();
    v.zeroize_remaining_space();
    CHECK(v.is_empty());
    for (std::size_t i = 0; i < v.capacity(); ++i)
        CHECK(v[i] == 0);
}

// ---- append_range / try_append_range / assign_range ----

static void
test_append_range()
{
    constexpr std::array tail{4, 5};
    const std::vector more{6, 7};

    dynamic_fixed_vector<int> v(12);
    v.append_range({1, 2, 3});                    // initializer_list
    v.append_range(std::span<const int>{tail});   // span
    v.append_range(more.begin(), more.end());     // iterator + sentinel
    v.append_range(more.begin(), std::size_t{1}); // iterator + count -> 6
    v.append_range(std::views::iota(8, 10));      // range -> 8,9
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 4, 5, 6, 7, 6, 8, 9}));
}

static void
test_append_range_input_iterators()
{
    // append_range accepts input-only sources, unlike the range constructors (which need forward
    // iterators to size the allocation up front -- see the static_assert above).
    std::istringstream iss("1 2 3");
    dynamic_fixed_vector<int> v(5);
    v.append_range(std::views::istream<int>(iss));
    CHECK(to_ivec(v) == std::vector({1, 2, 3}));
}

static void
test_try_append_range()
{
    constexpr std::array a{1, 2};
    const std::vector more{5, 6};

    dynamic_fixed_vector<int> v(4);
    CHECK(v.try_append_range(std::span<const int>{a}));       // span
    CHECK(v.try_append_range({3, 4}));                        // initializer_list
    CHECK(!v.try_append_range({5, 6}));                       // would overflow -> false
    CHECK(!v.try_append_range(std::views::iota(0, 3)));       // sized range: checked up front
    CHECK(!v.try_append_range(more.begin(), more.end()));     // sized sentinel: checked up front
    CHECK(!v.try_append_range(more.begin(), std::size_t{2})); // iterator + count
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 4}));           // nothing appended by the failures
}

static void
test_assign_range()
{
    constexpr std::array arr{5, 6};
    const std::vector src{7, 8, 9};

    dynamic_fixed_vector<int> v(6);
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
    CHECK(v.capacity() == 6); // assign_range keeps the current capacity
}

// ---- Element access ----

static void
test_span_and_data()
{
    dynamic_fixed_vector<int> v{1, 2, 3, 4};
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
    dynamic_fixed_vector<int> v{10, 20, 30};
    CHECK(v.front() == 10);
    CHECK(v.back() == 30);
    v.front() = 11;
    v.back() = 31;
    CHECK(to_ivec(v) == std::vector({11, 20, 31}));
}

static void
test_operator_index()
{
    dynamic_fixed_vector<int> v(5); // all 5 value-initialized to 0 by the reserve ctor
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
    dynamic_fixed_vector<int> v{1, 2, 3};
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
    const dynamic_fixed_vector<int> v{1, 2, 3};
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
    dynamic_fixed_vector<int> v{1, 2, 3};
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
    dynamic_fixed_vector<int> v{1, 2, 3};
    CHECK(std::vector<int>(v.rbegin(), v.rend()) == std::vector({3, 2, 1}));
    CHECK(std::vector<int>(v.crbegin(), v.crend()) == std::vector({3, 2, 1}));
    *v.rbegin() = 30; // back
    CHECK(to_ivec(v) == std::vector({1, 2, 30}));
}

// ---- Comparisons ----

static void
test_comparisons()
{
    // Capacity is not part of the type, so vectors of different capacity are comparable and
    // capacity takes no part in the result.
    dynamic_fixed_vector<int> a(10);
    a.append_range({1, 2, 3});
    const dynamic_fixed_vector<int> b{1, 2, 3}; // capacity 3
    CHECK(a == b);
    CHECK(a.capacity() != b.capacity());

    const dynamic_fixed_vector<int> c{1, 2, 4};
    const dynamic_fixed_vector<int> d{1, 2};
    CHECK(a != c);
    CHECK(a < c);
    CHECK(c > a);
    CHECK(d < a); // a prefix compares less
    CHECK((a <=> b) == std::strong_ordering::equal);
    CHECK((d <=> a) == std::strong_ordering::less);
}

// ---- Custom alignment / byte buffer (the motivating use case) ----

static void
test_alignment()
{
    // Over-alignment honored for several Align values: the block comes from the aligned
    // ::operator new, so Align can exceed alignof(T).
    const auto check_align = []<std::size_t A>()
    {
        dynamic_fixed_vector<std::byte, A> buf(64);
        buf.resize(A); // make it non-empty
        CHECK(is_aligned(buf.data(), A));
    };
    check_align.template operator()<16>();
    check_align.template operator()<32>();
    check_align.template operator()<64>();
}

static void
test_byte_storage_for_simd()
{
    dynamic_fixed_vector<std::byte, 16> buf(1024);
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
    static constexpr std::array too_many{1, 2, 3};

    // X(n) reserves rather than creating elements, so unlike fixed_vector the constructors
    // cannot overflow -- only the modifiers can.
    CHECK_THROWS(std::bad_alloc, dynamic_fixed_vector<int> v(2); v.push_back(1); v.push_back(2);
                 v.push_back(3));
    CHECK_THROWS(std::bad_alloc, dynamic_fixed_vector<int> v(1); v.emplace_back(1);
                 v.emplace_back(2));
    CHECK_THROWS(std::bad_alloc,
                 dynamic_fixed_vector<int> v(2); v.append_range(std::span<const int>{too_many}));
    CHECK_THROWS(std::bad_alloc, dynamic_fixed_vector<int> v(2); v.resize(3));
    // assign_range keeps the current capacity, so a source that does not fit throws.
    CHECK_THROWS(std::bad_alloc, dynamic_fixed_vector<int> v(2); v.assign_range({1, 2, 3}));
}

int
main()
{
    return run_tests([] {
        test_ctor_default();
        test_ctor_capacity();
        test_ctor_capacity_value();
        test_ctor_span();
        test_ctor_iter_sentinel();
        test_ctor_iter_count();
        test_ctor_init_list();
        test_ctor_from_range();
        test_assign_init_list();

        test_copy_ctor();
        test_move_ctor();
        test_copy_assign();
        test_move_assign();
        test_swap();

        test_data_null_iff_capacity_zero();
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
        test_append_range_input_iterators();
        test_try_append_range();
        test_assign_range();

        test_span_and_data();
        test_front_back();
        test_operator_index();
        test_at();
        test_const_accessors();

        test_forward_iteration();
        test_reverse_iteration();

        test_comparisons();

        test_alignment();
        test_byte_storage_for_simd();

        test_overflow_throws_bad_alloc();
    });
}
