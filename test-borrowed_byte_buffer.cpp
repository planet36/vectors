// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

#include "borrowed_byte_buffer.hpp"
#include "test_utils.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <ranges>
#include <stdexcept>
#include <vector>

// A borrowed_byte_buffer owns nothing: pointer + capacity + size, so it is trivially copyable and
// its special members are all defaulted (shallow copy/move).
static_assert(std::is_trivially_copyable_v<borrowed_byte_buffer>);
static_assert(std::is_trivially_destructible_v<borrowed_byte_buffer>);

// Compile-time check: the default (empty, non-borrowing) instance is usable in constant
// expressions.  The borrowing constructors are not -- forming a byte view needs a
// reinterpret_cast, which is barred in constant evaluation -- so only the default instance and the
// non-borrowing members are exercised here.
constexpr bool
constexpr_empty_ok()
{
    borrowed_byte_buffer a; // default ctor: null, capacity 0

    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (!(a.is_empty() && a.size() == 0 && a.capacity() == 0 && a.remaining_space() == 0))
        return false;
    if (a.data() != nullptr)
        return false;
    if (a.try_push_back(std::byte{1})) // capacity 0 -> full -> false, must not throw
        return false;
    a.clear();
    a.pop_back();
    a.zeroize_remaining_space(); // no reserved tail on a capacity-0 buffer -> no-op
    borrowed_byte_buffer c;
    swap(a, c);
    return a.size() == 0 && a == c;
}
static_assert(constexpr_empty_ok());

// The emplace_back family is constrained to at most one std::byte / integral argument (identical
// to aligned_byte_buffer).  A dependent context is needed so a rejected call yields false.
template <typename... Args>
constexpr bool can_emplace_back =
    requires(borrowed_byte_buffer v, Args&&... args) {
        v.emplace_back(std::forward<Args>(args)...);
    };
static_assert(can_emplace_back<std::byte>);
static_assert(can_emplace_back<int>);
static_assert(can_emplace_back<unsigned char>);
static_assert(can_emplace_back<>);          // appends byte{}
static_assert(!can_emplace_back<double>);   // floating point rejected
static_assert(!can_emplace_back<int, int>); // arity > 1 rejected

// The borrowing constructors accept only writable, trivially-copyable contiguous storage, and
// never a source that would leave a dangling view.  These document the is_writable_borrow_
// constraint as compile-time facts.
template <typename... Args>
constexpr bool can_construct =
    requires(Args&&... args) { borrowed_byte_buffer{std::forward<Args>(args)...}; };

// Accepted: lvalue contiguous containers, an rvalue std::span (a borrowed_range), a single object
// pointer, and the (void*, size_t) primitive.
static_assert(can_construct<std::array<std::byte, 8>&>);
static_assert(can_construct<std::vector<std::byte>&>);
static_assert(can_construct<std::vector<int>&>);
static_assert(can_construct<std::string&>);
static_assert(can_construct<std::span<std::byte>>); // rvalue span is safe (non-owning)
static_assert(can_construct<int*>);
static_assert(can_construct<void*, std::size_t>);
// Rejected: rvalue owning containers (would dangle), const elements (unwritable), a non-range
// non-pointer, and a const single object.
static_assert(!can_construct<std::vector<std::byte>>);
static_assert(!can_construct<std::array<std::byte, 8>>);
static_assert(!can_construct<std::string>);
static_assert(!can_construct<const std::array<std::byte, 8>&>);
static_assert(!can_construct<int&>);
static_assert(!can_construct<const int*>);
// Construction from another borrowed_byte_buffer resolves to copy/move, not the range ctor.
static_assert(can_construct<borrowed_byte_buffer&>);
static_assert(can_construct<borrowed_byte_buffer>);

// ---- Constructors ----

static void
test_ctor_default()
{
    const borrowed_byte_buffer v;
    CHECK(v.size() == 0);
    CHECK(v.capacity() == 0);
    CHECK(v.is_empty());
    CHECK(v.data() == nullptr);
}

