// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

/**
* \file
* \author Steven Ward
*
* Defines the class \c borrowed_byte_buffer, a run-time-capacity, non-owning buffer of
* \c std::byte overlaying storage it does not own.
*/

#pragma once

#include <algorithm>
#if defined(DEBUG)
#include <cassert>
#endif
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string.h> // memset_explicit, explicit_bzero
#include <type_traits>
#include <utility>

#include "byte_compare.hpp"

class borrowed_byte_buffer;

/// True if \a R is a contiguous range a \c borrowed_byte_buffer may borrow and write through
/**
* This constrains the two range-borrowing constructors and their \c adopting counterparts.  A
* concept is what lets all of the clauses sit in one place.  Its conjunction short-circuits, so
* \c range_value_t and \c range_reference_t are never formed for a non-range \a R, such as the
* single-object constructor's <code>unsigned int*</code>.
*
* Each clause earns its place:
*   - \c contiguous_range and \c sized_range.  The elements must be adjacent, and the byte size
*     must be known up front (\c std::ranges::size).  An unsized contiguous range is rejected
*     cleanly rather than hard-erroring in the constructor body.
*   - Not \c borrowed_byte_buffer itself.  This one is \b required, not cosmetic.  Without it,
*     constructing from a non-\c const \c borrowed_byte_buffer lvalue would prefer the range
*     constructor over the copy constructor, because it binds a less-cv-qualified reference, and
*     would reinterpret the source's own bytes.  The exclusion makes copy and move win.
*   - Trivially copyable, non-\c const elements.  The object representation is what gets written
*     and later read back, and the view writes through it.
*   - A \c borrowed_range \b or an lvalue.  An rvalue owning container, such as a temporary
*     \c std::vector, would leave a dangling view.  Only non-owning rvalues (\c std::span) and
*     lvalues are accepted.
*/
template <typename R>
concept borrowable_range =
    std::ranges::contiguous_range<R> && std::ranges::sized_range<R> &&
    !std::same_as<std::remove_cvref_t<R>, borrowed_byte_buffer> &&
    std::is_trivially_copyable_v<std::ranges::range_value_t<R>> &&
    !std::is_const_v<std::remove_reference_t<std::ranges::range_reference_t<R>>> &&
    (std::ranges::borrowed_range<R> || std::is_lvalue_reference_v<R>);

/// True if \a P is a pointer to a single writable, trivially copyable object
/**
* This constrains the single-object constructor, which takes a \e forwarding reference rather
* than a plain \c T* on purpose.  A \c T* parameter is (by partial ordering) more specialized
* than the range constructor's \c R&&, so a C array would decay to it and overlay only its first
* element.  Deducing \c P from the un-decayed argument and requiring \c std::is_pointer here
* rejects arrays, which then reach only the range constructor, while still accepting a genuine
* single-object pointer such as \c &obj.
*/
template <typename P>
concept borrowable_object_ptr =
    std::is_pointer_v<std::remove_cvref_t<P>> &&
    std::is_trivially_copyable_v<std::remove_pointer_t<std::remove_cvref_t<P>>> &&
    !std::is_const_v<std::remove_pointer_t<std::remove_cvref_t<P>>>;

