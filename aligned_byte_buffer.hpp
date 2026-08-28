// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

/**
* \file
* \author Steven Ward
*
* Defines the class \c aligned_byte_buffer, a run-time-capacity, over-aligned buffer of
* \c std::byte.
*/

#pragma once

#include <algorithm>
#include <bit>
#if defined(DEBUG)
#include <cassert>
#endif
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string.h> // memset_explicit, explicit_bzero
#include <type_traits>
#include <utility>

#include "byte_compare.hpp"

/// A resizable, fixed-capacity buffer of \c std::byte with over-alignable storage
/**
* The capacity is a constructor argument rather than a template parameter, so \c capacity() and
* \c max_size() are non-static.  It is settled at construction and never changes, since changing
* it would mean reallocating.  The element type is fixed to \c std::byte, which is what lets the
* implementation move bytes in bulk.
*
* The properties that shape the interface:
*   - The only template parameter is the alignment \a Align (a power of two, defaulting to 16).
*     \c aligned_byte_buffer<16> and \c aligned_byte_buffer<32> are distinct types.
*   - Storage is a heap block allocated with the aligned \c ::operator \c new and owned by a
*     \c std::unique_ptr.  Because \c sizeof(std::byte)==1, the allocation size is exactly the
*     capacity.  There is no multiplication and no overflow check.
*   - \c data() applies \c std::assume_aligned<Align> so caller loops can vectorize.
*   - Reserved-but-unused capacity is left \b uninitialized.  Storage lifetime is begun with
*     \c std::start_lifetime_as_array (no whole-capacity zeroing).  Bytes that enter \c size()
*     are always written.  Reading beyond \c size() via \c operator[] yields an \e unspecified
*     byte value, which is well-defined (not UB) for \c std::byte.
*   - \c operator[] is unchecked, and its bound is \c capacity() rather than \c size().
*     \c at() is the bounds-checked accessor.
*   - Nothing is zeroed on removal.  \c clear(), \c pop_back(), and \c resize() only change the
*     size.  \c zeroize_reserved_unused() zeros the reserved tail with non-elidable stores, and
*     \c clear() followed by it scrubs the whole buffer.
*   - The \c emplace_back family accepts at most one argument, of type \c std::byte or an
*     integral type (floating-point and other enumeration arguments are rejected).
*   - Capacity overflow throws \c std::bad_alloc.  The \c try_* family returns \c bool
*     instead of throwing.
*
* The interface is annotated \c constexpr, but over-aligned allocation is not usable in constant
* evaluation, so only empty (non-allocating) instances are usable in constant expressions.
*
* \invariant <code>size() <= capacity()</code>
* \invariant \c data() is null \b exactly when \c capacity() is 0.  A capacity of 0 allocates
* nothing, and the aligned <code>::operator new</code> never returns null (it throws), so no other
* state holds a null block.
*
* Together those make the preconditions below sufficient on their own.  \c !is_full(),
* \c !is_empty(), and <code>i < capacity()</code> each imply a non-null, \a Align-aligned block,
* so the members carrying them index \c data() without re-checking it for null.
*/
template <std::size_t Align = 16>
requires (std::has_single_bit(Align))
class aligned_byte_buffer
{
private:
    /// Stateless deleter that frees a block from the aligned <code>::operator new</code>
    struct aligned_deleter
    {
        constexpr void operator()(std::byte* const p) const noexcept
        {
            ::operator delete(p, std::align_val_t{Align});
        }
    };

    using storage_ptr = std::unique_ptr<std::byte, aligned_deleter>;

    std::size_t size_{};
    std::size_t capacity_{};
    storage_ptr data_{};

    /// Allocate an over-aligned, \b uninitialized block of \a cap bytes
    [[nodiscard]] static constexpr storage_ptr allocate_(const std::size_t cap)
    {
        // This early return is not an optimization.  ::operator new(0) returns a non-null
        // block, so without it the invariant "capacity 0 implies null data()" would not hold.
        if (cap == 0)
            return nullptr;

        // sizeof(std::byte) == 1, so the byte count is exactly cap and no overflow is possible.
        void* const raw = ::operator new(cap, std::align_val_t{Align});
        auto* const p = std::start_lifetime_as_array<std::byte>(raw, cap);
        return storage_ptr{p};
    }