static void
test_ctor_ptr_capacity()
{
    // The (void*, capacity) primitive: borrow raw bytes, start empty.
    std::array<std::byte, 8> s{};
    void* const p = s.data();
    const borrowed_byte_buffer v(p, 8);
    CHECK(v.capacity() == 8);
    CHECK(v.size() == 0);
    CHECK(v.is_empty());
    CHECK(v.data() == s.data()); // overlays the given address, does not copy
}

static void
test_ctor_range()
{
    // Any writable, contiguous, sized range -- overlaid, not copied; the buffer starts empty.
    std::array<std::byte, 4> a{};
    const borrowed_byte_buffer va{a};
    CHECK(va.capacity() == 4);
    CHECK(va.size() == 0);
    CHECK(va.data() == a.data());

    std::vector<std::byte> vec(5);
    const borrowed_byte_buffer vv{vec};
    CHECK(vv.capacity() == 5);
    CHECK(vv.data() == vec.data());

    std::vector<int> vi(3); // element type need not be std::byte; capacity is the byte size
    const borrowed_byte_buffer vvi{vi};
    CHECK(vvi.capacity() == 3 * sizeof(int));
    CHECK(vvi.data() == reinterpret_cast<const std::byte*>(vi.data()));

    std::byte carr[6]{};
    const borrowed_byte_buffer vc{carr};
    CHECK(vc.capacity() == 6);

    std::array<std::byte, 4> sp_backing{};
    const std::span<std::byte> sp{sp_backing};
    const borrowed_byte_buffer vs{sp};
    CHECK(vs.capacity() == 4);
    CHECK(vs.data() == sp_backing.data());
}

static void
test_ctor_range_capacity()
{
    // Borrow only the first `capacity` bytes of a larger range.
    std::array<std::byte, 8> a{};
    const borrowed_byte_buffer v{a, 4};
    CHECK(v.capacity() == 4);
    CHECK(v.size() == 0);
    CHECK(v.data() == a.data());
}

static void
test_ctor_single_object()
{
    // Overlay a single object; capacity is sizeof(T).
    std::uint32_t u = 0;
    const borrowed_byte_buffer v{&u};
    CHECK(v.capacity() == sizeof(u));
    CHECK(v.size() == 0);
    CHECK(v.data() == reinterpret_cast<const std::byte*>(&u));
}

static void
test_adopting()
{
    // adopting() starts full, so the bytes already in the region are the live contents.
    std::array<std::byte, 4> a{1_b, 2_b, 3_b, 4_b};
    const auto v = borrowed_byte_buffer::adopting(a);
    CHECK(v.size() == 4);
    CHECK(v.is_full());
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 4}));

    // adopting(range, capacity): adopt a prefix.
    const auto v2 = borrowed_byte_buffer::adopting(a, 2);
    CHECK(v2.size() == 2);
    CHECK(v2.capacity() == 2);
    CHECK(to_ivec(v2) == std::vector({1, 2}));

    // adopting(void*, capacity).
    void* const p = a.data();
    const auto v3 = borrowed_byte_buffer::adopting(p, 3);
    CHECK(v3.size() == 3);
    CHECK(to_ivec(v3) == std::vector({1, 2, 3}));

    // adopting(T*): a populated single object.
    std::uint16_t u = 0xBEEF;
    const auto v4 = borrowed_byte_buffer::adopting(&u);
    CHECK(v4.size() == sizeof(u));
    CHECK(v4.is_full());
}

static void
test_write_shows_through()
{
    // Writes through the buffer land in the borrowed storage; the reserved tail is untouched.
    std::array<std::byte, 4> s{};
    borrowed_byte_buffer v{s};
    v.append_range({1_b, 2_b, 3_b});
    CHECK(s[0] == 1_b);
    CHECK(s[2] == 3_b);
    CHECK(s[3] == std::byte{}); // beyond size(): not written
}

// ---- Copy / move / swap (shallow: the view is copied, the bytes are shared) ----

