// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

#include "aligned_byte_buffer.hpp"
#include "test_utils.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <vector>

// Compile-time check: empty / zero-capacity instances are usable in constant expressions.
// (The allocating paths are not, since over-aligned allocation is not usable in constant
// evaluation -- so only the non-allocating members are exercised here.)
constexpr bool
constexpr_empty_ok()
{
    aligned_byte_buffer<16> a;    // default ctor (no allocation)
    aligned_byte_buffer<16> b(0); // zero-capacity ctor (no allocation)

    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (!(a.is_empty() && a.size() == 0 && a.remaining_space() == 0))
        return false;
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
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

// The emplace_back family is constrained to at most one std::byte / integral argument.
// (A dependent context is needed so a rejected call yields false instead of a hard error.)
template <typename... Args>
constexpr bool can_emplace_back =
    requires(aligned_byte_buffer<16> v, Args&&... args) {
        v.emplace_back(std::forward<Args>(args)...);
    };
static_assert(can_emplace_back<std::byte>);
static_assert(can_emplace_back<int>);
static_assert(can_emplace_back<unsigned char>);
static_assert(can_emplace_back<>);          // appends byte{}
static_assert(!can_emplace_back<double>);   // floating point rejected
static_assert(!can_emplace_back<int, int>); // arity > 1 rejected

// constant_time_equal() is usable in constant expressions (where timing is moot).
static_assert([] {
    constexpr std::array a{1_b, 2_b, 3_b};
    constexpr std::array b{1_b, 2_b, 3_b};
    constexpr std::array c{1_b, 2_b, 4_b};
    return constant_time_equal(a, b) && !constant_time_equal(a, c) &&
           !constant_time_equal(a, std::span<const std::byte>{}) &&
           constant_time_equal(std::span<const std::byte>{}, {});
}());

// Only Align is a template parameter; the requires clause reduces to has_single_bit(Align),
// since alignof(std::byte) == 1 makes the Align >= alignof(T) constraint vacuous here.
static_assert(alignof(std::byte) == 1);

// ---- Constructors ----

static void
test_ctor_default()
{
    const aligned_byte_buffer<16> v;
    CHECK(v.size() == 0);
    CHECK(v.capacity() == 0);
    CHECK(v.is_empty());
    CHECK(v.data() == nullptr);
}

static void
test_ctor_capacity()
{
    // Reserves capacity and starts empty; the reserved bytes are left uninitialized (no
    // whole-capacity memset), unlike dynamic_fixed_vector's value-initialized tail.
    const aligned_byte_buffer<16> v(64);
    CHECK(v.size() == 0);
    CHECK(v.capacity() == 64);
    CHECK(v.remaining_space() == 64);
    CHECK(v.is_empty());
    CHECK(!v.is_full());
}

static void
test_ctor_capacity_value()
{
    const aligned_byte_buffer<16> v(3, 0x42_b);
    CHECK(v.size() == 3);
    CHECK(v.capacity() == 3);
    CHECK(v.is_full());
    CHECK(to_ivec(v) == std::vector({0x42, 0x42, 0x42}));
}

static void
test_ctor_span()
{
    constexpr std::array arr{1_b, 2_b, 3_b};
    const aligned_byte_buffer<16> v(std::span<const std::byte>{arr});
    CHECK(v.capacity() == 3);
    CHECK(to_ivec(v) == std::vector({1, 2, 3}));
}

static void
test_ctor_iter_sentinel()
{
    const std::vector src{1_b, 2_b, 3_b, 4_b};
    const aligned_byte_buffer<16> v(src.begin(), src.end());
    CHECK(v.capacity() == 4);
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 4}));
}

static void
test_ctor_iter_count()
{
    const std::vector src{1_b, 2_b, 3_b, 4_b};
    const aligned_byte_buffer<16> v(src.begin() + 1, 2);
    CHECK(v.capacity() == 2);
    CHECK(to_ivec(v) == std::vector({2, 3}));
}

static void
test_ctor_init_list()
{
    const aligned_byte_buffer<16> v{1_b, 2_b, 3_b};
    CHECK(v.capacity() == 3);
    CHECK(to_ivec(v) == std::vector({1, 2, 3}));
}

