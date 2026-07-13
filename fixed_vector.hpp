// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

/**
* \file
* \author Steven Ward
*
* Defines the class \c fixed_vector, a fixed-capacity vector with in-place storage.
*/

#pragma once

#include <algorithm>
#include <array>
#include <bit>
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
#include <type_traits>
#include <utility>

/// A resizable array container with fixed capacity.
/**
* This is similar to \c std::inplace_vector and \c boost::container::static_vector,
* except for these important differences:
*   - The data is stored in a `std::array<T, N>`.
*   - An alignment for the data may be given.
*   - \a N elements are value-initialized upon instantiation.
*   - \c clear(), \c pop_back(), and \c resize() only change the size -- they do not destroy
*     any removed elements.
*   - Array elements are never explicitly destroyed.
*   - \c operator[] is unchecked and \b capacity-based: an index in [\c size(), \c capacity())
*     legitimately reads a live element.  \c at() is the only bounds-checked accessor.
*
* Like \c std::inplace_vector, capacity overflow throws \c std::bad_alloc and the \c try_* /
* \c unchecked_* families are provided -- though the \c try_* members return \c bool here
* rather than \c std::inplace_vector's pointer/iterator.
*
* \warning This container is only suitable for trivially destructible types.
*
* \sa https://cppreference.com/w/cpp/container/inplace_vector.html
* \sa https://www.boost.org/doc/libs/latest/doc/html/doxygen/boost_container_header_reference/classboost_1_1container_1_1static__vector.html
*/
template <typename T,
          std::size_t N,
          std::size_t Align = std::max(alignof(std::size_t), alignof(T))>
requires (N > 0) && std::default_initializable<T> && std::movable<T> &&
         std::is_trivially_destructible_v<T> && (std::has_single_bit(Align))
class fixed_vector
{
private:
    std::size_t size_{};
    alignas(Align) std::array<T, N> data_{};

    /// Check the index \a i against \c size()
    constexpr void check_idx_(const std::size_t i) const
    {
        if (i >= size())
            throw std::out_of_range("fixed_vector: index >= size");
    }

    /// \pre \a spn does not overlap this vector's storage.
    constexpr void common_append_range_(const std::span<const T> spn)
    {
        (void)std::ranges::copy(spn, end());
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
    using value_type = T;
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

    constexpr fixed_vector() noexcept = default;
    /// \note Copy and move are member-wise (defaulted).  Moving does \b not empty the
    /// source: for trivially copyable \c T a moved-from fixed_vector is left unchanged --
    /// unlike the heap-backed siblings, where move construction empties the source.
    fixed_vector(const fixed_vector&) noexcept(std::is_nothrow_copy_constructible_v<T>) = default;
    fixed_vector(fixed_vector&&) noexcept(std::is_nothrow_move_constructible_v<T>) = default;
    fixed_vector& operator=(const fixed_vector&) noexcept(std::is_nothrow_copy_assignable_v<T>) = default;
    fixed_vector& operator=(fixed_vector&&) noexcept(std::is_nothrow_move_assignable_v<T>) = default;
    ~fixed_vector() = default;

    constexpr explicit fixed_vector(const std::size_t count, const T& value)
    {
        resize(count, value);
    }

    constexpr explicit fixed_vector(const std::size_t count)
    {
        if (count > max_size())
            throw std::bad_alloc{};

        // Elements of \c data_ are already value-initialized.
        size_ = count;
    }

    constexpr explicit fixed_vector(const std::span<const T> spn) { append_range(spn); }

    template <std::input_iterator It, std::sentinel_for<It> S>
    constexpr explicit fixed_vector(It first, S last)
    {
        append_range(first, last);
    }

    template <std::input_iterator It>
    constexpr explicit fixed_vector(It first, const std::size_t count)
    {
        append_range(first, count);
    }

    constexpr fixed_vector(const std::initializer_list<T> il) { append_range(il); }

    template <std::ranges::input_range R>
    constexpr explicit fixed_vector(std::from_range_t, R&& rg)
    {
        append_range(std::forward<R>(rg));
    }

    constexpr fixed_vector& operator=(const std::initializer_list<T> il)
    {
        assign_range(il);
        return *this;
    }

    /// Swap the sizes and all \c max_size() array slots (not just the live elements).
    constexpr void swap(fixed_vector& other) noexcept(std::is_nothrow_swappable_v<T>)
    {
        std::swap(size_, other.size_);
        std::swap(data_, other.data_);
    }

    friend constexpr void swap(fixed_vector& a, fixed_vector& b)
        noexcept(std::is_nothrow_swappable_v<T>)
    {
        a.swap(b);
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }

    [[nodiscard]] static constexpr std::size_t max_size() noexcept { return N; }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }

    [[nodiscard]] constexpr std::size_t remaining_space() const noexcept
    {
        return max_size() - size();
    }

    [[nodiscard]] constexpr bool is_empty() const noexcept { return size() == 0; }

    [[nodiscard]] constexpr bool is_full() const noexcept { return size() == max_size(); }

    /**
    * \note Does not destroy elements.
    * \sa https://cppreference.com/w/cpp/container/inplace_vector/clear.html
    */
    constexpr void clear() noexcept { size_ = 0; }

    /// Resize to \a count elements
    /**
    * If \a count > \c size(), new elements are assigned \a value.
    * If \a count <= \c size(), removed elements are unchanged.
    * \note Does not destroy elements.
    * \sa https://cppreference.com/w/cpp/container/inplace_vector/resize.html
    */
    constexpr void resize(const std::size_t count, const T& value)
    {
        if (count > max_size())
            throw std::bad_alloc{};

        if (count > size())
            (void)std::ranges::fill(data() + size(), data() + count, value);

        size_ = count;
    }

    constexpr void resize(const std::size_t count) { resize(count, T{}); }

    /**
    * \note Does not destroy elements.
    * \note No-op if empty (unlike \c std::inplace_vector::pop_back, where that is UB).
    * \sa https://cppreference.com/w/cpp/container/inplace_vector/pop_back.html
    */
    constexpr void pop_back() noexcept
    {
        if (is_empty())
            return;

        --size_;
    }

    /**
    * \pre \c !is_full()
    * \note "Emplace" cannot construct in place here: the slot already holds a live element
    * (elements are never destroyed), so a temporary \c T is constructed from \a args and
    * move-assigned into the slot -- equivalent to \c push_back(T(args...)).  Kept for API
    * parity with \c std::inplace_vector.
    * \sa https://cppreference.com/w/cpp/container/inplace_vector/unchecked_emplace_back.html
    */
    template <class... Args>
    requires std::constructible_from<T, Args...> && std::assignable_from<T&, T>
    constexpr void unchecked_emplace_back(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...> &&
                 std::is_nothrow_assignable_v<T&, T>)
    {
        *end() = T(std::forward<Args>(args)...);
        ++size_;
    }

    /**
    * \sa https://cppreference.com/w/cpp/container/inplace_vector/emplace_back.html
    */
    template <class... Args>
    requires std::constructible_from<T, Args...> && std::assignable_from<T&, T>
    constexpr void emplace_back(Args&&... args)
    {
        if (is_full())
            throw std::bad_alloc{};

        unchecked_emplace_back(std::forward<Args>(args)...);
    }

    /**
    * \retval true if success
    * \retval false if failure
    * \sa https://cppreference.com/w/cpp/container/inplace_vector/try_emplace_back.html
    */
    template <class... Args>
    requires std::constructible_from<T, Args...> && std::assignable_from<T&, T>
    [[nodiscard]] constexpr bool try_emplace_back(Args&&... args)
    {
        if (is_full())
            return false;

        unchecked_emplace_back(std::forward<Args>(args)...);
        return true;
    }

    /**
    * \pre \c !is_full()
    * \sa https://cppreference.com/w/cpp/container/inplace_vector/unchecked_push_back.html
    */
    constexpr void unchecked_push_back(const T& value)
        noexcept(noexcept(unchecked_emplace_back(value)))
    {
        unchecked_emplace_back(value);
    }

    constexpr void unchecked_push_back(T&& value)
        noexcept(noexcept(unchecked_emplace_back(std::move(value))))
    {
        unchecked_emplace_back(std::move(value));
    }

    /**
    * \sa https://cppreference.com/w/cpp/container/inplace_vector/push_back.html
    */
    constexpr void push_back(const T& value) { emplace_back(value); }

    constexpr void push_back(T&& value) { emplace_back(std::move(value)); }

    /**
    * \sa https://cppreference.com/w/cpp/container/inplace_vector/try_push_back.html
    */
    [[nodiscard]] constexpr bool try_push_back(const T& value)
    {
        return try_emplace_back(value);
    }

    [[nodiscard]] constexpr bool try_push_back(T&& value)
    {
        return try_emplace_back(std::move(value));
    }

    /**
    * Fill all \c max_size() elements with \a value and set \c size() to \c max_size().
    * \sa https://cppreference.com/w/cpp/container/array/fill.html
    */
    constexpr void fill_capacity(const T& value)
    {
        data_.fill(value);
        size_ = max_size();
    }