/// A non-owning, run-time-capacity buffer of \c std::byte overlaying borrowed storage
/**
* The bytes belong to the caller.  Construction takes a pointer or a contiguous range whose
* lifetime the caller manages, and the buffer never allocates or frees.  What it adds is a
* fixed-capacity append interface (\c is_full, \c reserved_unused, \c append_range,
* \c push_back, ...) over memory it does not own.
*
* These properties shape the interface:
*   - \b Non-owning.  The object is just a pointer, a capacity, and a size, with no
*     allocation, a trivial destructor, and defaulted special members.  Copy and move are
*     shallow (both objects then view the same bytes, and move does not empty the source).  The
*     type is trivially copyable and cheap to pass by value.  Keeping the borrowed storage
*     alive for the buffer's lifetime is the caller's responsibility.  A destroyed source
*     leaves a dangling view.
*   - \b Capacity is supplied, not allocated.  There is no reserve / fill / iterator /
*     initializer-list / from-range \e element-copying constructor.  A borrowed buffer is
*     built directly over existing memory (a pointer, or a contiguous range whose storage it
*     \e overlays rather than copies), then filled via \c append_range / \c assign_range.
*   - \b Construction leaves \c size()==0, so the region is treated as empty space to build
*     into.  To instead adopt bytes already present in the region (for reading, iterating, or
*     comparing), use the \c adopting named constructors, which start \c size()==capacity().
*   - \b No \a Align parameter.  Borrowed memory carries no alignment promise, so \c data()
*     returns the raw pointer unadorned, with no \c std::assume_aligned.
*   - \b No \c operator=(initializer_list).  Writing through a view-like type via assignment
*     reads as rebinding rather than a bulk store.  Use \c assign_range for that.
*   - The reserved tail [\c size(), \c capacity()) is left as-is.  Borrowed bytes are neither
*     zeroed nor read on construction.
*   - \c operator[] is unchecked, and its bound is \c capacity() rather than \c size().
*     \c at() is the bounds-checked accessor.
*   - Capacity overflow throws \c std::bad_alloc.  The \c try_* family returns \c bool
*     instead of throwing.
*   - \c zeroize_reserved_unused() and the free \c equal_constant_time (from
*     \c byte_compare.hpp) are available.
*
* The element type of a source must be trivially copyable and non-\c const (see
* \c borrowable_range).
*
* Nearly the whole interface is \c constexpr, but forming a byte view over an object needs a
* \c reinterpret_cast, which is barred in constant evaluation, so only the default (empty)
* instance is usable in constant expressions.
*
* \note A read past \c size() returns the caller's own bytes.  The container promises nothing
* about their value.  In practice the tail is memory the caller owns and most likely
* initialized, so the byte is usually perfectly determinate.
* \note Those bytes are the caller's data, which a beyond-size read (or an \c operator== after
* a \c resize, or a \c span() handed onward) will disclose.  Call
* \c zeroize_reserved_unused() when the tail must not leak.
*
* \invariant <code>size() <= capacity()</code>
* \note \c data() carries no null-when-empty guarantee.  A caller may borrow a zero-length
* region at a non-null address.  The mutating members index \c data() only under
* \c !is_full() / \c !is_empty() / <code>i < capacity()</code>, each of which implies
* <code>capacity() > 0</code> and therefore (by the constructor precondition that the source
* points to at least \c capacity() writable bytes) a non-null, indexable block.
*/
class borrowed_byte_buffer
{
private:
    std::byte* data_ = nullptr;
    std::size_t capacity_{};
    std::size_t size_{};

    constexpr void check_idx_(const std::size_t i) const
    {
        if (i >= size())
            throw std::out_of_range("borrowed_byte_buffer: index >= size");
    }

    /**
    * \pre \a spn does not overlap this buffer's storage.
    */
    constexpr void common_append_range_(const std::span<const std::byte> spn) noexcept
    {
        if (!spn.empty())
            (void)std::memcpy(end(), std::data(spn), std::size(spn));
        size_ += std::size(spn);
    }