static void
test_copy_ctor()
{
    std::array<std::byte, 8> s{};
    borrowed_byte_buffer a{s};
    a.append_range({1_b, 2_b, 3_b});
    const borrowed_byte_buffer b = a;
    CHECK(a.data() == b.data()); // SAME storage -- a shallow copy, unlike aligned_byte_buffer
    CHECK(b.capacity() == 8);
    CHECK(b.size() == 3);
    CHECK(to_ivec(a) == to_ivec(b));
    a[0] = 99_b;
    CHECK(b[0] == 99_b); // the copy aliases the same bytes, so it observes the write
}

static void
test_move_ctor()
{
    std::array<std::byte, 4> s{1_b, 2_b, 3_b, 4_b};
    borrowed_byte_buffer a = borrowed_byte_buffer::adopting(s);
    const std::byte* const orig = a.data();
    const borrowed_byte_buffer b = std::move(a);
    CHECK(b.data() == orig);
    CHECK(to_ivec(b) == std::vector({1, 2, 3, 4}));
    // Move is a shallow copy (defaulted, trivially copyable): the source is NOT emptied, unlike
    // aligned_byte_buffer's move.
    // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved,clang-analyzer-cplusplus.Move)
    CHECK(a.data() == orig);
    CHECK(a.size() == 4);
    CHECK(a.capacity() == 4);
}

static void
test_copy_assign()
{
    std::array<std::byte, 5> s1{1_b, 2_b, 3_b, 4_b, 5_b};
    std::array<std::byte, 3> s2{};
    const borrowed_byte_buffer a = borrowed_byte_buffer::adopting(s1);
    borrowed_byte_buffer b{s2};
    b = a;
    CHECK(b.data() == a.data()); // b now aliases s1 (capacity and pointer replaced)
    CHECK(b.capacity() == 5);
    CHECK(b.size() == 5);
    CHECK(to_ivec(b) == to_ivec(a));

    // Self-assignment is a harmless no-op (defaulted, scalar members).
    auto& r = b;
    b = r;
    CHECK(b.data() == a.data());
    CHECK(to_ivec(b) == std::vector({1, 2, 3, 4, 5}));
}

static void
test_move_assign()
{
    std::array<std::byte, 3> s{4_b, 5_b, 6_b};
    borrowed_byte_buffer a = borrowed_byte_buffer::adopting(s);
    borrowed_byte_buffer b;
    b = std::move(a);
    CHECK(b.data() == s.data());
    CHECK(b.capacity() == 3);
    CHECK(to_ivec(b) == std::vector({4, 5, 6}));
}

static void
test_swap()
{
    std::array<std::byte, 2> s1{1_b, 2_b};
    std::array<std::byte, 3> s2{7_b, 8_b, 9_b};
    borrowed_byte_buffer a = borrowed_byte_buffer::adopting(s1);
    borrowed_byte_buffer b = borrowed_byte_buffer::adopting(s2);
    swap(a, b); // hidden friend
    CHECK(to_ivec(a) == std::vector({7, 8, 9}));
    CHECK(to_ivec(b) == std::vector({1, 2}));
    CHECK(a.capacity() == 3); // capacity travels with the view
    CHECK(b.capacity() == 2);
    a.swap(b); // member
    CHECK(to_ivec(a) == std::vector({1, 2}));
    CHECK(to_ivec(b) == std::vector({7, 8, 9}));
}

// ---- Observers ----

static void
test_capacity_max_size()
{
    // Non-static and reporting the runtime capacity, deliberately not a SIZE_MAX-ish value.
    std::array<std::byte, 10> s{};
    const borrowed_byte_buffer v{s};
    CHECK(v.capacity() == 10);
    CHECK(v.max_size() == 10);
    CHECK(v.max_size() != std::numeric_limits<std::size_t>::max());
}