    /**
    * Fill the live elements [0, \c size()) with \a value; \c size() is unchanged.
    * \sa https://cppreference.com/w/cpp/algorithm/ranges/fill
    */
    constexpr void fill_size(const T& value) { (void)std::ranges::fill(span(), value); }

    /// Zeroize the reserved tail elements [\c size(), \c max_size()); \c size() is unchanged.
    /**
    * Each tail element stays alive; its object representation is set to all-zero bytes
    * (for scalar \c T this equals the value-initialized value, as after construction).
    * At run time the stores are guaranteed to happen even if nothing reads the tail
    * afterward (\c memset_explicit / \c explicit_bzero / volatile fallback), so \c clear()
    * followed by this zeroizes the whole array (e.g. scrubbing sensitive contents, where a
    * plain fill is a dead store the optimizer may elide).  During constant evaluation --
    * where there is no memory to scrub -- the tail is value-assigned instead.
    */
    constexpr void zeroize_remaining_space() noexcept
    requires std::is_trivially_copyable_v<T>
    {
        if consteval
        {
            for (std::size_t i = size(); i < max_size(); ++i)
                data_[i] = T{};
        }
        else
        {
            if (remaining_space() != 0)
                zero_explicit_(static_cast<void*>(data() + size()), remaining_space() * sizeof(T));
        }
    }

    /**
    * \pre \a spn does not overlap this vector's storage.
    * \sa https://cppreference.com/w/cpp/container/inplace_vector/append_range.html
    */
    constexpr void append_range(const std::span<const T> spn)
    {
        if (std::size(spn) > remaining_space())
            throw std::bad_alloc{};

        common_append_range_(spn);
    }

    /**
    * \pre <code>[first, last)</code> is a valid range (\a last is reachable from \a first).
    * For a \c std::sized_sentinel_for this guarantees <code>last - first</code> is
    * non-negative, so the up-front size check's cast to \c std::size_t is well-defined;
    * a caller passing \a first past \a last is undefined regardless (the loop below
    * would never terminate).
    * \note If the source size is computable up front (\c std::sized_sentinel_for), it is
    * checked before writing (all-or-nothing).  Otherwise appends element-wise: the
    * elements that fit are appended before \c std::bad_alloc is thrown.
    */
    template <std::input_iterator It, std::sentinel_for<It> S>
    constexpr void append_range(It first, S last)
    {
        if constexpr (std::sized_sentinel_for<S, It>)
        {
            if (static_cast<std::size_t>(last - first) > remaining_space())
                throw std::bad_alloc{};
        }

        for (; first != last; ++first)
        {
            emplace_back(*first);
        }
    }

    template <std::input_iterator It>
    constexpr void append_range(It first, const std::size_t count)
    {
        if (count > remaining_space())
            throw std::bad_alloc{};

        common_append_range_(first, count);
    }

    constexpr void append_range(const std::initializer_list<T> il)
    {
        append_range(std::span<const T>{std::data(il), std::size(il)});
    }

    /**
    * \note Sized sources are checked up front (all-or-nothing); unsized sources append
    * element-wise and may partially append before throwing \c std::bad_alloc.
    */
    template <std::ranges::input_range R>
    constexpr void append_range(R&& rg)
    {
        if constexpr (std::ranges::sized_range<R>)
        {
            if (std::ranges::size(rg) > remaining_space())
                throw std::bad_alloc{};
        }

        for (auto&& e : std::forward<R>(rg))
        {
            emplace_back(std::forward<decltype(e)>(e));
        }
    }

    /**
    * \pre \a spn does not overlap this vector's storage.
    * \retval false if failure
    * \retval true if success
    * \sa https://cppreference.com/w/cpp/container/inplace_vector/try_append_range.html
    */
    [[nodiscard]] constexpr bool try_append_range(const std::span<const T> spn)
    {
        if (std::size(spn) > remaining_space())
            return false;

        common_append_range_(spn);
        return true;
    }

    /**
    * \pre <code>[first, last)</code> is a valid range (\a last is reachable from \a first).
    * For a \c std::sized_sentinel_for this guarantees <code>last - first</code> is
    * non-negative, so the up-front size check's cast to \c std::size_t is well-defined;
    * a caller passing \a first past \a last is undefined regardless (the loop below
    * would never terminate).
    * \note If the source size is computable up front (\c std::sized_sentinel_for), it is
    * checked before writing (nothing appended on \c false).  Otherwise appends
    * element-wise: on \c false, the elements that fit have already been appended
    * (observe \c size()).
    */
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