static void
test_ctor_from_range()
{
    const auto rg = std::views::iota(1, 5) | std::views::transform(to_byte);
    const aligned_byte_buffer<16> v(std::from_range, rg);
    CHECK(v.capacity() == 4);
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 4}));
}

static void
test_assign_init_list()
{
    aligned_byte_buffer<16> v(5);
    v = {7_b, 8_b, 9_b};
    CHECK(v.capacity() == 5); // capacity unchanged by assign
    CHECK(to_ivec(v) == std::vector({7, 8, 9}));
}

// ---- Copy / move / swap ----

static void
test_copy_ctor()
{
    aligned_byte_buffer<16> a(8);
    a.append_range({1_b, 2_b, 3_b});
    const aligned_byte_buffer<16> b = a; // copies only the live [0,size) bytes
    CHECK(a.data() != b.data());         // independent buffers
    CHECK(b.capacity() == 8);
    CHECK(to_ivec(a) == to_ivec(b));
    a[0] = 99_b;
    CHECK(b[0] == 1_b); // mutation of a does not affect b
}

static void
test_move_ctor()
{
    aligned_byte_buffer<16> a{1_b, 2_b, 3_b};
    const std::byte* const orig = a.data();
    const aligned_byte_buffer<16> b = std::move(a);
    CHECK(b.data() == orig); // buffer transferred, not reallocated
    CHECK(to_ivec(b) == std::vector({1, 2, 3}));
    // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved,clang-analyzer-cplusplus.Move)
    CHECK(a.size() == 0);
    CHECK(a.capacity() == 0);
    CHECK(a.data() == nullptr);
}

static void
test_copy_assign()
{
    const aligned_byte_buffer<16> a{1_b, 2_b, 3_b, 4_b, 5_b};
    aligned_byte_buffer<16> b(1);
    b = a;
    CHECK(b.capacity() == 5); // capacity replaced too
    CHECK(to_ivec(b) == to_ivec(a));
    CHECK(a.data() != b.data());
}

static void
test_move_assign()
{
    aligned_byte_buffer<16> a{4_b, 5_b, 6_b};
    const std::byte* const a_buf = a.data();
    aligned_byte_buffer<16> b(1);
    const std::byte* const b_buf = b.data();
    b = std::move(a);
    CHECK(b.data() == a_buf); // buffer transferred, not reallocated
    CHECK(b.capacity() == 3);
    CHECK(to_ivec(b) == std::vector({4, 5, 6}));
    // Move assignment swaps: the source keeps the target's former buffer until it is destroyed,
    // rather than being left empty as after move construction.
    // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved,clang-analyzer-cplusplus.Move)
    CHECK(a.data() == b_buf);
    CHECK(a.capacity() == 1);
}