static void
test_size_remaining_space_is_empty_is_full()
{
    std::array<std::byte, 3> s{};
    borrowed_byte_buffer v{s};
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
    std::array<std::byte, 3> s{1_b, 2_b, 3_b};
    borrowed_byte_buffer v = borrowed_byte_buffer::adopting(s);
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
    std::array<std::byte, 5> s{};
    borrowed_byte_buffer v{s};
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
    std::array<std::byte, 3> s{1_b, 2_b, 3_b};
    borrowed_byte_buffer v = borrowed_byte_buffer::adopting(s);
    v.pop_back();
    CHECK(to_ivec(v) == std::vector({1, 2}));
    CHECK(v[2] == 3_b); // still in the borrowed region, just outside size()
    v.pop_back();
    v.pop_back();
    v.pop_back(); // pop on empty is a no-op
    CHECK(v.is_empty());
}

static void
test_push_back_emplace_back()
{
    std::array<std::byte, 4> s{};
    borrowed_byte_buffer v{s};
    const std::byte x = 10_b;
    v.push_back(x); // by value
    v.push_back(20_b);
    v.emplace_back(30); // int -> std::byte via functional cast
    v.emplace_back();   // no args -> std::byte{}
    CHECK(to_ivec(v) == std::vector({10, 20, 30, 0}));
}

static void
test_unchecked_push_back_unchecked_emplace_back()
{
    std::array<std::byte, 3> s{};
    borrowed_byte_buffer v{s};
    v.unchecked_emplace_back(1);   // int
    v.unchecked_push_back(2_b);
    v.unchecked_emplace_back(3_b); // byte
    CHECK(to_ivec(v) == std::vector({1, 2, 3}));
    CHECK(v.is_full());
}

static void
test_try_push_back_try_emplace_back()
{
    std::array<std::byte, 2> s{};
    borrowed_byte_buffer v{s};
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
    std::array<std::byte, 5> s{};
    borrowed_byte_buffer v{s};
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
    std::array<std::byte, 8> s{};
    s.fill(0xEE_b); // pre-dirty the borrowed region to prove the tail is really zeroed
    borrowed_byte_buffer v{s};
    v.append_range({1_b, 2_b, 3_b});
    v.zeroize_remaining_space(); // [size, capacity) is now zero; size unchanged
    CHECK(v.size() == 3);
    CHECK(v.capacity() == 8);
    CHECK(to_ivec(v) == std::vector({1, 2, 3}));
    for (std::size_t i = v.size(); i < v.capacity(); ++i)
        CHECK(v[i] == 0_b);
    // Scrub the whole region: clear() + zeroize_remaining_space() (non-elidable stores).
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

    std::array<std::byte, 12> s{};
    borrowed_byte_buffer v{s};
    v.append_range({1_b, 2_b, 3_b});                  // initializer_list
    v.append_range(std::span<const std::byte>{tail}); // span (memcpy fast path)
    v.append_range(more.begin(), more.end());         // iterator + sentinel
    v.append_range(more.begin(), std::size_t{1});     // iterator + count -> 6
    v.append_range(std::views::iota(8, 10) | std::views::transform(to_byte)); // range -> 8,9
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 4, 5, 6, 7, 6, 8, 9}));
}

static void
test_try_append_range()
{
    constexpr std::array a{1_b, 2_b};
    const std::vector more{5_b, 6_b};

    std::array<std::byte, 4> s{};
    borrowed_byte_buffer v{s};
    CHECK(v.try_append_range(std::span<const std::byte>{a})); // span
    CHECK(v.try_append_range({3_b, 4_b}));                    // initializer_list
    CHECK(!v.try_append_range({5_b, 6_b}));                   // would overflow -> false
    CHECK(!v.try_append_range(std::views::iota(0, 3) | std::views::transform(to_byte)));
    CHECK(!v.try_append_range(more.begin(), more.end()));     // sized sentinel: checked up front
    CHECK(!v.try_append_range(more.begin(), std::size_t{2})); // iterator + count
    CHECK(to_ivec(v) == std::vector({1, 2, 3, 4}));           // nothing appended by the failures
}

