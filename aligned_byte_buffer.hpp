// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

/**
* \file
* \author Steven Ward
* \sa https://github.com/planet36/vectors
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
#include <type_traits>
#include <utility>

/// A resizable, fixed-capacity buffer of \c std::byte with over-alignable storage.
/**
* This is the \c std::byte specialization of \c dynamic_fixed_vector: the same API, but the
* element type is fixed to \c std::byte so the implementation can be simpler and faster.
*
* Differences from \c dynamic_fixed_vector:
*   - There is no element-type template parameter; only the alignment \a Align (a power of
*     two, defaulting to 16).  \c aligned_byte_buffer<16> and \c aligned_byte_buffer<32> are
*     distinct types.
*   - Because \c sizeof(std::byte)==1, the allocation size is exactly the capacity: there is
*     no multiplication and no overflow check.
*   - Reserved-but-unused capacity is left \b uninitialized.  Storage lifetime is begun with
*     \c std::start_lifetime_as_array (no whole-capacity zeroing).  Bytes that enter \c size()
*     are always written; reading beyond \c size() via \c operator[] yields an \e unspecified
*     byte value -- which is well-defined (not UB) for \c std::byte.
*   - The \c emplace_back family accepts at most one argument, of type \c std::byte or an
*     integral type (floating-point and other enumeration arguments are rejected).
*
* Like \c dynamic_fixed_vector: \c data() applies \c std::assume_aligned<Align> so caller
* loops can vectorize, \c zeroize_remaining_space() zeros the reserved tail with non-elidable
* stores (\c clear() followed by it zeroizes the whole buffer), capacity is fixed at
* construction (\c capacity() / \c
* max_size() return it), \c operator[] is unchecked and capacity-based, \c at() is
* bounds-checked, capacity overflow throws \c std::bad_alloc, and the \c try_* family returns
* \c bool.  The interface is annotated \c constexpr, but because over-aligned allocation is
* not usable in constant evaluation, only empty (non-allocating) instances are usable in
* constant expressions.
*
* \invariant \c size() \c <= \c capacity().
* \invariant \c data() is null \b exactly when \c capacity() is 0.  A capacity of 0 allocates
* nothing, and the aligned \c ::operator \c new never returns null (it throws \c std::bad_alloc),
* so no other state holds a null block.
*
* Those two are what make the preconditions below sufficient on their own.  \c !is_full(),
* \c !is_empty() and <code>i < capacity()</code> each imply \c capacity() \c != \c 0, hence a
* non-null, \a Align-aligned block -- which is why the members carrying them index \c data()
* without re-checking it for null.
*
* \sa dynamic_fixed_vector
*/
template <std::size_t Align = 16>
requires (std::has_single_bit(Align))
class aligned_byte_buffer
{
private:
    /// Stateless deleter that frees a block from the aligned \c ::operator \c new.
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

    /// Allocate an over-aligned, \b uninitialized block of \a cap bytes.
    [[nodiscard]] static constexpr storage_ptr allocate_(const std::size_t cap)
    {
        // Not an optimization: ::operator new(0) returns a non-null block, so only this keeps
        // the class invariant's "capacity 0 implies null data()" true.
        if (cap == 0)
            return nullptr;

        // sizeof(std::byte) == 1, so the byte count is exactly cap -- no overflow is possible.
        void* const raw = ::operator new(cap, std::align_val_t{Align});
        // Begin the lifetimes of cap std::byte objects without initializing them.
        std::byte* const p = std::start_lifetime_as_array<std::byte>(raw, cap);
        return storage_ptr{p};
    }

    /// Check the index \a i against \c size()
    constexpr void check_idx_(const std::size_t i) const
    {
        if (i >= size())
            throw std::out_of_range("aligned_byte_buffer: index >= size");
    }