static void
test_swap()
{
    aligned_byte_buffer<16> a{1_b, 2_b};
    aligned_byte_buffer<16> b{7_b, 8_b, 9_b};
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
    // returns a *non-null* block, so allocate_'s early return is the only thing making it
    // true.  Cover each structurally distinct way to reach capacity 0, not every permutation.
    const std::vector<std::byte> empty;

    { const aligned_byte_buffer<16> b;           CHECK(data_null_iff_empty(b)); } // never allocates
    { const aligned_byte_buffer<16> b(0);        CHECK(data_null_iff_empty(b)); } // the early return
    { const aligned_byte_buffer<16> b(8);        CHECK(data_null_iff_empty(b)); } // real allocation
    { const aligned_byte_buffer<16> b(0, 7_b);   CHECK(data_null_iff_empty(b)); }
    { const aligned_byte_buffer<16> b(std::span<const std::byte>{});     CHECK(data_null_iff_empty(b)); }
    { const aligned_byte_buffer<16> b(empty.begin(), empty.end());       CHECK(data_null_iff_empty(b)); }
    { const aligned_byte_buffer<16> b(std::from_range, empty);           CHECK(data_null_iff_empty(b)); }

    // Capacity 0 reached by transfer rather than by construction.
    {
        aligned_byte_buffer<16> a(8);
        const aligned_byte_buffer<16> b = std::move(a);
        // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved)
        CHECK(data_null_iff_empty(a)); // moved-from: null unique_ptr, capacity 0
        CHECK(data_null_iff_empty(b));
    }
    {
        aligned_byte_buffer<16> a(8);
        aligned_byte_buffer<16> b; // capacity 0
        b = std::move(a);
        // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved)
        CHECK(data_null_iff_empty(a)); // swap gave the source the target's former (empty) state
        CHECK(data_null_iff_empty(b));
    }
    {
        const aligned_byte_buffer<16> a; // capacity 0
        aligned_byte_buffer<16> b(8);
        b = a; // copy assignment replaces capacity, so b becomes empty
        CHECK(data_null_iff_empty(b));
    }
    {
        aligned_byte_buffer<16> a(8);
        aligned_byte_buffer<16> b;
        swap(a, b);
        CHECK(data_null_iff_empty(a));
        CHECK(data_null_iff_empty(b));
    }

    // Members that change size() must not disturb it.
    {
        aligned_byte_buffer<16> b(8);
        b.clear();
        CHECK(data_null_iff_empty(b)); // clear() does not release the block
        b.resize(2);
        CHECK(data_null_iff_empty(b));
        b.fill_capacity(1_b);
        CHECK(data_null_iff_empty(b));
    }

    // The invariant must survive a throw: assign_range keeps the current capacity, so this
    // overflows a capacity-0 buffer and must leave it consistent.
    {
        aligned_byte_buffer<16> b;
        CHECK_THROWS(std::bad_alloc, b.assign_range({1_b, 2_b}));
        CHECK(data_null_iff_empty(b));
    }
}

static void
test_capacity_max_size()
{
    // Non-static and reporting the runtime capacity, deliberately not a SIZE_MAX-ish value like
    // std::vector::max_size().
    const aligned_byte_buffer<16> v(10);
    CHECK(v.capacity() == 10);
    CHECK(v.max_size() == 10);
    CHECK(v.max_size() != std::numeric_limits<std::size_t>::max());
}

static void
test_size_remaining_space_is_empty_is_full()
{
    aligned_byte_buffer<16> v(3);
    CHECK(v.is_empty());
    CHECK(!v.is_full());
    CHECK(v.size() == 0);
    CHECK(v.remaining_space() == 3);
    v.push_back(1_b);
    CHECK(!v.is_empty());
    CHECK(!v.is_full());
    CHECK(v.size() == 1);
    CHECK(v.remaining_space() == 2);
    v.push_back(2_b);
    v.push_back(3_b);
    CHECK(v.is_full());
    CHECK(v.size() == 3);
    CHECK(v.remaining_space() == 0);
}

// ---- Modifiers ----

static void
test_clear()
{
    aligned_byte_buffer<16> v{1_b, 2_b, 3_b};
    v.clear();
    CHECK(v.is_empty());
    CHECK(v.capacity() == 3); // clear() does not change capacity
    // clear() only resets size(); operator[] is capacity-based, so the bytes still read back.
    CHECK(v[0] == 1_b);
    CHECK(v[2] == 3_b);
}

static void
test_resize()
{
    aligned_byte_buffer<16> v(5);
    v.resize(3, 7_b); // grow with a value (memset)
    CHECK(to_ivec(v) == std::vector({7, 7, 7}));
    v.resize(1); // shrink
    CHECK(to_ivec(v) == std::vector({7}));
    v.resize(4); // grow with std::byte{} == 0
    CHECK(to_ivec(v) == std::vector({7, 0, 0, 0}));
    CHECK(v.capacity() == 5); // resize never changes capacity
}

static void
test_pop_back()
{
    aligned_byte_buffer<16> v{1_b, 2_b, 3_b};
    v.pop_back();
    CHECK(to_ivec(v) == std::vector({1, 2}));
    CHECK(v[2] == 3_b); // not destroyed, just outside size()
    v.pop_back();
    v.pop_back();
    v.pop_back(); // pop on empty is a no-op
    CHECK(v.is_empty());
}