static void
test_assign_range()
{
    constexpr std::array arr{5_b, 6_b};
    const std::vector src{7_b, 8_b, 9_b};

    std::array<std::byte, 6> s{};
    borrowed_byte_buffer v{s};
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
    std::array<std::byte, 4> s{1_b, 2_b, 3_b, 4_b};
    borrowed_byte_buffer v = borrowed_byte_buffer::adopting(s);
    const std::span<const std::byte> s1 = v.span();
    const auto s2 = static_cast<std::span<std::byte>>(v); // operator std::span<std::byte>
    CHECK(s1.size() == 4);
    CHECK(s2.size() == 4);
    CHECK(v.data() == s1.data());
    CHECK(v.data() == s2.data());
    CHECK(v.data() == s.data()); // data() is the borrowed pointer, unadorned
}

static void
test_front_back()
{
    std::array<std::byte, 3> s{10_b, 20_b, 30_b};
    borrowed_byte_buffer v = borrowed_byte_buffer::adopting(s);
    CHECK(v.front() == 10_b);
    CHECK(v.back() == 30_b);
    v.front() = 11_b;
    v.back() = 31_b;
    CHECK(to_ivec(v) == std::vector({11, 20, 31}));
    CHECK(s[0] == 11_b); // writes through to the borrowed storage
}

static void
test_operator_index()
{
    std::array<std::byte, 5> s{};
    borrowed_byte_buffer v{s};
    v.append_range({11_b, 22_b, 33_b});
    CHECK(v[0] == 11_b);
    CHECK(v[2] == 33_b);
    v[1] = 99_b;
    CHECK(v[1] == 99_b);
    // Exercise -- but do not check the value of -- a read at an index >= size() within capacity:
    // operator[] is capacity-based, and the borrowed reserved tail is unspecified here.
    const auto probe = std::to_integer<unsigned>(v[v.capacity() - 1]);
    (void)probe;
}

static void
test_at()
{
    std::array<std::byte, 3> s{1_b, 2_b, 3_b};
    borrowed_byte_buffer v = borrowed_byte_buffer::adopting(s);
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
    std::array<std::byte, 3> s{1_b, 2_b, 3_b};
    const borrowed_byte_buffer v = borrowed_byte_buffer::adopting(s);
    CHECK(v.front() == 1_b);
    CHECK(v.back() == 3_b);
    CHECK(v[2] == 3_b);
    CHECK(v.at(2) == 3_b);
    CHECK(v.data() != nullptr);
    CHECK(v.span().size() == 3);
    CHECK(std::vector<std::byte>(v.begin(), v.end()) == std::vector({1_b, 2_b, 3_b}));
    CHECK(std::vector<std::byte>(v.rbegin(), v.rend()) == std::vector({3_b, 2_b, 1_b}));
    const auto sp = static_cast<std::span<const std::byte>>(v); // operator std::span<const byte>
    CHECK(sp.size() == 3);
    CHECK_THROWS(std::out_of_range, (void)v.at(3));
}

// ---- Iterators ----

static void
test_forward_iteration()
{
    std::array<std::byte, 3> s{1_b, 2_b, 3_b};
    borrowed_byte_buffer v = borrowed_byte_buffer::adopting(s);
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
    std::array<std::byte, 3> s{1_b, 2_b, 3_b};
    borrowed_byte_buffer v = borrowed_byte_buffer::adopting(s);
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
    std::array<std::byte, 10> s{};
    borrowed_byte_buffer a{s};
    a.append_range({1_b, 2_b, 3_b});
    std::array<std::byte, 3> sb{1_b, 2_b, 3_b};
    const borrowed_byte_buffer b = borrowed_byte_buffer::adopting(sb); // capacity 3
    CHECK(a == b);
    CHECK(a.capacity() != b.capacity());

    std::array<std::byte, 3> sc{1_b, 2_b, 4_b};
    std::array<std::byte, 2> sd{1_b, 2_b};
    const borrowed_byte_buffer c = borrowed_byte_buffer::adopting(sc);
    const borrowed_byte_buffer d = borrowed_byte_buffer::adopting(sd);
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
    // Free function (from byte_compare.hpp) for secret-dependent data; the container's operator==
    // stays variable-time.
    std::array<std::byte, 3> sa{1_b, 2_b, 3_b};
    std::array<std::byte, 3> sb{1_b, 2_b, 3_b};
    std::array<std::byte, 3> sc{1_b, 2_b, 4_b};
    const borrowed_byte_buffer a = borrowed_byte_buffer::adopting(sa);
    const borrowed_byte_buffer b = borrowed_byte_buffer::adopting(sb);
    const borrowed_byte_buffer c = borrowed_byte_buffer::adopting(sc);
    CHECK(constant_time_equal(a.span(), b.span()));
    CHECK(!constant_time_equal(a.span(), c.span()));
    CHECK(!constant_time_equal(a.span(), a.span().first(2))); // unequal sizes
    CHECK(constant_time_equal(std::span<const std::byte>{}, std::span<const std::byte>{}));
}