    /// \pre \a spn does not overlap this buffer's storage.
    constexpr void common_append_range_(const std::span<const std::byte> spn)
    {
        if (!spn.empty())
            std::memcpy(data() + size_, spn.data(), spn.size());
        size_ += spn.size();
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

    /// True if \a R is a sized, contiguous range of exactly \c std::byte.
    /**
    * Such a range is handed to the \c std::span overload, which copies with \c std::memcpy,
    * rather than appended byte-wise.  Overload resolution does not do this on its own: for
    * e.g. \c std::vector<std::byte> the \c R&& template is an exact match while the
    * \c std::span overload needs a user-defined conversion, so the template wins and the
    * \c memcpy is never reached unless the caller writes the \c std::span out by hand.
    */
    template <typename R>
    static constexpr bool is_bulk_appendable_ =
        std::ranges::contiguous_range<R> && std::ranges::sized_range<R> &&
        std::same_as<std::ranges::range_value_t<R>, std::byte>;

    /// \pre \a rg satisfies \c is_bulk_appendable_.
    template <typename R>
    [[nodiscard]] static constexpr std::span<const std::byte> as_span_(R& rg)
    {
        return std::span<const std::byte>{std::ranges::data(rg),
                                          static_cast<std::size_t>(std::ranges::size(rg))};
    }

    /// Zero \a n bytes at \a p with stores the compiler must not optimize away.
    /**
    * Uses \c memset_explicit (C23 / C++26) or \c explicit_bzero (glibc, BSDs) when the C
    * library declares one, and otherwise falls back to writes through a \c volatile
    * pointer.  Availability is detected by name lookup on the dependent parameter \a P:
    * neither function has a feature-test macro, and \c __cplusplus is useless here (GCC 16
    * still reports 202400L for \c -std=c++26; the C library, not the language mode,
    * determines availability).
    */
    template <typename P>
    static void zero_explicit_(P const p, const std::size_t n) noexcept
    {
        if constexpr (requires { memset_explicit(p, 0, n); })
            memset_explicit(p, 0, n);
        else if constexpr (requires { explicit_bzero(p, n); })
            explicit_bzero(p, n);
        else
        {
            volatile unsigned char* const q = static_cast<volatile unsigned char*>(p);
            for (std::size_t i = 0; i < n; ++i)
                q[i] = 0;
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

    constexpr aligned_byte_buffer(const aligned_byte_buffer& other)
        : size_{other.size_}, capacity_{other.capacity_}, data_{allocate_(other.capacity_)}
    {
        // The reserved tail is unspecified, so only the live [0,size) bytes are copied.
        if (size_ != 0)
            std::memcpy(data(), other.data(), size_);
    }

    constexpr aligned_byte_buffer(aligned_byte_buffer&& other) noexcept
        : size_{std::exchange(other.size_, 0)},
          capacity_{std::exchange(other.capacity_, 0)},
          data_{std::move(other.data_)}
    {}

    constexpr aligned_byte_buffer& operator=(const aligned_byte_buffer& other)
    {
        aligned_byte_buffer tmp{other};
        swap(tmp);
        return *this;
    }

    /// Swap-based: \a other is left holding this buffer's former contents, not emptied.
    constexpr aligned_byte_buffer& operator=(aligned_byte_buffer&& other) noexcept
    {
        swap(other);
        return *this;
    }

    ~aligned_byte_buffer() = default;

    /// Reserve capacity \a capacity; the buffer starts empty.
    constexpr explicit aligned_byte_buffer(const std::size_t capacity)
        : size_{0}, capacity_{capacity}, data_{allocate_(capacity)}
    {}

    /// Reserve capacity \a capacity and fill it with \a value (\c size()==capacity).
    constexpr explicit aligned_byte_buffer(const std::size_t capacity, const std::byte value)
        : size_{capacity}, capacity_{capacity}, data_{allocate_(capacity)}
    {
        if (capacity != 0)
            std::memset(data(), std::to_integer<int>(value), capacity);
    }

    /// Capacity is the size of \a spn.
    constexpr explicit aligned_byte_buffer(const std::span<const std::byte> spn)
        : aligned_byte_buffer(std::size(spn))
    {
        common_append_range_(spn);
    }

    /// Capacity is the distance between \a first and \a last (forward iterators required).
    template <std::forward_iterator It, std::sentinel_for<It> S>
    constexpr explicit aligned_byte_buffer(It first, S last)
        : aligned_byte_buffer(static_cast<std::size_t>(std::ranges::distance(first, last)))
    {
        for (; first != last; ++first)
            unchecked_emplace_back(*first);
    }

    /// Capacity is \a count.
    template <std::input_iterator It>
    constexpr explicit aligned_byte_buffer(It first, const std::size_t count)
        : aligned_byte_buffer(count)
    {
        common_append_range_(first, count);
    }

    constexpr aligned_byte_buffer(const std::initializer_list<std::byte> il)
        : aligned_byte_buffer(std::span<const std::byte>{std::data(il), std::size(il)})
    {}

    /// Capacity is the size of \a rg (forward range required).
    template <std::ranges::forward_range R>
    constexpr explicit aligned_byte_buffer(std::from_range_t, R&& rg)
        : aligned_byte_buffer(static_cast<std::size_t>(std::ranges::distance(rg)))
    {
        for (auto&& e : std::forward<R>(rg))
            unchecked_emplace_back(std::forward<decltype(e)>(e));
    }

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

    [[nodiscard]] constexpr std::size_t remaining_space() const noexcept
    {
        return capacity_ - size_;
    }

    [[nodiscard]] constexpr bool is_empty() const noexcept { return size_ == 0; }

    [[nodiscard]] constexpr bool is_full() const noexcept { return size_ == capacity_; }

    /// \note Does not zero the bytes: they stay in the buffer, readable through \c operator[]
    /// as the now-reserved tail.  \c clear() followed by \c zeroize_remaining_space() scrubs
    /// them.
    constexpr void clear() noexcept { size_ = 0; }

    /// Resize to \a count bytes
    /**
    * If \a count > \c size(), new bytes are set to \a value.
    * If \a count <= \c size(), removed bytes are unchanged.
    */
    constexpr void resize(const std::size_t count, const std::byte value)
    {
        if (count > capacity_)
            throw std::bad_alloc{};

        if (count > size_)
            std::memset(data() + size_, std::to_integer<int>(value), count - size_);

        size_ = count;
    }

    constexpr void resize(const std::size_t count) { resize(count, std::byte{}); }

    /// \note No-op if empty (unlike \c std::inplace_vector::pop_back, where that is UB).
    constexpr void pop_back() noexcept
    {
        if (is_empty())
            return;

        --size_;
    }

    /// \pre \c !is_full()
    /// \note Accepts no argument (appends \c std::byte{}) or one \c std::byte / integral
    /// argument, converted as by \c static_cast (out-of-range integers truncate mod 256).
    /// Floating-point and other enumeration arguments are rejected; cast explicitly if
    /// intended.
    /// \note "Emplace" is assignment here: the slot already holds a live byte (elements are
    /// never destroyed), so this is equivalent to \c push_back(std::byte(args...)).
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

    /**
    * \retval true if success
    * \retval false if failure
    */
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

    /// \pre \c !is_full()
    constexpr void unchecked_push_back(const std::byte value) noexcept
    {
        unchecked_emplace_back(value);
    }

    constexpr void push_back(const std::byte value) { emplace_back(value); }

    [[nodiscard]] constexpr bool try_push_back(const std::byte value) noexcept
    {
        return try_emplace_back(value);
    }

    /// Fill all \c capacity() bytes with \a value and set \c size() to \c capacity().
    constexpr void fill_capacity(const std::byte value) noexcept
    {
        if (capacity_ != 0)
            std::memset(data(), std::to_integer<int>(value), capacity_);
        size_ = capacity_;
    }

    /// Fill the live bytes [0, \c size()) with \a value; \c size() is unchanged.
    constexpr void fill_size(const std::byte value) noexcept
    {
        if (size_ != 0)
            std::memset(data(), std::to_integer<int>(value), size_);
    }

    /// Zero the reserved tail [\c size(), \c capacity()); \c size() is unchanged.
    /**
    * Replaces the unspecified reserved bytes with zeros -- e.g. to pad to an alignment
    * boundary before reading whole SIMD lanes past \c size(), or to keep stale heap bytes
    * from leaking through beyond-size reads.  The stores are guaranteed to happen even if
    * nothing reads the tail afterward (\c memset_explicit / \c explicit_bzero / volatile
    * fallback), so \c clear() followed by this zeroizes the whole buffer (e.g. scrubbing
    * sensitive contents before destruction or reuse, where a plain \c memset is a dead
    * store the optimizer may elide).
    */
    constexpr void zeroize_remaining_space() noexcept
    {
        if (remaining_space() != 0)
            zero_explicit_(static_cast<void*>(data() + size_), remaining_space());
    }

    /// \pre \a spn does not overlap this buffer's storage.
    constexpr void append_range(const std::span<const std::byte> spn)
    {
        if (std::size(spn) > remaining_space())
            throw std::bad_alloc{};

        common_append_range_(spn);
    }

    /// \note If the source size is computable up front (\c std::sized_sentinel_for), it is
    /// checked before writing (all-or-nothing).  Otherwise appends byte-wise: the bytes
    /// that fit are appended before \c std::bad_alloc is thrown.
    /// \pre <code>[first, last)</code> is a valid range (\a last is reachable from \a first).
    /// For a \c std::sized_sentinel_for this guarantees <code>last - first</code> is
    /// non-negative, so the up-front size check's cast to \c std::size_t is well-defined;
    /// a caller passing \a first past \a last is undefined regardless (the loop below
    /// would never terminate).
    template <std::input_iterator It, std::sentinel_for<It> S>
    constexpr void append_range(It first, S last)
    {
        if constexpr (std::sized_sentinel_for<S, It>)
        {
            if (static_cast<std::size_t>(last - first) > remaining_space())
                throw std::bad_alloc{};
        }

        for (; first != last; ++first)
            emplace_back(*first);
    }

    template <std::input_iterator It>
    constexpr void append_range(It first, const std::size_t count)
    {
        if (count > remaining_space())
            throw std::bad_alloc{};

        common_append_range_(first, count);
    }

    constexpr void append_range(const std::initializer_list<std::byte> il)
    {
        append_range(std::span<const std::byte>{std::data(il), std::size(il)});
    }

    /// \note Sized sources are checked up front (all-or-nothing); unsized sources append
    /// element-wise and may partially append before throwing \c std::bad_alloc.
    /// \pre If \a rg is a contiguous range of \c std::byte, it does not overlap this buffer's
    /// storage: that case is forwarded to the \c std::span overload, which carries the same tag.
    template <std::ranges::input_range R>
    constexpr void append_range(R&& rg)
    {
        if constexpr (is_bulk_appendable_<R>)
        {
            append_range(as_span_(rg));
        }
        else if constexpr (std::ranges::sized_range<R>)
        {
            if (std::ranges::size(rg) > remaining_space())
                throw std::bad_alloc{};

            // The check above covers every element, so emplace_back's per-element repeat of
            // it would be redundant.
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
    * \retval false if failure
    * \retval true if success
    */
    [[nodiscard]] constexpr bool try_append_range(const std::span<const std::byte> spn)
    {
        if (std::size(spn) > remaining_space())
            return false;

        common_append_range_(spn);
        return true;
    }

    /// \note If the source size is computable up front (\c std::sized_sentinel_for), it is
    /// checked before writing (nothing appended on \c false).  Otherwise appends
    /// byte-wise: on \c false, the bytes that fit have already been appended
    /// (observe \c size()).
    /// \pre <code>[first, last)</code> is a valid range (\a last is reachable from \a first).
    /// For a \c std::sized_sentinel_for this guarantees <code>last - first</code> is
    /// non-negative, so the up-front size check's cast to \c std::size_t is well-defined;
    /// a caller passing \a first past \a last is undefined regardless (the loop below
    /// would never terminate).
    template <std::input_iterator It, std::sentinel_for<It> S>
    [[nodiscard]] constexpr bool try_append_range(It first, S last)
    {
        if constexpr (std::sized_sentinel_for<S, It>)
        {
            if (static_cast<std::size_t>(last - first) > remaining_space())
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
        if (count > remaining_space())
            return false;

        common_append_range_(first, count);
        return true;
    }

    [[nodiscard]] constexpr bool try_append_range(const std::initializer_list<std::byte> il)
    {
        return try_append_range(std::span<const std::byte>{std::data(il), std::size(il)});
    }

    /// \note Sized sources are checked up front (nothing appended on \c false); unsized
    /// sources append element-wise, so on \c false the bytes that fit have already been
    /// appended (observe \c size()).
    /// \pre If \a rg is a contiguous range of \c std::byte, it does not overlap this buffer's
    /// storage: that case is forwarded to the \c std::span overload, which carries the same tag.
    template <std::ranges::input_range R>
    [[nodiscard]] constexpr bool try_append_range(R&& rg)
    {
        if constexpr (is_bulk_appendable_<R>)
        {
            return try_append_range(as_span_(rg));
        }
        else if constexpr (std::ranges::sized_range<R>)
        {
            if (std::ranges::size(rg) > remaining_space())
                return false;

            // The check above covers every element, so try_emplace_back's per-element repeat
            // of it would be redundant.
            for (auto&& e : std::forward<R>(rg))
                unchecked_emplace_back(std::forward<decltype(e)>(e));

            return true;
        }
        else
        {
            for (auto&& e : std::forward<R>(rg))
            {
                if (!try_emplace_back(std::forward<decltype(e)>(e)))
                    return false;
            }

            return true;
        }
    }

    /// Throws if the source exceeds \c capacity().
    /// \pre \a spn does not overlap this buffer's storage.
    constexpr void assign_range(const std::span<const std::byte> spn)
    {
        clear();
        append_range(spn);
    }

    template <std::input_iterator It, std::sentinel_for<It> S>
    constexpr void assign_range(It first, S last)
    {
        clear();
        append_range(first, last);
    }

    template <std::input_iterator It>
    constexpr void assign_range(It first, const std::size_t count)
    {
        clear();
        append_range(first, count);
    }

    constexpr void assign_range(const std::initializer_list<std::byte> il)
    {
        clear();
        append_range(il);
    }

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

    /// \returns A pointer to the block, aligned to \a Align, or \c nullptr if \c capacity()
    /// is 0 (per the class invariant, that is the only case).
    /**
    * \note The null test is not defensive: \c std::assume_aligned requires a pointer to a real
    * object, so it may not be applied to the empty buffer's null block.  Members whose
    * precondition already rules \c capacity() \c == \c 0 out do not repeat the test.
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

    /// \pre \c !is_empty()
    [[nodiscard]] constexpr std::byte& front() noexcept
    {
#if defined(DEBUG)
        assert(!is_empty());
#endif
        return *begin();
    }

    [[nodiscard]] constexpr const std::byte& front() const noexcept
    {
#if defined(DEBUG)
        assert(!is_empty());
#endif
        return *begin();
    }

    /// \pre \c !is_empty()
    [[nodiscard]] constexpr std::byte& back() noexcept
    {
#if defined(DEBUG)
        assert(!is_empty());
#endif
        return *rbegin();
    }

    [[nodiscard]] constexpr const std::byte& back() const noexcept
    {
#if defined(DEBUG)
        assert(!is_empty());
#endif
        return *rbegin();
    }

    /// \pre \a i < \c capacity()
    /// \note Does not check bounds.  Reading an index in [size(), capacity()) is valid but
    /// yields an unspecified (not indeterminate) byte value; \c at() is the bounds-checked
    /// accessor.
    [[nodiscard]] constexpr std::byte& operator[](const std::size_t i) noexcept
    {
#if defined(DEBUG)
        assert(i < capacity());
#endif
        return data()[i];
    }

    [[nodiscard]] constexpr const std::byte& operator[](const std::size_t i) const noexcept
    {
#if defined(DEBUG)
        assert(i < capacity());
#endif
        return data()[i];
    }

    /// \returns A reference to the byte at index \a i.
    /// \note The only bounds-checked accessor.  It is checked against \c size(), not
    /// \c capacity(): \c operator[] reads an index in [size(), capacity()) and yields an
    /// unspecified byte, but this rejects that index.
    /// \throws std::out_of_range if \a i >= \c size().
    [[nodiscard]] constexpr std::byte& at(const std::size_t i)
    {
        check_idx_(i);
        return data()[i];
    }

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

    [[nodiscard]] constexpr std::strong_ordering
    operator<=>(const aligned_byte_buffer& rhs) const noexcept
    {
        return std::lexicographical_compare_three_way(begin(), end(), rhs.begin(), rhs.end());
    }
};

/// Constant-time equality comparison of two byte spans.
/**
* Runs in time dependent only on the spans' sizes, never on their contents: every byte pair
* is examined and the XORed differences are OR-accumulated, with no data-dependent branch or
* early exit.  Use this instead of \c operator== / \c std::memcmp when comparing
* secret-dependent data (e.g. verifying a MAC / authentication tag), where a first-mismatch
* early exit leaks the position of the first differing byte through timing.
*
* \note Spans of unequal size compare unequal immediately; only the sizes (normally public,
* e.g. a fixed tag length) influence the timing, not the contents.
* \note \c aligned_byte_buffer itself does \b not use this: container \c operator== is
* deliberately variable-time, per ordinary container semantics.
* \note Branch-freedom is a property of this source, not one the language guarantees: nothing
* forbids a compiler from proving \c diff can only accumulate and exiting the loop early.  The
* asymmetry with \c zeroize_remaining_space() is deliberate but worth knowing -- that one
* defeats the optimizer outright (\c memset_explicit and friends), whereas this one relies on
* it declining a transformation it is permitted to make.  Verified for GCC 16 at \c -O3
* \c -march=native: the loop vectorizes to a \c vpxor / \c vpor accumulation with a horizontal
* reduce, and every conditional branch that survives tests a size, not a content byte.  Re-check
* if the compiler or the flags change; should one ever short-circuit here, the fix is a barrier
* on \c diff (an empty \c asm volatile reading it, or a volatile accumulator), not a rewrite.
*/
[[nodiscard]] constexpr bool
constant_time_equal(const std::span<const std::byte> a,
                    const std::span<const std::byte> b) noexcept
{
    if (std::size(a) != std::size(b))
        return false;

    std::byte diff{};
    for (std::size_t i = 0; i < std::size(a); ++i)
        diff |= a[i] ^ b[i];

    return diff == std::byte{0};
}