    template <std::input_iterator It>
    constexpr void common_append_range_(It first, const std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            unchecked_emplace_back(*first);
            ++first;
        }
    }

    /// True if \a R is a sized, contiguous range of \c std::byte
    /**
    * Such a range is handed to the \c std::span overload for its \c std::memcpy.  Overload
    * resolution will not do that on its own, since the \c R&& template is an exact match for a
    * \c std::vector<std::byte> where the \c std::span overload needs a user-defined conversion.
    * Without this test, only a hand-written span would ever reach the \c memcpy.
    */
    template <typename R>
    static constexpr bool is_bulk_appendable_ =
        std::ranges::contiguous_range<R> && std::ranges::sized_range<R> &&
        std::same_as<std::ranges::range_value_t<R>, std::byte>;

    /// View \a rg as the \c std::span of \c const \c std::byte the \c memcpy overload takes
    template <typename R>
    requires is_bulk_appendable_<R>
    [[nodiscard]] static constexpr std::span<const std::byte> as_span_(R& rg)
    {
        return std::span{rg};
    }

    /// The pointee size of a \c P accepted by \c borrowable_object_ptr (the capacity)
    template <borrowable_object_ptr P>
    static constexpr std::size_t object_ptr_size_ =
        sizeof(std::remove_pointer_t<std::remove_cvref_t<P>>);

    /// Zero \a n bytes at \a p with stores that the compiler must not elide
    /**
    * Uses \c ::memset_explicit (C23) or \c ::explicit_bzero (glibc, BSDs) when the C library
    * declares one, else writes through a \c volatile pointer.  Neither has a feature-test
    * macro, so availability is probed by unqualified name lookup on the dependent parameter
    * \a P.
    */
    template <typename P>
    static void zero_explicit_(P const p, const std::size_t n) noexcept
    {
        // Do not change these to std::memset_explicit.  A name qualified into a namespace
        // that lacks the member is a hard error, not a substitution failure, so the probe
        // could not reject it.
        if constexpr (requires { ::memset_explicit(p, 0, n); })
        {
            (void)::memset_explicit(p, 0, n);
        }
        else if constexpr (requires { ::explicit_bzero(p, n); })
        {
            ::explicit_bzero(p, n);
        }
        else
        {
            volatile auto* const q = static_cast<volatile unsigned char*>(p);
            for (std::size_t i = 0; i < n; ++i)
            {
                q[i] = 0;
            }
        }
    }