static void
test_push_back_emplace_back()
{
    aligned_byte_buffer<16> v(4);
    const std::byte x = 10_b;
    v.push_back(x);     // by value
    v.push_back(20_b);
    v.emplace_back(30); // int -> std::byte via functional cast
    v.emplace_back();   // no args -> std::byte{}
    CHECK(to_ivec(v) == std::vector({10, 20, 30, 0}));
}

static void
test_unchecked_push_back_unchecked_emplace_back()
{
    aligned_byte_buffer<16> v(3);
    v.unchecked_emplace_back(1);   // int
    v.unchecked_push_back(2_b);
    v.unchecked_emplace_back(3_b); // byte
    CHECK(to_ivec(v) == std::vector({1, 2, 3}));
    CHECK(v.is_full());
}

static void
test_try_push_back_try_emplace_back()
{
    aligned_byte_buffer<16> v(2);
    CHECK(v.try_push_back(1_b));
    CHECK(v.try_emplace_back(2));
    CHECK(v.is_full());
    // Full -> false, no throw.
    CHECK(!v.try_push_back(3_b));
    CHECK(!v.try_emplace_back(4));
    CHECK(to_ivec(v) == std::vector({1, 2}));
}

static void
test_fill_capacity_fill_size()
{
    aligned_byte_buffer<16> v(5);
    v.append_range({1_b, 2_b, 3_b});
    v.fill_size(9_b); // only the live [0,size)
    CHECK(to_ivec(v) == std::vector({9, 9, 9}));
    CHECK(!v.is_full());
    v.fill_capacity(4_b); // whole capacity, size := capacity
    CHECK(to_ivec(v) == std::vector({4, 4, 4, 4, 4}));
    CHECK(v.is_full());
}

static void
test_zeroize_remaining_space()
{
    aligned_byte_buffer<16> v(8);
    v.append_range({1_b, 2_b, 3_b});
    v.zeroize_remaining_space(); // [size, capacity) is now zero; size unchanged
    CHECK(v.size() == 3);
    CHECK(v.capacity() == 8);
    CHECK(to_ivec(v) == std::vector({1, 2, 3}));
    // Turning the unspecified reserved tail into determinate zeros is the point: only now may
    // those bytes be checked for a value.
    for (std::size_t i = v.size(); i < v.capacity(); ++i)
        CHECK(v[i] == 0_b);
    // Scrub the whole buffer: clear() + zeroize_remaining_space() (non-elidable stores).
    v.clear();
    v.zeroize_remaining_space();
    CHECK(v.is_empty());
    CHECK(v.capacity() == 8);
    for (std::size_t i = 0; i < v.capacity(); ++i)
        CHECK(v[i] == 0_b);
}

// ---- append_range / try_append_range / assign_range ----

static void
test_append_range()
{
    constexpr std::array tail{4_b, 5_b};
    const std::vector more{6_b, 7_b};

    aligned_byte_buffer<16> v(12);
    v.append_range({1_b, 2_b, 3_b});                        // initializer_list
    v.append_range(std::span<const std::byte>{tail});       // span (memcpy fast path)
    v.append_range(more.begin(), more.end());               // iterator + sentinel
    v.append_range(more.begin(), std::size_t{1});           // iterator + count -> 6
    v.append_range(std::views::iota(8, 10) | std::views::transform(to_byte)); // range -> 8,9
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 4, 5, 6, 7, 6, 8, 9}));
}

static void
test_try_append_range()
{
    constexpr std::array a{1_b, 2_b};
    const std::vector more{5_b, 6_b};

    aligned_byte_buffer<16> v(4);
    CHECK(v.try_append_range(std::span<const std::byte>{a}));  // span
    CHECK(v.try_append_range({3_b, 4_b}));                     // initializer_list
    CHECK(!v.try_append_range({5_b, 6_b}));                    // would overflow -> false
    CHECK(!v.try_append_range(std::views::iota(0, 3) | std::views::transform(to_byte)));
    CHECK(!v.try_append_range(more.begin(), more.end()));      // sized sentinel: checked up front
    CHECK(!v.try_append_range(more.begin(), std::size_t{2}));  // iterator + count
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 4}));            // nothing appended by the failures
}