// ---- Overlaying a typed object (the motivating use case) ----

static void
test_overlay_object()
{
    // This test is endianness-agnostic: each scalar is copied in as its native object
    // representation and read back through the same-typed member, so the byte order cancels.
    // It never inspects a byte at a fixed offset, which is the only thing that would care.
    struct Header
    {
        std::uint32_t magic;
        std::uint16_t len;
        std::uint16_t flags;
    };
    static_assert(std::is_trivially_copyable_v<Header>);

    Header h{};
    borrowed_byte_buffer v{&h};
    CHECK(v.capacity() == sizeof(Header));
    CHECK(v.is_empty());

    // Build the header's bytes through the overlay; the writes land in `h` directly.
    const std::uint32_t magic = 0x01020304;
    const std::uint16_t len = 0x0506;
    const std::uint16_t flags = 0x0708;
    v.append_range(std::span<const std::byte>{reinterpret_cast<const std::byte*>(&magic),
                                              sizeof(magic)});
    v.append_range(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(&len), sizeof(len)});
    v.append_range(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(&flags), sizeof(flags)});
    CHECK(v.is_full());
    CHECK(h.magic == magic); // the object now holds what was written through its byte view
    CHECK(h.len == len);
    CHECK(h.flags == flags);
}

// ---- Overflow -> std::bad_alloc ----

static void
test_overflow_throws_bad_alloc()
{
    static constexpr std::array too_many{1_b, 2_b, 3_b};

    // The borrowing constructors reserve rather than creating elements, so only the modifiers can
    // overflow.  Backing storage is declared inside each check so it lives for the throwing call.
    CHECK_THROWS(std::bad_alloc, std::array<std::byte, 2> s{}; borrowed_byte_buffer v{s};
                 v.push_back(1_b); v.push_back(2_b); v.push_back(3_b));
    CHECK_THROWS(std::bad_alloc, std::array<std::byte, 1> s{}; borrowed_byte_buffer v{s};
                 v.emplace_back(1); v.emplace_back(2));
    CHECK_THROWS(std::bad_alloc, std::array<std::byte, 2> s{}; borrowed_byte_buffer v{s};
                 v.append_range(std::span<const std::byte>{too_many}));
    CHECK_THROWS(std::bad_alloc, std::array<std::byte, 2> s{}; borrowed_byte_buffer v{s};
                 v.resize(3));
    // assign_range keeps the current capacity, so a source that does not fit throws.
    CHECK_THROWS(std::bad_alloc, std::array<std::byte, 2> s{}; borrowed_byte_buffer v{s};
                 v.assign_range({1_b, 2_b, 3_b}));
}

int
main() // NOLINT(bugprone-exception-escape)
{
    return run_tests([] {
        test_ctor_default();
        test_ctor_ptr_capacity();
        test_ctor_range();
        test_ctor_range_capacity();
        test_ctor_single_object();
        test_adopting();
        test_write_shows_through();

        test_copy_ctor();
        test_move_ctor();
        test_copy_assign();
        test_move_assign();
        test_swap();

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

        test_overlay_object();

        test_overflow_throws_bad_alloc();
    });
}