    constexpr void check_idx_(const std::size_t i) const
    {
        if (i >= size())
            throw std::out_of_range("aligned_byte_buffer: index >= size");
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

    /// Zero \a n bytes at \a p with stores that the compiler must not elide
    /**
    * Uses \c ::memset_explicit (C23) or \c ::explicit_bzero (glibc, BSDs) when the C library
    * declares one, else writes through a \c volatile pointer.  Neither has a feature-test
    * macro, so availability is probed by unqualified name lookup on the dependent parameter
    * \a P.
    *
    * \note The lookup must stay unqualified, so do \b not "modernize" it to
    * \c std::memset_explicit.  A qualified name into a namespace that lacks the member is a
    * hard error rather than a substitution failure, so the \c requires probe cannot reject it
    * and the build fails outright.  libstdc++ 16 declares no such name at any \c -std, and a
    * later release that adds it would not lift the rule, since \c <string.h> declares the C
    * spelling at global scope where the unqualified probe already finds it.
    */
    template <typename P>
    static void zero_explicit_(P const p, const std::size_t n) noexcept
    {
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

    constexpr aligned_byte_buffer() noexcept = default;

    /**
    * \exception std::bad_alloc if the allocation fails.
    */
    constexpr aligned_byte_buffer(const aligned_byte_buffer& other)
        : size_{other.size_}, capacity_{other.capacity_}, data_{allocate_(other.capacity_)}
    {
        // The reserved tail is unspecified, so only the live [0,size) bytes are copied.
        if (size() != 0)
            (void)std::memcpy(data(), other.data(), size());
    }

    constexpr aligned_byte_buffer(aligned_byte_buffer&& other) noexcept
        : size_{std::exchange(other.size_, 0)},
          capacity_{std::exchange(other.capacity_, 0)},
          data_{std::move(other.data_)}
    {}

    /**
    * \exception std::bad_alloc if the allocation fails.
    */
    constexpr aligned_byte_buffer& operator=(const aligned_byte_buffer& other)
    {
        if (this == &other)
        {
            return *this;
        }
        aligned_byte_buffer tmp{other};
        swap(tmp);
        return *this;
    }

    /// Swap-based move assignment
    /**
    * \a other is left holding this buffer's former contents rather than being emptied.  That
    * storage is freed when \a other is destroyed.
    */
    constexpr aligned_byte_buffer& operator=(aligned_byte_buffer&& other) noexcept
    {
        swap(other);
        return *this;
    }

    ~aligned_byte_buffer() = default;

    /// Reserve capacity \a capacity, leaving the buffer empty
    /**
    * \exception std::bad_alloc if the allocation fails.
    */
    constexpr explicit aligned_byte_buffer(const std::size_t capacity)
        : capacity_{capacity}, data_{allocate_(capacity)}
    {}

    /// Reserve capacity \a capacity and fill it with \a value (\c size()==capacity)
    /**
    * \copydetails aligned_byte_buffer(std::size_t)
    */
    constexpr explicit aligned_byte_buffer(const std::size_t capacity, const std::byte value)
        : size_{capacity}, capacity_{capacity}, data_{allocate_(capacity)}
    {
        if (this->capacity() != 0)
            (void)std::memset(data(), std::to_integer<int>(value), this->capacity());
    }

    /// Capacity is the size of \a spn
    /**
    * \exception std::bad_alloc if the allocation fails.
    */
    constexpr explicit aligned_byte_buffer(const std::span<const std::byte> spn)
        : aligned_byte_buffer(std::size(spn))
    {
        common_append_range_(spn);
    }

    /// Capacity is the distance between \a first and \a last
    /**
    * \exception std::bad_alloc if the allocation fails.
    */
    template <std::forward_iterator It, std::sentinel_for<It> S>
    constexpr explicit aligned_byte_buffer(It first, S last)
        : aligned_byte_buffer(static_cast<std::size_t>(std::ranges::distance(first, last)))
    {
        for (; first != last; ++first)
            unchecked_emplace_back(*first);
    }

    /// Capacity is \a count
    /**
    * \exception std::bad_alloc if the allocation fails.
    */
    template <std::input_iterator It>
    constexpr explicit aligned_byte_buffer(It first, const std::size_t count)
        : aligned_byte_buffer(count)
    {
        common_append_range_(first, count);
    }

    /// Capacity is the size of \a il
    /**
    * \exception std::bad_alloc if the allocation fails.
    */
    constexpr aligned_byte_buffer(const std::initializer_list<std::byte> il)
        : aligned_byte_buffer(std::span{std::data(il), std::size(il)})
    {}

    /// Capacity is the size of \a rg
    /**
    * \exception std::bad_alloc if the allocation fails.
    */
    template <std::ranges::forward_range R>
    constexpr explicit aligned_byte_buffer(std::from_range_t, R&& rg)
        : aligned_byte_buffer(static_cast<std::size_t>(std::ranges::distance(rg)))
    {
        for (auto&& e : std::forward<R>(rg))
            unchecked_emplace_back(std::forward<decltype(e)>(e));
    }

    /**
    * \exception std::bad_alloc if \a il does not fit in \c capacity().
    */
    constexpr aligned_byte_buffer& operator=(const std::initializer_list<std::byte> il)
    {
        assign_range(il);
        return *this;
    }

    constexpr void swap(aligned_byte_buffer& other) noexcept
    {
        std::swap(size_, other.size_);
        std::swap(capacity_, other.capacity_);
        std::swap(data_, other.data_);
    }

    friend constexpr void swap(aligned_byte_buffer& a, aligned_byte_buffer& b) noexcept
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
    * \note The bytes are not zeroed but stay in the buffer, and setting the size to 0 moves
    * all of them into the reserved-unused tail, where \c operator[] still reads them.
    * \c clear() followed by \c zeroize_reserved_unused() scrubs them.
    */
    constexpr void clear() noexcept { size_ = 0; }

    /// Resize to \a count bytes
    /**
    * Growing sets the new bytes to \a value.  Shrinking leaves the removed ones unchanged.
    * \note \c resize(capacity(), \a value) is how to fill only the reserved-unused tail
    * [\c size(), \c capacity()) and grow into it.  \c fill_capacity() overwrites the live
    * bytes as well.
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
    * truncates mod 256.  Floating-point and other enumeration arguments are rejected, so cast
    * explicitly if that is intended.
    * \note "Emplace" is assignment here.  The slot already holds a live byte, so this is
    * equivalent to \c push_back(std::byte(args...)).
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
    * \note The capacity is kept, not resized to the source.
    * \pre \a spn does not overlap this buffer's storage.
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
    * \return A pointer to the block, aligned to \a Align, or \c nullptr if \c capacity()
    * is 0.
    * \note The null test is not defensive.  \c std::assume_aligned requires a pointer to a
    * real object, so it may not be applied to the empty buffer's null block.
    */
    [[nodiscard]] constexpr std::byte* data() noexcept
    {
        std::byte* const p = data_.get();
        return p != nullptr ? std::assume_aligned<Align>(p) : p;
    }

    /// \copydoc data()
    [[nodiscard]] constexpr const std::byte* data() const noexcept
    {
        const std::byte* const p = data_.get();
        return p != nullptr ? std::assume_aligned<Align>(p) : p;
    }

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
    * an index in [\c size(), \c capacity()) is a valid read that yields an
    * unspecified (not indeterminate) byte.
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
    * rejects an index in [\c size(), \c capacity()) where \c operator[] yields an
    * unspecified byte.
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

    [[nodiscard]] constexpr bool operator==(const aligned_byte_buffer& rhs) const noexcept
    {
        return std::ranges::equal(span(), rhs.span());
    }

    [[nodiscard]] constexpr auto
    operator<=>(const aligned_byte_buffer& rhs) const noexcept
    {
        return std::lexicographical_compare_three_way(begin(), end(), rhs.begin(), rhs.end());
    }
};