public:
    using value_type = std::byte;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = value_type&;
    using const_reference = const value_type&;
    using pointer = value_type*;
    using const_pointer = const value_type*;
    using iterator = pointer;
    using const_iterator = const_pointer;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    constexpr borrowed_byte_buffer() noexcept = default;
    constexpr borrowed_byte_buffer(const borrowed_byte_buffer&) noexcept = default;
    constexpr borrowed_byte_buffer(borrowed_byte_buffer&&) noexcept = default;
    constexpr borrowed_byte_buffer& operator=(const borrowed_byte_buffer&) noexcept = default;
    constexpr borrowed_byte_buffer& operator=(borrowed_byte_buffer&&) noexcept = default;
    ~borrowed_byte_buffer() = default;

    /// Borrow \a capacity bytes at \a data, leaving the buffer empty (\c size()==0)
    /**
    * \pre \a data points to at least \a capacity writable bytes.
    */
    borrowed_byte_buffer(void* const data, const std::size_t capacity) noexcept
        : data_{static_cast<std::byte*>(data)}, capacity_{capacity}
    {}

    /// Borrow \a capacity bytes of the range \a r, leaving the buffer empty (\c size()==0)
    /**
    * \a r is any contiguous, sized range of writable, trivially copyable elements, e.g. a
    * \c std::array, \c std::vector, \c std::span, or C array (see \c borrowable_range).
    * Its storage is overlaid, not copied.
    * \pre \a r's byte size is at least \a capacity.
    */
    template <borrowable_range R>
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
    explicit borrowed_byte_buffer(R&& r, const std::size_t capacity) noexcept
        : data_{reinterpret_cast<std::byte*>(std::ranges::data(r))}, capacity_{capacity}
    {
#if defined(DEBUG)
        assert(this->capacity() <= std::span{r}.size_bytes());
#endif
    }

    /// Borrow all of the range \a r as empty space, with its byte size as the capacity
    /**
    * \a r is any contiguous, sized range of writable, trivially copyable elements, e.g. a
    * \c std::array, \c std::vector, \c std::span, or C array (see \c borrowable_range).
    * Its storage is overlaid, not copied.  Taking the whole range, this carries no size
    * precondition of its own, unlike the overload that takes a capacity.
    */
    template <borrowable_range R>
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
    explicit borrowed_byte_buffer(R&& r) noexcept
        : data_{reinterpret_cast<std::byte*>(std::ranges::data(r))},
          capacity_{std::span{r}.size_bytes()}
    {}

    /// Overlay the single object \a data, with the pointee's \c sizeof as the capacity
    /**
    * The buffer starts empty.  This accepts a genuine object pointer (e.g. \c &obj), not an
    * array, which the range constructor handles.
    */
    template <borrowable_object_ptr P>
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
    explicit borrowed_byte_buffer(P&& data) noexcept
        : data_{reinterpret_cast<std::byte*>(data)}, capacity_{object_ptr_size_<P>}
    {}

    /// Adopt the bytes already present in the borrowed region, so \c size()==capacity()
    /**
    * The \c adopting family mirrors the value constructors' source forms but starts the buffer
    * \e full, so \c span(), the iterators, and \c operator== immediately see the bytes already
    * in the region.  That suits reading, iterating, or comparing memory that is already
    * populated (a header, a received packet, a key), rather than building into an empty
    * region.
    *
    * \note Adopting is about where the size cursor starts, not about read-only access.  These
    * take the same sources the value constructors do, so the storage must still be \b writable
    * and its elements non-\c const (\c borrowable_range / \c borrowable_object_ptr).  A
    * \c const source is rejected even though adopting it would only be read, because the type
    * writes through its pointer and so cannot hold one.  View \c const bytes with \c std::span
    * instead.
    * \pre The source points to at least \c capacity() writable bytes.
    */
    [[nodiscard]] static borrowed_byte_buffer
    adopting(void* const data, const std::size_t capacity) noexcept
    {
        borrowed_byte_buffer b{data, capacity};
        b.size_ = b.capacity();
        return b;
    }

    /// \copydoc adopting(void*,std::size_t)
    template <borrowable_range R>
    [[nodiscard]] static borrowed_byte_buffer adopting(R&& r, const std::size_t capacity) noexcept
    {
        borrowed_byte_buffer b{std::forward<R>(r), capacity};
        b.size_ = b.capacity();
        return b;
    }

    /// \copydoc adopting(void*,std::size_t)
    template <borrowable_range R>
    [[nodiscard]] static borrowed_byte_buffer adopting(R&& r) noexcept
    {
        borrowed_byte_buffer b{std::forward<R>(r)};
        b.size_ = b.capacity();
        return b;
    }

    /// \copydoc adopting(void*,std::size_t)
    template <borrowable_object_ptr P>
    [[nodiscard]] static borrowed_byte_buffer adopting(P&& data) noexcept
    {
        borrowed_byte_buffer b{std::forward<P>(data)};
        b.size_ = b.capacity();
        return b;
    }

    constexpr void swap(borrowed_byte_buffer& other) noexcept
    {
        std::swap(data_, other.data_);
        std::swap(capacity_, other.capacity_);
        std::swap(size_, other.size_);
    }

    friend constexpr void swap(borrowed_byte_buffer& a, borrowed_byte_buffer& b) noexcept
    {
        a.swap(b);
    }

    [[nodiscard]] constexpr std::size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] constexpr std::size_t max_size() const noexcept { return capacity_; }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }

    /// Get the amount of reserved unused space (i.e., between \c size() and \c capacity())
    [[nodiscard]] constexpr std::size_t reserved_unused() const noexcept
    {
        return capacity() - size();
    }

    [[nodiscard]] constexpr bool is_empty() const noexcept { return size() == 0; }

    [[nodiscard]] constexpr bool is_full() const noexcept { return size() == capacity(); }

    /**
    * \note The bytes are not zeroed but stay in the borrowed region, and setting the size to 0
    * moves all of them into the reserved-unused tail, where \c operator[] still reads them.
    * \c clear() followed by \c zeroize_reserved_unused() scrubs them.
    */
    constexpr void clear() noexcept { size_ = 0; }

    /// Resize to \a count bytes
    /**
    * Growing sets the new bytes to \a value.  Shrinking leaves the removed ones unchanged.
    * \note <code>resize(capacity(), value)</code> is how to fill only the reserved-unused tail
    * [\c size(), \c capacity()) and grow into it.  \c fill_capacity() overwrites the live
    * bytes as well.
    * \note The bound is \c capacity(), which is the borrowed region, so growing past it throws
    * rather than reaching outside what the caller lent.
    * \exception std::bad_alloc if \a count > \c capacity().
    */
    constexpr void resize(const std::size_t count, const std::byte value)
    {
        if (count > capacity())
            throw std::bad_alloc{};

        if (count > size())
            (void)std::memset(end(), std::to_integer<int>(value), count - size());

        size_ = count;
    }

    /**
    * \exception std::bad_alloc if \a count > \c capacity().
    */
    constexpr void resize(const std::size_t count) { resize(count, std::byte{}); }

    /**
    * \note Popping an empty buffer is a no-op, unlike \c std::inplace_vector::pop_back,
    * where it is UB.
    */
    constexpr void pop_back() noexcept
    {
        if (is_empty())
            return;

        --size_;
    }

    /**
    * \pre \c !is_full()
    * \note An integral argument is converted as by \c static_cast, so an out-of-range value
    * truncates mod 256.
    * \note "Emplace" is assignment here.  The slot already holds a live byte.
    */
    template <class... Args>
    requires (sizeof...(Args) <= 1) &&
             ((std::same_as<std::remove_cvref_t<Args>, std::byte> ||
               std::integral<std::remove_cvref_t<Args>>) && ...)
    constexpr void unchecked_emplace_back(Args&&... args) noexcept
    {
#if defined(DEBUG)
        assert(!is_full());
#endif
        *end() = std::byte(std::forward<Args>(args)...);
        ++size_;
    }

    /**
    * \exception std::bad_alloc if \c is_full().
    */
    template <class... Args>
    requires (sizeof...(Args) <= 1) &&
             ((std::same_as<std::remove_cvref_t<Args>, std::byte> ||
               std::integral<std::remove_cvref_t<Args>>) && ...)
    constexpr void emplace_back(Args&&... args)
    {
        if (is_full())
            throw std::bad_alloc{};

        unchecked_emplace_back(std::forward<Args>(args)...);
    }

    template <class... Args>
    requires (sizeof...(Args) <= 1) &&
             ((std::same_as<std::remove_cvref_t<Args>, std::byte> ||
               std::integral<std::remove_cvref_t<Args>>) && ...)
    [[nodiscard]] constexpr bool try_emplace_back(Args&&... args) noexcept
    {
        if (is_full())
            return false;

        unchecked_emplace_back(std::forward<Args>(args)...);
        return true;
    }

    /**
    * \pre \c !is_full()
    */
    constexpr void unchecked_push_back(const std::byte value) noexcept
    {
        unchecked_emplace_back(value);
    }

    /**
    * \exception std::bad_alloc if \c is_full().
    */
    constexpr void push_back(const std::byte value) { emplace_back(value); }

    [[nodiscard]] constexpr bool try_push_back(const std::byte value) noexcept
    {
        return try_emplace_back(value);
    }

    /// Fill all \c capacity() bytes with \a value and set \c size() to \c capacity()
    constexpr void fill_capacity(const std::byte value) noexcept
    {
        if (capacity() != 0)
            (void)std::memset(data(), std::to_integer<int>(value), capacity());
        size_ = capacity();
    }

    /// Fill the live bytes [0, \c size()) with \a value, leaving \c size() unchanged
    constexpr void fill_size(const std::byte value) noexcept
    {
        if (size() != 0)
            (void)std::memset(data(), std::to_integer<int>(value), size());
    }

    /// Zeroize the reserved tail [\c size(), \c capacity()), leaving \c size() unchanged
    constexpr void zeroize_reserved_unused() noexcept
    {
        if (reserved_unused() != 0)
            zero_explicit_(static_cast<void*>(end()), reserved_unused());
    }

    /**
    * \pre \a spn does not overlap this buffer's storage.
    * \note The check is made up front, so nothing is appended when it throws.
    * \exception std::bad_alloc if \a spn does not fit in \c reserved_unused().
    */
    constexpr void append_range(const std::span<const std::byte> spn)
    {
        if (std::size(spn) > reserved_unused())
            throw std::bad_alloc{};

        common_append_range_(spn);
    }

    /**
    * \pre <code>[first, last)</code> is a valid range.  For a \c std::sized_sentinel_for this
    * keeps <code>last - first</code> non-negative, so the size check's cast to \c std::size_t
    * is well-defined.
    * \note A \c std::sized_sentinel_for source is checked up front, so nothing is appended when
    * it throws.  An unsized one appends the bytes that fit before throwing.
    * \exception std::bad_alloc if the source does not fit in \c reserved_unused().
    */
    template <std::input_iterator It, std::sentinel_for<It> S>
    constexpr void append_range(It first, S last)
    {
        if constexpr (std::sized_sentinel_for<S, It>)
        {
            if (static_cast<std::size_t>(last - first) > reserved_unused())
                throw std::bad_alloc{};
        }

        for (; first != last; ++first)
            emplace_back(*first);
    }

    /**
    * \note The check is made up front, so nothing is appended when it throws.
    * \exception std::bad_alloc if \a count > \c reserved_unused().
    */
    template <std::input_iterator It>
    constexpr void append_range(It first, const std::size_t count)
    {
        if (count > reserved_unused())
            throw std::bad_alloc{};

        common_append_range_(first, count);
    }

    /**
    * \note The check is made up front, so nothing is appended when it throws.
    * \exception std::bad_alloc if \a il does not fit in \c reserved_unused().
    */
    constexpr void append_range(const std::initializer_list<std::byte> il)
    {
        append_range(std::span{std::data(il), std::size(il)});
    }

    /**
    * \pre \a rg does not overlap this buffer's storage if it is a contiguous range of
    * \c std::byte.
    * \note A sized source is checked up front, so nothing is appended when it throws.  An
    * unsized one appends the bytes that fit before throwing.
    * \exception std::bad_alloc if the source does not fit in \c reserved_unused().
    */
    template <std::ranges::input_range R>
    constexpr void append_range(R&& rg)
    {
        if constexpr (is_bulk_appendable_<R>)
        {
            append_range(as_span_(rg));
        }
        else if constexpr (std::ranges::sized_range<R>)
        {
            if (std::ranges::size(rg) > reserved_unused())
                throw std::bad_alloc{};

            // The size check above covers every element, so skip the per-element repeat.
            for (auto&& e : std::forward<R>(rg))
                unchecked_emplace_back(std::forward<decltype(e)>(e));
        }
        else
        {
            for (auto&& e : std::forward<R>(rg))
                emplace_back(std::forward<decltype(e)>(e));
        }
    }

    /**
    * \pre \a spn does not overlap this buffer's storage.
    */
    [[nodiscard]] constexpr bool try_append_range(const std::span<const std::byte> spn) noexcept
    {
        if (std::size(spn) > reserved_unused())
            return false;

        common_append_range_(spn);
        return true;
    }

    /**
    * \pre <code>[first, last)</code> is a valid range.  For a \c std::sized_sentinel_for this
    * keeps <code>last - first</code> non-negative, so the size check's cast to \c std::size_t
    * is well-defined.
    * \note A \c std::sized_sentinel_for source is checked up front, so nothing is appended on
    * \c false.  An unsized one has already appended the bytes that fit when \c false is
    * returned.
    */
    template <std::input_iterator It, std::sentinel_for<It> S>
    [[nodiscard]] constexpr bool try_append_range(It first, S last)
    {
        if constexpr (std::sized_sentinel_for<S, It>)
        {
            if (static_cast<std::size_t>(last - first) > reserved_unused())
                return false;
        }

        for (; first != last; ++first)
        {
            if (!try_emplace_back(*first))
                return false;
        }

        return true;
    }

    template <std::input_iterator It>
    [[nodiscard]] constexpr bool try_append_range(It first, const std::size_t count)
    {
        if (count > reserved_unused())
            return false;

        common_append_range_(first, count);
        return true;
    }

    [[nodiscard]] constexpr bool
    try_append_range(const std::initializer_list<std::byte> il) noexcept
    {
        return try_append_range(std::span{std::data(il), std::size(il)});
    }

    /**
    * \pre \a rg does not overlap this buffer's storage if it is a contiguous range of
    * \c std::byte.
    * \note A sized source is checked up front, so nothing is appended on \c false.  An unsized
    * one has already appended the bytes that fit when \c false is returned.
    */
    template <std::ranges::input_range R>
    [[nodiscard]] constexpr bool try_append_range(R&& rg)
    {
        if constexpr (is_bulk_appendable_<R>)
        {
            return try_append_range(as_span_(rg));
        }
        else if constexpr (std::ranges::sized_range<R>)
        {
            if (std::ranges::size(rg) > reserved_unused())
                return false;

            // The size check above covers every element, so skip the per-element repeat.
            for (auto&& e : std::forward<R>(rg))
                unchecked_emplace_back(std::forward<decltype(e)>(e));

            return true;
        }
        else
        {
            // NOLINTNEXTLINE(readability-use-anyofallof)
            for (auto&& e : std::forward<R>(rg))
            {
                if (!try_emplace_back(std::forward<decltype(e)>(e)))
                    return false;
            }

            return true;
        }
    }

    /// \c clear() followed by \c append_range(), so the source is bounded by \c capacity()
    /**
    * \pre The source does not overlap this buffer's storage.
    * \note The capacity is kept.  Assigning does not re-borrow, so this never changes which
    * region the buffer views.
    * \note The \c clear() happens first, so the previous contents are gone whether the assign
    * succeeds or fails.  A sized source then leaves the buffer empty, and an unsized one leaves
    * the bytes that fit.
    * \exception std::bad_alloc if the source does not fit in \c capacity().
    */
    constexpr void assign_range(const std::span<const std::byte> spn)
    {
        clear();
        append_range(spn);
    }

    /// \copydoc assign_range(std::span<const std::byte>)
    template <std::input_iterator It, std::sentinel_for<It> S>
    constexpr void assign_range(It first, S last)
    {
        clear();
        append_range(first, last);
    }

    /// \copydoc assign_range(std::span<const std::byte>)
    template <std::input_iterator It>
    constexpr void assign_range(It first, const std::size_t count)
    {
        clear();
        append_range(first, count);
    }

    /// \copydoc assign_range(std::span<const std::byte>)
    constexpr void assign_range(const std::initializer_list<std::byte> il)
    {
        clear();
        append_range(il);
    }

    /// \copydoc assign_range(std::span<const std::byte>)
    template <std::ranges::input_range R>
    constexpr void assign_range(R&& rg)
    {
        clear();
        append_range(std::forward<R>(rg));
    }

    [[nodiscard]] constexpr std::span<std::byte> span() noexcept { return {data(), size()}; }

    [[nodiscard]] constexpr std::span<const std::byte> span() const noexcept
    {
        return {data(), size()};
    }

    [[nodiscard]] constexpr explicit operator std::span<std::byte>() noexcept { return span(); }

    [[nodiscard]] constexpr explicit operator std::span<const std::byte>() const noexcept
    {
        return span();
    }

    /**
    * \return The borrowed pointer, raw and with no assumed alignment.
    */
    [[nodiscard]] constexpr std::byte* data() noexcept { return data_; }

    /// \copydoc data()
    [[nodiscard]] constexpr const std::byte* data() const noexcept { return data_; }

    /**
    * \pre \c !is_empty()
    */
    [[nodiscard]] constexpr std::byte& front() noexcept
    {
#if defined(DEBUG)
        assert(!is_empty());
#endif
        return *begin();
    }

    /// \copydoc front()
    [[nodiscard]] constexpr const std::byte& front() const noexcept
    {
#if defined(DEBUG)
        assert(!is_empty());
#endif
        return *begin();
    }

    /**
    * \pre \c !is_empty()
    */
    [[nodiscard]] constexpr std::byte& back() noexcept
    {
#if defined(DEBUG)
        assert(!is_empty());
#endif
        return *rbegin();
    }

    /// \copydoc back()
    [[nodiscard]] constexpr const std::byte& back() const noexcept
    {
#if defined(DEBUG)
        assert(!is_empty());
#endif
        return *rbegin();
    }

    /**
    * \pre \a i < \c capacity()
    * \note The index is unchecked and bounded by \c capacity(), not \c size(), so
    * an index in [size(), capacity()) is a valid read that returns whatever the
    * borrowed region holds there.
    */
    [[nodiscard]] constexpr std::byte& operator[](const std::size_t i) noexcept
    {
#if defined(DEBUG)
        assert(i < capacity());
#endif
        return data()[i];
    }

    /// \copydoc operator[](std::size_t)
    [[nodiscard]] constexpr const std::byte& operator[](const std::size_t i) const noexcept
    {
#if defined(DEBUG)
        assert(i < capacity());
#endif
        return data()[i];
    }

    /**
    * \note This is the only bounds-checked accessor, and it checks against \c size(), so it
    * rejects an index in [size(), capacity()) where \c operator[] returns the borrowed
    * region's own byte.
    * \exception std::out_of_range if \a i >= \c size().
    */
    [[nodiscard]] constexpr std::byte& at(const std::size_t i)
    {
        check_idx_(i);
        return data()[i];
    }

    /// \copydoc at(std::size_t)
    [[nodiscard]] constexpr const std::byte& at(const std::size_t i) const
    {
        check_idx_(i);
        return data()[i];
    }

    [[nodiscard]] constexpr std::byte* begin() noexcept { return data(); }

    [[nodiscard]] constexpr const std::byte* begin() const noexcept { return data(); }

    [[nodiscard]] constexpr const std::byte* cbegin() const noexcept { return data(); }

    [[nodiscard]] constexpr std::byte* end() noexcept { return data() + size(); }

    [[nodiscard]] constexpr const std::byte* end() const noexcept { return data() + size(); }

    [[nodiscard]] constexpr const std::byte* cend() const noexcept { return data() + size(); }

    [[nodiscard]] constexpr std::reverse_iterator<std::byte*> rbegin() noexcept
    {
        return std::reverse_iterator(end());
    }

    [[nodiscard]] constexpr std::reverse_iterator<const std::byte*> rbegin() const noexcept
    {
        return std::reverse_iterator(end());
    }

    [[nodiscard]] constexpr std::reverse_iterator<const std::byte*> crbegin() const noexcept
    {
        return std::reverse_iterator(cend());
    }

    [[nodiscard]] constexpr std::reverse_iterator<std::byte*> rend() noexcept
    {
        return std::reverse_iterator(begin());
    }

    [[nodiscard]] constexpr std::reverse_iterator<const std::byte*> rend() const noexcept
    {
        return std::reverse_iterator(begin());
    }

    [[nodiscard]] constexpr std::reverse_iterator<const std::byte*> crend() const noexcept
    {
        return std::reverse_iterator(cbegin());
    }

    [[nodiscard]] constexpr bool operator==(const borrowed_byte_buffer& rhs) const noexcept
    {
        return std::ranges::equal(span(), rhs.span());
    }

    [[nodiscard]] constexpr auto
    operator<=>(const borrowed_byte_buffer& rhs) const noexcept
    {
        return std::lexicographical_compare_three_way(begin(), end(), rhs.begin(), rhs.end());
    }
};