static void
test_assign_range()
{
    constexpr std::array arr{5_b, 6_b};
    const std::vector src{7_b, 8_b, 9_b};

    aligned_byte_buffer<16> v(6);
    v.append_range({1_b, 2_b, 3_b});
    v.assign_range(std::span<const std::byte>{arr}); // span
    CHECK(to_ivec(v) == std::vector({5, 6}));
    v.assign_range(src.begin(), src.end()); // iterator + sentinel
    CHECK(to_ivec(v) == std::vector({7, 8, 9}));
    v.assign_range(src.begin(), std::size_t{2}); // iterator + count
    CHECK(to_ivec(v) == std::vector({7, 8}));
    v.assign_range({1_b, 1_b}); // initializer_list
    CHECK(to_ivec(v) == std::vector({1, 1}));
    v.assign_range(std::views::iota(10, 13) | std::views::transform(to_byte)); // range
    CHECK(to_ivec(v) == std::vector({10, 11, 12}));
    CHECK(v.capacity() == 6); // assign_range keeps the current capacity
}

// ---- Element access ----

static void
test_span_and_data()
{
    aligned_byte_buffer<16> v{1_b, 2_b, 3_b, 4_b};
    const std::span<const std::byte> s1 = v.span();
    const auto s2 = static_cast<std::span<std::byte>>(v); // operator std::span<std::byte>
    CHECK(s1.size() == 4);
    CHECK(s2.size() == 4);
    CHECK(v.data() == s1.data());
    CHECK(v.data() == s2.data());
}

static void
test_front_back()
{
    aligned_byte_buffer<16> v{10_b, 20_b, 30_b};
    CHECK(v.front() == 10_b);
    CHECK(v.back() == 30_b);
    v.front() = 11_b;
    v.back() = 31_b;
    CHECK(to_ivec(v) == std::vector({11, 20, 31}));
}

static void
test_operator_index()
{
    aligned_byte_buffer<16> v(5);
    v.append_range({11_b, 22_b, 33_b});
    CHECK(v[0] == 11_b);
    CHECK(v[2] == 33_b);
    v[1] = 99_b;
    CHECK(v[1] == 99_b);
    // Exercise -- but do not check the value of -- a read at an index >= size() within capacity.
    // The reserved tail is left uninitialized, so for std::byte this is well-defined but
    // *unspecified*: unlike fixed_vector / dynamic_fixed_vector, no value may be asserted here.
    const auto probe = std::to_integer<unsigned>(v[v.capacity() - 1]);
    (void)probe;
}

static void
test_at()
{
    aligned_byte_buffer<16> v{1_b, 2_b, 3_b};
    CHECK(v.at(0) == 1_b);
    CHECK(v.at(2) == 3_b);
    v.at(1) = 99_b;
    CHECK(v.at(1) == 99_b);
    // at() is size-checked, so an index the unchecked operator[] would happily read throws.
    CHECK_THROWS(std::out_of_range, (void)v.at(3));
}

static void
test_const_accessors()
{
    const aligned_byte_buffer<16> v{1_b, 2_b, 3_b};
    CHECK(v.front() == 1_b);
    CHECK(v.back() == 3_b);
    CHECK(v[2] == 3_b);
    CHECK(v.at(2) == 3_b);
    CHECK(v.data() != nullptr);
    CHECK(v.span().size() == 3);
    CHECK(std::vector<std::byte>(v.begin(), v.end()) == std::vector({1_b, 2_b, 3_b}));
    CHECK(std::vector<std::byte>(v.rbegin(), v.rend()) == std::vector({3_b, 2_b, 1_b}));
    const auto s = static_cast<std::span<const std::byte>>(v); // operator std::span<const byte>
    CHECK(s.size() == 3);
    CHECK_THROWS(std::out_of_range, (void)v.at(3));
}

// ---- Iterators ----