    [[nodiscard]] constexpr bool try_append_range(const std::initializer_list<T> il)
    {
        return try_append_range(std::span<const T>{std::data(il), std::size(il)});
    }

    /**
    * \note Sized sources are checked up front (nothing appended on \c false); unsized
    * sources append element-wise, so on \c false the elements that fit have already been
    * appended (observe \c size()).
    */
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

    /**
    * \note Does not destroy elements.
    * \pre \a spn does not overlap this vector's storage.
    * \sa https://cppreference.com/w/cpp/container/inplace_vector/assign_range.html
    */
    constexpr void assign_range(const std::span<const T> spn)
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

    constexpr void assign_range(const std::initializer_list<T> il)
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

    [[nodiscard]] constexpr std::span<T> span() noexcept { return {data(), size()}; }

    [[nodiscard]] constexpr std::span<const T> span() const noexcept
    {
        return {data(), size()};
    }

    [[nodiscard]] constexpr explicit operator std::span<T>() noexcept { return span(); }

    [[nodiscard]] constexpr explicit operator std::span<const T>() const noexcept
    {
        return span();
    }

    /// \note Unlike the heap-backed siblings, no \c std::assume_aligned<Align> is applied:
    /// the array is a member of an \c alignas(Align) object, so the compiler derives the
    /// pointer's alignment statically.
    [[nodiscard]] constexpr T* data() noexcept { return std::data(data_); }

    [[nodiscard]] constexpr const T* data() const noexcept { return std::data(data_); }

    /**
    * \pre \c !is_empty()
    */
    [[nodiscard]] constexpr T& front() noexcept { return *begin(); }

    [[nodiscard]] constexpr const T& front() const noexcept { return *begin(); }

    /**
    * \pre \c !is_empty()
    */
    [[nodiscard]] constexpr T& back() noexcept { return *rbegin(); }

    [[nodiscard]] constexpr const T& back() const noexcept { return *rbegin(); }

    /**
    * \pre \a i < \c capacity()
    * \note Does not check bounds.  Indexes in [size(), capacity()) are valid reads (every
    * capacity slot holds a live element); \c at() is the bounds-checked accessor.
    */
    [[nodiscard]] constexpr T& operator[](const std::size_t i) noexcept { return data_[i]; }

    [[nodiscard]] constexpr const T& operator[](const std::size_t i) const noexcept
    {
        return data_[i];
    }

    [[nodiscard]] constexpr T& at(const std::size_t i)
    {
        check_idx_(i);
        return data_[i];
    }

    [[nodiscard]] constexpr const T& at(const std::size_t i) const
    {
        check_idx_(i);
        return data_[i];
    }

    [[nodiscard]] constexpr T* begin() noexcept { return data(); }

    [[nodiscard]] constexpr const T* begin() const noexcept { return data(); }

    [[nodiscard]] constexpr const T* cbegin() const noexcept { return data(); }

    [[nodiscard]] constexpr T* end() noexcept { return data() + size(); }

    [[nodiscard]] constexpr const T* end() const noexcept { return data() + size(); }

    [[nodiscard]] constexpr const T* cend() const noexcept { return data() + size(); }

    [[nodiscard]] constexpr std::reverse_iterator<T*> rbegin() noexcept
    {
        return std::reverse_iterator(end());
    }

    [[nodiscard]] constexpr std::reverse_iterator<const T*> rbegin() const noexcept
    {
        return std::reverse_iterator(end());
    }

    [[nodiscard]] constexpr std::reverse_iterator<const T*> crbegin() const noexcept
    {
        return std::reverse_iterator(cend());
    }

    [[nodiscard]] constexpr std::reverse_iterator<T*> rend() noexcept
    {
        return std::reverse_iterator(begin());
    }

    [[nodiscard]] constexpr std::reverse_iterator<const T*> rend() const noexcept
    {
        return std::reverse_iterator(begin());
    }

    [[nodiscard]] constexpr std::reverse_iterator<const T*> crend() const noexcept
    {
        return std::reverse_iterator(cbegin());
    }

    [[nodiscard]] constexpr bool operator==(const fixed_vector& rhs) const
    requires std::equality_comparable<T>
    {
        return std::ranges::equal(span(), rhs.span());
    }

    [[nodiscard]] constexpr auto operator<=>(const fixed_vector& rhs) const
    requires std::three_way_comparable<T>
    {
        return std::lexicographical_compare_three_way(begin(), end(), rhs.begin(), rhs.end());
    }
};
