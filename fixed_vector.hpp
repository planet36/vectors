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
#if defined(DEBUG)
#include <cassert>
#endif
#include <compare>
#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string.h> // memset_explicit, explicit_bzero
#include <type_traits>
#include <utility>

/// A resizable array container with in-place storage and a compile-time capacity bound
/**
* \a N bounds the capacity but does not fix it: the capacity is a run-time value that
* \c reserve() moves after construction, in either direction.
*
* This is similar to \c std::inplace_vector and \c boost::container::static_vector,
* except for these important differences:
*   - The data is stored in a `std::array<T, N>`.
*   - An alignment for the data may be given.
*   - \a N elements are value-initialized upon instantiation.
*   - \c clear(), \c pop_back(), and \c resize() only change the size.  They do not destroy
*     any removed elements.
*   - Array elements are never explicitly destroyed.
*   - \c operator[] is unchecked and \b capacity-based.  An index in [\c size(), \c capacity())
*     legitimately reads a live element.  \c at() is the only bounds-checked accessor.
*   - \c capacity() is a \b run-time value in [0, \a N], not the template parameter.  It starts
*     at \a N and is moved by \c reserve(), which shrinks as well as grows.  \a N is reported by
*     \c max_size().  Capacity bounds every space check, but the storage is always the whole
*     \c std::array<T, N>, so no slot is ever (de)allocated, constructed, or destroyed.
*
* Like \c std::inplace_vector, capacity overflow throws \c std::bad_alloc and the \c try_* /
* \c unchecked_* families are provided.  The \c try_* members return \c bool here rather than
* \c std::inplace_vector's pointer/iterator.
*
* \note <code>Align >= alignof(T)</code> is a diagnostic, not a correctness requirement, since
* \c alignas cannot weaken a type's natural alignment.  A weakened \c alignas is ill-formed
* ([dcl.align]/5) yet GCC accepts it silently, so without the constraint an under-alignment
* request would be quietly ignored.
*
* \note The default \a Align keeps the storage word-aligned even for a narrow \a T.
*
* \invariant \c size() \c <= \c capacity() \c <= \c max_size(), which is \a N.
* \invariant \c data() is never null, since \a N > 0 and the storage is an in-place member.
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
         std::is_trivially_destructible_v<T> &&
         (std::has_single_bit(Align)) && (Align >= alignof(T))
class fixed_vector
{
private:
    std::size_t size_{};
    std::size_t capacity_{N};
    alignas(Align) std::array<T, N> data_{};

    constexpr void check_idx_(const std::size_t i) const
    {
        if (i >= size())
            throw std::out_of_range("fixed_vector: index >= size");
    }

    /**
    * \pre \a spn does not overlap this vector's storage.
    */
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

    /// True if \a R is a sized, contiguous range of \c T
    /**
    * Such a range is handed to the \c std::span overload for its bulk copy.  Overload
    * resolution will not do that on its own, since the \c R&& template is an exact match for a
    * \c std::vector<T> where the \c std::span overload needs a user-defined conversion.
    * Without this test, only a hand-written span would ever reach the bulk copy.
    */
    template <typename R>
    static constexpr bool is_bulk_appendable_ =
        std::ranges::contiguous_range<R> && std::ranges::sized_range<R> &&
        std::same_as<std::ranges::range_value_t<R>, T>;

    /// View \a rg as the \c std::span of \c const \c T that the bulk-copy overload takes
    template <typename R>
    requires is_bulk_appendable_<R>
    [[nodiscard]] static constexpr std::span<const T> as_span_(R& rg)
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
        if constexpr (requires { memset_explicit(p, 0, n); })
        {
            memset_explicit(p, 0, n);
        }
        else if constexpr (requires { explicit_bzero(p, n); })
        {
            explicit_bzero(p, n);
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

    /**
    * \note This value-initializes all \a N array elements, and \c std::default_initializable<T>
    * does not require that to be non-throwing.
    */
    constexpr fixed_vector() noexcept(std::is_nothrow_default_constructible_v<T>) = default;

    /**
    * \note A move does \b not empty the source.  The elements are moved one by one, and a
    * trivially copyable \c T leaves the source unchanged.
    */
    fixed_vector(const fixed_vector&) noexcept(std::is_nothrow_copy_constructible_v<T>) = default;
    fixed_vector(fixed_vector&&) noexcept(std::is_nothrow_move_constructible_v<T>) = default;
    fixed_vector& operator=(const fixed_vector&) noexcept(std::is_nothrow_copy_assignable_v<T>) = default;
    fixed_vector& operator=(fixed_vector&&) noexcept(std::is_nothrow_move_assignable_v<T>) = default;
    ~fixed_vector() = default;

    /// Create \a count elements equal to \a value (\c size()==count)
    /**
    * \exception std::bad_alloc if \a count > \c max_size().
    */
    constexpr explicit fixed_vector(const std::size_t count, const T& value)
    {
        resize(count, value);
    }

    /// Create \a count value-initialized elements (\c size()==count)
    /**
    * \param count The number of elements, not a capacity to reserve.  Capacity is already \a N
    * at construction, and \c reserve() is what lowers it.
    * \exception std::bad_alloc if \a count > \c max_size().
    */
    constexpr explicit fixed_vector(const std::size_t count)
    {
        if (count > max_size())
            throw std::bad_alloc{};

        // Elements of data_ are already value-initialized.
        size_ = count;
    }

    /// Copy the elements of \a spn (\c size()==std::size(spn))
    /**
    * \exception std::bad_alloc if \a spn does not fit in \c max_size().
    */
    constexpr explicit fixed_vector(const std::span<const T> spn) { append_range(spn); }

    /// Copy the elements of <code>[first, last)</code>
    /**
    * \exception std::bad_alloc if the source does not fit in \c max_size().
    */
    template <std::input_iterator It, std::sentinel_for<It> S>
    constexpr explicit fixed_vector(It first, S last)
    {
        append_range(first, last);
    }

    /// Copy \a count elements starting at \a first (\c size()==count)
    /**
    * \exception std::bad_alloc if \a count > \c max_size().
    */
    template <std::input_iterator It>
    constexpr explicit fixed_vector(It first, const std::size_t count)
    {
        append_range(first, count);
    }

    /// Copy the elements of \a il (\c size()==il.size())
    /**
    * \exception std::bad_alloc if \a il does not fit in \c max_size().
    */
    constexpr fixed_vector(const std::initializer_list<T> il) { append_range(il); }

    /// Copy the elements of \a rg
    /**
    * \exception std::bad_alloc if the source does not fit in \c max_size().
    */
    template <std::ranges::input_range R>
    constexpr explicit fixed_vector(std::from_range_t, R&& rg)
    {
        append_range(std::forward<R>(rg));
    }

    /**
    * \exception std::bad_alloc if \a il does not fit in \c capacity().
    */
    constexpr fixed_vector& operator=(const std::initializer_list<T> il)
    {
        assign_range(il);
        return *this;
    }

    /// Swap the sizes, the capacities, and all \c max_size() array slots
    constexpr void swap(fixed_vector& other) noexcept(std::is_nothrow_swappable_v<T>)
    {
        std::swap(size_, other.size_);
        std::swap(capacity_, other.capacity_);
        std::swap(data_, other.data_);
    }

    friend constexpr void swap(fixed_vector& a, fixed_vector& b)
        noexcept(std::is_nothrow_swappable_v<T>)
    {
        a.swap(b);
    }

    /// Get the current capacity, which is in [0, \a N] and moved by \c reserve()
    [[nodiscard]] constexpr std::size_t capacity() const noexcept { return capacity_; }

    /// Get \a N, the number of array slots, which is the capacity \c reserve() may not exceed
    [[nodiscard]] static constexpr std::size_t max_size() noexcept { return N; }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }

    /// Get the amount of reserved unused space (i.e., between \c size() and \c capacity())
    [[nodiscard]] constexpr std::size_t reserved_unused() const noexcept
    {
        return capacity() - size();
    }

    /// Get the amount of unreserved space (i.e., between \c capacity() and \c max_size())
    [[nodiscard]] constexpr std::size_t unreserved() const noexcept
    {
        return max_size() - capacity();
    }

    [[nodiscard]] constexpr bool is_empty() const noexcept { return size() == 0; }

    [[nodiscard]] constexpr bool is_full() const noexcept { return size() == capacity(); }

    /**
    * \note The elements are not destroyed.
    */
    constexpr void clear() noexcept { size_ = 0; }

    /// Set the capacity to \a new_cap, the limit that every space check consults
    /**
    * Growing leaves the newly reserved slots holding whatever was last written there (\c T{}
    * if never written).  Shrinking below \c size() truncates \c size() to \a new_cap, leaving
    * the excess elements alive and readable again once the capacity is grown back.
    *
    * \note Unlike \c std::vector::reserve, this shrinks as well as grows.
    * \post <code>capacity() == new_cap && size() <= capacity()</code>
    * \exception std::bad_alloc if \a new_cap > \c max_size().
    */
    constexpr void reserve(const std::size_t new_cap)
    {
        if (new_cap > max_size())
            throw std::bad_alloc{};

        if (new_cap < size())
            size_ = new_cap;

        capacity_ = new_cap;
    }

    /// Resize to \a count elements
    /**
    * Growing assigns \a value to the new elements.  Shrinking leaves the removed ones alive
    * and unchanged (nothing is destroyed).
    * \note \c resize(capacity(), \a value) is how to fill only the reserved-unused tail
    * [\c size(), \c capacity()) and grow into it.  \c fill_capacity() overwrites the live
    * elements as well.
    * \exception std::bad_alloc if \a count > \c capacity().
    */
    constexpr void resize(const std::size_t count, const T& value)
    {
        if (count > capacity())
            throw std::bad_alloc{};

        if (count > size())
            (void)std::ranges::fill(end(), data() + count, value);

        size_ = count;
    }

    /**
    * \exception std::bad_alloc if \a count > \c capacity().
    */
    constexpr void resize(const std::size_t count) { resize(count, T{}); }

    /**
    * \note The removed element is not destroyed.
    * \note Popping an empty vector is a no-op, unlike \c std::inplace_vector::pop_back, where
    * it is UB.
    */
    constexpr void pop_back() noexcept
    {
        if (is_empty())
            return;

        --size_;
    }

    /**
    * \pre \c !is_full()
    * \note "Emplace" cannot construct in place here.  The slot already holds a live element, so
    * a temporary \c T is constructed from \a args and move-assigned in, which is equivalent to
    * \c push_back(T(args...)).  Kept for API parity with \c std::inplace_vector.
    */
    template <class... Args>
    requires std::constructible_from<T, Args...> && std::assignable_from<T&, T>
    constexpr void unchecked_emplace_back(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...> &&
                 std::is_nothrow_assignable_v<T&, T>)
    {
#if defined(DEBUG)
        assert(!is_full());
#endif
        *end() = T(std::forward<Args>(args)...);
        ++size_;
    }

    /**
    * \exception std::bad_alloc if \c is_full().
    */
    template <class... Args>
    requires std::constructible_from<T, Args...> && std::assignable_from<T&, T>
    constexpr void emplace_back(Args&&... args)
    {
        if (is_full())
            throw std::bad_alloc{};

        unchecked_emplace_back(std::forward<Args>(args)...);
    }

    template <class... Args>
    requires std::constructible_from<T, Args...> && std::assignable_from<T&, T>
    [[nodiscard]] constexpr bool try_emplace_back(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...> &&
                 std::is_nothrow_assignable_v<T&, T>)
    {
        if (is_full())
            return false;

        unchecked_emplace_back(std::forward<Args>(args)...);
        return true;
    }

    /**
    * \pre \c !is_full()
    */
    constexpr void unchecked_push_back(const T& value)
        noexcept(noexcept(unchecked_emplace_back(value)))
    {
        unchecked_emplace_back(value);
    }

    /**
    * \pre \c !is_full()
    */
    constexpr void unchecked_push_back(T&& value)
        noexcept(noexcept(unchecked_emplace_back(std::move(value))))
    {
        unchecked_emplace_back(std::move(value));
    }

    /**
    * \exception std::bad_alloc if \c is_full().
    */
    constexpr void push_back(const T& value) { emplace_back(value); }

    /// \copydoc push_back(const T&)
    constexpr void push_back(T&& value) { emplace_back(std::move(value)); }

    [[nodiscard]] constexpr bool try_push_back(const T& value)
        noexcept(noexcept(try_emplace_back(value)))
    {
        return try_emplace_back(value);
    }

    [[nodiscard]] constexpr bool try_push_back(T&& value)
        noexcept(noexcept(try_emplace_back(std::move(value))))
    {
        return try_emplace_back(std::move(value));
    }

    /// Fill all \c capacity() elements with \a value and set \c size() to \c capacity()
    /**
    * The live elements are overwritten too, not only the reserved-unused tail.  To fill just
    * the tail and grow into it, call \c resize(capacity(), \a value) instead.  To fill just the
    * live elements, call \c fill_size().
    * \note The unreserved slots beyond \c capacity() are left alone.
    */
    constexpr void fill_capacity(const T& value)
        noexcept(std::is_nothrow_copy_assignable_v<T>)
    {
        (void)std::ranges::fill(data(), data() + capacity(), value);
        size_ = capacity();
    }

    /// Fill the live elements [0, \c size()) with \a value, leaving \c size() unchanged
    constexpr void fill_size(const T& value)
        noexcept(std::is_nothrow_copy_assignable_v<T>)
    {
        (void)std::ranges::fill(span(), value);
    }

    /// Zeroize the reserved tail [\c size(), \c capacity()), leaving \c size() unchanged
    /**
    * The tail elements stay alive with an all-zero object representation, which for a scalar
    * \c T is the value-initialized value.  Unlike a plain fill, the stores are not elidable, so
    * \c clear() followed by this scrubs everything up to \c capacity().  Constant evaluation
    * has no memory to scrub, so the tail is value-assigned there instead.
    * \note \c zeroize_unreserved() covers the slots beyond a reduced capacity.
    */
    constexpr void zeroize_reserved_unused() noexcept
    requires std::is_trivially_copyable_v<T>
    {
        if consteval
        {
            for (std::size_t i = size(); i < capacity(); ++i)
                data()[i] = T{};
        }
        else
        {
            if (reserved_unused() != 0)
                zero_explicit_(static_cast<void*>(end()), reserved_unused() * sizeof(T));
        }
    }

    /// Zeroize the unreserved slots [\c capacity(), \c max_size()), leaving \c size() unchanged
    /**
    * A \c reserve() shrink leaves these slots alive and still holding what they held while they
    * were reserved, so a full scrub needs this call alongside \c zeroize_reserved_unused().  It
    * is a no-op while \c capacity() \c == \c max_size(), and the guarantees match the reserved
    * half.
    */
    constexpr void zeroize_unreserved() noexcept
    requires std::is_trivially_copyable_v<T>
    {
        if consteval
        {
            for (std::size_t i = capacity(); i < max_size(); ++i)
                data()[i] = T{};
        }
        else
        {
            if (unreserved() != 0)
                zero_explicit_(static_cast<void*>(data() + capacity()),
                               unreserved() * sizeof(T));
        }
    }

    /**
    * \pre \a spn does not overlap this vector's storage.
    * \note The check is made up front, so nothing is appended when it throws.
    * \exception std::bad_alloc if \a spn does not fit in \c reserved_unused().
    */
    constexpr void append_range(const std::span<const T> spn)
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
    * it throws.  An unsized one appends the elements that fit before throwing.
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
        {
            emplace_back(*first);
        }
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
    constexpr void append_range(const std::initializer_list<T> il)
    {
        append_range(std::span<const T>{std::data(il), std::size(il)});
    }

    /**
    * \pre \a rg does not overlap this vector's storage if it is a contiguous range of \c T.
    * \note A sized source is checked up front, so nothing is appended when it throws.  An
    * unsized one appends the elements that fit before throwing.
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
            {
                unchecked_emplace_back(std::forward<decltype(e)>(e));
            }
        }
        else
        {
            for (auto&& e : std::forward<R>(rg))
            {
                emplace_back(std::forward<decltype(e)>(e));
            }
        }
    }

    /**
    * \pre \a spn does not overlap this vector's storage.
    */
    [[nodiscard]] constexpr bool try_append_range(const std::span<const T> spn)
        noexcept(std::is_nothrow_copy_assignable_v<T>)
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
    * \c false.  An unsized one has already appended the elements that fit when \c false is
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

    [[nodiscard]] constexpr bool try_append_range(const std::initializer_list<T> il)
        noexcept(std::is_nothrow_copy_assignable_v<T>)
    {
        return try_append_range(std::span<const T>{std::data(il), std::size(il)});
    }

    /**
    * \pre \a rg does not overlap this vector's storage if it is a contiguous range of \c T.
    * \note A sized source is checked up front, so nothing is appended on \c false.  An unsized
    * one has already appended the elements that fit when \c false is returned.
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
            {
                unchecked_emplace_back(std::forward<decltype(e)>(e));
            }

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
    * \pre The source does not overlap this vector's storage.
    * \note The replaced elements are not destroyed.
    * \note The \c clear() happens first, so the previous contents are gone whether the assign
    * succeeds or fails.  A sized source then leaves the vector empty, and an unsized one leaves
    * the elements that fit.
    * \exception std::bad_alloc if the source does not fit in \c capacity().
    */
    constexpr void assign_range(const std::span<const T> spn)
    {
        clear();
        append_range(spn);
    }

    /// \copydoc assign_range(std::span<const T>)
    template <std::input_iterator It, std::sentinel_for<It> S>
    constexpr void assign_range(It first, S last)
    {
        clear();
        append_range(first, last);
    }

    /// \copydoc assign_range(std::span<const T>)
    template <std::input_iterator It>
    constexpr void assign_range(It first, const std::size_t count)
    {
        clear();
        append_range(first, count);
    }

    /// \copydoc assign_range(std::span<const T>)
    constexpr void assign_range(const std::initializer_list<T> il)
    {
        clear();
        append_range(il);
    }

    /// \copydoc assign_range(std::span<const T>)
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

    /**
    * \note The array member carries the \c alignas(Align), so the compiler already knows the
    * alignment and no \c std::assume_aligned is needed.
    */
    [[nodiscard]] constexpr T* data() noexcept { return std::data(data_); }

    /// \copydoc data()
    [[nodiscard]] constexpr const T* data() const noexcept { return std::data(data_); }

    /**
    * \pre \c !is_empty()
    */
    [[nodiscard]] constexpr T& front() noexcept
    {
#if defined(DEBUG)
        assert(!is_empty());
#endif
        return *begin();
    }

    /// \copydoc front()
    [[nodiscard]] constexpr const T& front() const noexcept
    {
#if defined(DEBUG)
        assert(!is_empty());
#endif
        return *begin();
    }

    /**
    * \pre \c !is_empty()
    */
    [[nodiscard]] constexpr T& back() noexcept
    {
#if defined(DEBUG)
        assert(!is_empty());
#endif
        return *rbegin();
    }

    /// \copydoc back()
    [[nodiscard]] constexpr const T& back() const noexcept
    {
#if defined(DEBUG)
        assert(!is_empty());
#endif
        return *rbegin();
    }

    /**
    * \pre \a i < \c capacity()
    * \note The index is unchecked and bounded by \c capacity(), not \c size(), so an index in
    * [size(), capacity()) reads a live element.  \c at() is the bounds-checked accessor.
    * \note The bound is the \e current capacity, so a \c reserve() shrink puts the slots beyond
    * it out of contract even though they stay alive.
    */
    [[nodiscard]] constexpr T& operator[](const std::size_t i) noexcept
    {
#if defined(DEBUG)
        assert(i < capacity());
#endif
        return data()[i];
    }

    /// \copydoc operator[](std::size_t)
    [[nodiscard]] constexpr const T& operator[](const std::size_t i) const noexcept
    {
#if defined(DEBUG)
        assert(i < capacity());
#endif
        return data()[i];
    }

    /**
    * \note This is the only bounds-checked accessor, and it checks against \c size(), so it
    * rejects an index in [size(), capacity()) that \c operator[] would read.
    * \exception std::out_of_range if \a i >= \c size().
    */
    [[nodiscard]] constexpr T& at(const std::size_t i)
    {
        check_idx_(i);
        return data()[i];
    }

    /// \copydoc at(std::size_t)
    [[nodiscard]] constexpr const T& at(const std::size_t i) const
    {
        check_idx_(i);
        return data()[i];
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
        noexcept(noexcept(std::declval<const T&>() == std::declval<const T&>()))
    requires std::equality_comparable<T>
    {
        return std::ranges::equal(span(), rhs.span());
    }

    [[nodiscard]] constexpr auto operator<=>(const fixed_vector& rhs) const
        noexcept(noexcept(std::declval<const T&>() <=> std::declval<const T&>()))
    requires std::three_way_comparable<T>
    {
        return std::lexicographical_compare_three_way(begin(), end(), rhs.begin(), rhs.end());
    }
};