static void
test_forward_iteration()
{
    aligned_byte_buffer<16> v{1_b, 2_b, 3_b};
    int sum = 0;
    for (const std::byte e : v)
        sum += std::to_integer<int>(e);
    CHECK(sum == 6);
    CHECK(std::vector<std::byte>(v.begin(), v.end()) == std::vector({1_b, 2_b, 3_b}));
    CHECK(std::vector<std::byte>(v.cbegin(), v.cend()) == std::vector({1_b, 2_b, 3_b}));
    *v.begin() = 10_b;
    CHECK(to_ivec(v) == std::vector({10, 2, 3}));
}

static void
test_reverse_iteration()
{
    aligned_byte_buffer<16> v{1_b, 2_b, 3_b};
    CHECK(std::vector<std::byte>(v.rbegin(), v.rend()) == std::vector({3_b, 2_b, 1_b}));
    CHECK(std::vector<std::byte>(v.crbegin(), v.crend()) == std::vector({3_b, 2_b, 1_b}));
    *v.rbegin() = 30_b; // back
    CHECK(to_ivec(v) == std::vector({1, 2, 30}));
}

// ---- Comparisons ----

static void
test_comparisons()
{
    // Unconditional (std::byte is always comparable) and capacity takes no part in the result.
    aligned_byte_buffer<16> a(10);
    a.append_range({1_b, 2_b, 3_b});
    const aligned_byte_buffer<16> b{1_b, 2_b, 3_b}; // capacity 3
    CHECK(a == b);
    CHECK(a.capacity() != b.capacity());

    const aligned_byte_buffer<16> c{1_b, 2_b, 4_b};
    const aligned_byte_buffer<16> d{1_b, 2_b};
    CHECK(a != c);
    CHECK(a < c);
    CHECK(c > a);
    CHECK(d < a); // a prefix compares less
    CHECK((a <=> b) == std::strong_ordering::equal);
    CHECK((d <=> a) == std::strong_ordering::less);
}

static void
test_constant_time_equal()
{
    // Free function for secret-dependent data; the container's operator== stays variable-time.
    const aligned_byte_buffer<16> a{1_b, 2_b, 3_b};
    const aligned_byte_buffer<16> b{1_b, 2_b, 3_b};
    const aligned_byte_buffer<16> c{1_b, 2_b, 4_b};
    CHECK(constant_time_equal(a.span(), b.span()));
    CHECK(!constant_time_equal(a.span(), c.span()));
    CHECK(!constant_time_equal(a.span(), a.span().first(2))); // unequal sizes
    CHECK(constant_time_equal(std::span<const std::byte>{}, std::span<const std::byte>{}));
}

// ---- Custom alignment / SIMD buffer (the motivating use case) ----

static void
test_alignment()
{
    // Over-alignment honored for several Align values.
    const auto check_align = []<std::size_t A>()
    {
        aligned_byte_buffer<A> buf(64);
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
    aligned_byte_buffer<16> buf(1024);
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
    static constexpr std::array too_many{1_b, 2_b, 3_b};

    // The capacity constructor reserves rather than creating elements, so only the modifiers
    // can overflow.
    CHECK_THROWS(std::bad_alloc, aligned_byte_buffer<16> v(2); v.push_back(1_b); v.push_back(2_b);
                 v.push_back(3_b));
    CHECK_THROWS(std::bad_alloc, aligned_byte_buffer<16> v(1); v.emplace_back(1);
                 v.emplace_back(2));
    CHECK_THROWS(std::bad_alloc, aligned_byte_buffer<16> v(2);
                 v.append_range(std::span<const std::byte>{too_many}));
    CHECK_THROWS(std::bad_alloc, aligned_byte_buffer<16> v(2); v.resize(3));
    // assign_range keeps the current capacity, so a source that does not fit throws.
    CHECK_THROWS(std::bad_alloc, aligned_byte_buffer<16> v(2); v.assign_range({1_b, 2_b, 3_b}));
}

int
main() // NOLINT(bugprone-exception-escape)
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
        test_push_back_emplace_back();
        test_unchecked_push_back_unchecked_emplace_back();
        test_try_push_back_try_emplace_back();
        test_fill_capacity_fill_size();
        test_zeroize_remaining_space();

        test_append_range();
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
        test_constant_time_equal();

        test_alignment();
        test_byte_storage_for_simd();

        test_overflow_throws_bad_alloc();
    });
}
