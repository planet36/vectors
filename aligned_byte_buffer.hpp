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
*   - \c data() applies \c std::assume_aligned<Align> so caller loops can vectorize on the
*     known alignment.
*
* Like \c dynamic_fixed_vector: capacity is fixed at construction (\c capacity() / \c
* max_size() return it), \c operator[] is unchecked and capacity-based, \c at() is
* bounds-checked, capacity overflow throws \c std::bad_alloc, and the \c try_* family returns
* \c bool.  The interface is annotated \c constexpr, but because over-aligned allocation is
* not usable in constant evaluation, only empty (non-allocating) instances are usable in
* constant expressions.
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

public:
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
        : aligned_byte_buffer(std::data(il), std::size(il))
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
        using std::swap;
        swap(size_, other.size_);
        swap(capacity_, other.capacity_);
        swap(data_, other.data_);
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

    constexpr void pop_back() noexcept
    {
        if (is_empty())
            return;

        --size_;
    }

    /// \pre \c !is_full()
    template <class... Args>
    requires requires(Args&&... args) { std::byte(std::forward<Args>(args)...); }
    constexpr void unchecked_emplace_back(Args&&... args)
    {
        *end() = std::byte(std::forward<Args>(args)...);
        ++size_;
    }

    template <class... Args>
    requires requires(Args&&... args) { std::byte(std::forward<Args>(args)...); }
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
    requires requires(Args&&... args) { std::byte(std::forward<Args>(args)...); }
    [[nodiscard]] constexpr bool try_emplace_back(Args&&... args)
    {
        if (is_full())
            return false;

        unchecked_emplace_back(std::forward<Args>(args)...);
        return true;
    }

    /// \pre \c !is_full()
    constexpr void unchecked_push_back(const std::byte value) { unchecked_emplace_back(value); }

    constexpr void push_back(const std::byte value) { emplace_back(value); }

    [[nodiscard]] constexpr bool try_push_back(const std::byte value)
    {
        return try_emplace_back(value);
    }

    constexpr void fill_capacity(const std::byte value)
    {
        if (capacity_ != 0)
            std::memset(data(), std::to_integer<int>(value), capacity_);
        size_ = capacity_;
    }

    constexpr void fill_size(const std::byte value)
    {
        if (size_ != 0)
            std::memset(data(), std::to_integer<int>(value), size_);
    }

    constexpr void append_range(const std::span<const std::byte> spn)
    {
        if (std::size(spn) > remaining_space())
            throw std::bad_alloc{};

        common_append_range_(spn);
    }

    template <std::input_iterator It, std::sentinel_for<It> S>
    constexpr void append_range(It first, S last)
    {
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

    template <std::ranges::input_range R>
    constexpr void append_range(R&& rg)
    {
        if constexpr (std::ranges::sized_range<R>)
        {
            if (std::ranges::size(rg) > remaining_space())
                throw std::bad_alloc{};
        }

        for (auto&& e : std::forward<R>(rg))
            emplace_back(std::forward<decltype(e)>(e));
    }

    /**
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

    template <std::input_iterator It, std::sentinel_for<It> S>
    [[nodiscard]] constexpr bool try_append_range(It first, S last)
    {
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

    template <std::ranges::input_range R>
    [[nodiscard]] constexpr bool try_append_range(R&& rg)
    {
        if constexpr (std::ranges::sized_range<R>)
        {
            if (std::ranges::size(rg) > remaining_space())
                return false;
        }

        for (auto&& e : std::forward<R>(rg))
        {
            if (!try_emplace_back(std::forward<decltype(e)>(e)))
                return false;
        }

        return true;
    }

    /// Throws if the source exceeds \c capacity().
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

    [[nodiscard]] constexpr std::byte* data() noexcept
    {
        std::byte* const p = data_.get();
        return p != nullptr ? std::assume_aligned<Align>(p) : p;
    }

    [[nodiscard]] constexpr const std::byte* data() const noexcept
    {
        const std::byte* const p = data_.get();
        return p != nullptr ? std::assume_aligned<Align>(p) : p;
    }

    /// \pre \c !is_empty()
    [[nodiscard]] constexpr std::byte& front() noexcept { return *begin(); }

    [[nodiscard]] constexpr const std::byte& front() const noexcept { return *begin(); }

    /// \pre \c !is_empty()
    [[nodiscard]] constexpr std::byte& back() noexcept { return *rbegin(); }

    [[nodiscard]] constexpr const std::byte& back() const noexcept { return *rbegin(); }

    /// \pre \a i < \c capacity()
    /// \note Does not check bounds.  Reading an index in [size(), capacity()) is valid but
    /// yields an unspecified (not indeterminate) byte value; \c at() is the bounds-checked
    /// accessor.
    [[nodiscard]] constexpr std::byte& operator[](const std::size_t i) noexcept
    {
        return data()[i];
    }

    [[nodiscard]] constexpr const std::byte& operator[](const std::size_t i) const noexcept
    {
        return data()[i];
    }

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

    [[nodiscard]] constexpr bool operator==(const aligned_byte_buffer& rhs) const
    {
        return std::ranges::equal(span(), rhs.span());
    }

    [[nodiscard]] constexpr std::strong_ordering operator<=>(const aligned_byte_buffer& rhs) const
    {
        return std::lexicographical_compare_three_way(begin(), end(), rhs.begin(), rhs.end());
    }
};
