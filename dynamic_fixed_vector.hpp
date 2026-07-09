// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

/**
* \file
* \author Steven Ward
*
* Defines the class \c dynamic_fixed_vector, a fixed-capacity vector whose capacity is
* chosen at run time and whose storage may be over-aligned.
*/

#pragma once

#include <algorithm>
#include <bit>
#include <compare>
#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

/// A resizable array container whose fixed capacity is set at run time.
/**
* This is the run-time-capacity sibling of \c fixed_vector.  It behaves like \c fixed_vector
* except:
*   - The capacity (a.k.a. \c max_size()) is passed to the constructor instead of being a
*     template parameter, so \c capacity() / \c max_size() are non-static.
*   - Storage is an over-alignable heap block allocated with the aligned \c ::operator \c new
*     and owned by a \c std::unique_ptr.  \a Align may exceed \c alignof(T) (e.g. \c std::byte
*     data aligned like a 16-byte SIMD lane).  \c data() applies \c std::assume_aligned<Align>
*     (guarded for the null/empty case) so caller loops can vectorize on the known alignment.
*   - The single-argument constructor reserves capacity and starts \b empty
*     (\c size()==0), unlike \c fixed_vector where it created elements.
*   - Range / iterator-sentinel constructors require \b forward iterators (the capacity must
*     be computed up front); input-only sources use `dynamic_fixed_vector(capacity)` followed
*     by \c append_range.
*   - Copy makes a deep copy.  Move construction transfers ownership and leaves the source
*     empty (capacity 0); move \e assignment swaps, so the source is left holding this
*     vector's former buffer (freed when the source is destroyed).  The interface is
*     annotated \c constexpr, but because over-aligned
*     allocation is not usable in constant evaluation, only empty (non-allocating) instances
*     are usable in constant expressions.
*
* Like \c fixed_vector: all \c capacity() elements are alive from construction onward
* (value-initialized by the reserve constructor; constructed directly from the source by
* the copy/fill/range constructors), elements are never explicitly destroyed, \c operator[]
* is unchecked and
* capacity-based, \c at() is bounds-checked, capacity overflow throws \c std::bad_alloc, and
* the \c try_* family returns \c bool.
*
* \warning This container is only suitable for trivially destructible types.
*
* \sa fixed_vector
*/
template <typename T,
          std::size_t Align = alignof(T)>
requires std::default_initializable<T> && std::movable<T> &&
         std::is_trivially_destructible_v<T> &&
         (std::has_single_bit(Align)) && (Align >= alignof(T))
class dynamic_fixed_vector
{
private:
    /// Stateless deleter that frees a block from the aligned \c ::operator \c new.
    struct aligned_deleter
    {
        constexpr void operator()(T* const p) const noexcept
        {
            ::operator delete(p, std::align_val_t{Align});
        }
    };

    using storage_ptr = std::unique_ptr<T, aligned_deleter>;

    std::size_t size_{};
    std::size_t capacity_{};
    storage_ptr data_{};

    /// Allocate an over-aligned block for \a cap elements without beginning any lifetimes.
    /**
    * The caller must begin the lifetime of every element (e.g. with the
    * \c std::uninitialized_* algorithms or \c std::construct_at) before the container is
    * used.  The whole block is freed by \c aligned_deleter.  A capacity of 0 allocates
    * nothing.
    */
    [[nodiscard]] static constexpr storage_ptr allocate_raw_(const std::size_t cap)
    {
        if (cap == 0)
            return nullptr;

        // Guard the size computation: the aligned ::operator new is called with a size we
        // compute ourselves, so the language's array-new overflow check does not apply.
        if (cap > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_alloc{};

        return storage_ptr{
            static_cast<T*>(::operator new(cap * sizeof(T), std::align_val_t{Align}))};
    }

    /// Allocate an over-aligned block of \a cap value-initialized elements.
    /**
    * The elements are never individually destroyed; the whole block is freed by
    * \c aligned_deleter.  A capacity of 0 allocates nothing.
    */
    [[nodiscard]] static constexpr storage_ptr allocate_(const std::size_t cap)
    {
        // Own the raw block first so a throwing value-init still frees it.
        storage_ptr up = allocate_raw_(cap);
        if (cap != 0)
            std::uninitialized_value_construct_n(up.get(), cap);
        return up;
    }

    /// Tag for the constructor that allocates without beginning element lifetimes.
    struct raw_alloc_t {};

    /// Allocate \a capacity slots with \c size()==capacity but no lifetimes begun.
    /**
    * The delegating constructor's body must begin the lifetime of every element.  A throwing
    * element constructor still frees the block (owned by \c data_); the already-constructed
    * elements need no destruction (\c T is trivially destructible).
    */
    constexpr dynamic_fixed_vector(raw_alloc_t, const std::size_t capacity)
        : size_{capacity}, capacity_{capacity}, data_{allocate_raw_(capacity)}
    {}

    /// Check the index \a i against \c size()
    constexpr void check_idx_(const std::size_t i) const
    {
        if (i >= size())
            throw std::out_of_range("dynamic_fixed_vector: index >= size");
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

    constexpr dynamic_fixed_vector() noexcept = default;

    constexpr dynamic_fixed_vector(const dynamic_fixed_vector& other)
        : size_{other.size_}, capacity_{other.capacity_}, data_{allocate_raw_(other.capacity_)}
    {
        // Copy the entire capacity buffer (faithful to beyond-size operator[] reads),
        // beginning each element's lifetime directly -- no value-init-then-overwrite.
        if (capacity_ != 0)
            std::uninitialized_copy_n(other.data(), capacity_, data());
    }

    constexpr dynamic_fixed_vector(dynamic_fixed_vector&& other) noexcept
        : size_{std::exchange(other.size_, 0)},
          capacity_{std::exchange(other.capacity_, 0)},
          data_{std::move(other.data_)}
    {}

    constexpr dynamic_fixed_vector& operator=(const dynamic_fixed_vector& other)
    {
        dynamic_fixed_vector tmp{other};
        swap(tmp);
        return *this;
    }

    /// Swap-based: \a other is left holding this vector's former buffer, not emptied.
    constexpr dynamic_fixed_vector& operator=(dynamic_fixed_vector&& other) noexcept
    {
        swap(other);
        return *this;
    }

    ~dynamic_fixed_vector() = default;

    /// Reserve capacity \a capacity; the vector starts empty.
    constexpr explicit dynamic_fixed_vector(const std::size_t capacity)
        : size_{0}, capacity_{capacity}, data_{allocate_(capacity)}
    {}

    /// Reserve capacity \a capacity and fill it with \a value (\c size()==capacity).
    constexpr explicit dynamic_fixed_vector(const std::size_t capacity, const T& value)
        : dynamic_fixed_vector(raw_alloc_t{}, capacity)
    {
        if (capacity_ != 0)
            std::uninitialized_fill_n(data(), capacity_, value);
    }

    /// Capacity is the size of \a spn.
    constexpr explicit dynamic_fixed_vector(const std::span<const T> spn)
        : dynamic_fixed_vector(raw_alloc_t{}, std::size(spn))
    {
        if (capacity_ != 0)
            std::uninitialized_copy_n(std::data(spn), capacity_, data());
    }

    /// Capacity is the distance between \a first and \a last (forward iterators required).
    template <std::forward_iterator It, std::sentinel_for<It> S>
    constexpr explicit dynamic_fixed_vector(It first, S last)
        : dynamic_fixed_vector(raw_alloc_t{},
                               static_cast<std::size_t>(std::ranges::distance(first, last)))
    {
        std::size_t i = 0;
        for (; first != last; ++first)
        {
            std::construct_at(data() + i, *first);
            ++i;
        }
    }

    /// Capacity is \a count.
    template <std::input_iterator It>
    constexpr explicit dynamic_fixed_vector(It first, const std::size_t count)
        : dynamic_fixed_vector(raw_alloc_t{}, count)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            std::construct_at(data() + i, *first);
            ++first;
        }
    }

    constexpr dynamic_fixed_vector(const std::initializer_list<T> il)
        : dynamic_fixed_vector(std::data(il), std::size(il))
    {}

    /// Capacity is the size of \a rg (forward range required).
    template <std::ranges::forward_range R>
    constexpr explicit dynamic_fixed_vector(std::from_range_t, R&& rg)
        : dynamic_fixed_vector(raw_alloc_t{}, static_cast<std::size_t>(std::ranges::distance(rg)))
    {
        std::size_t i = 0;
        for (auto&& e : std::forward<R>(rg))
        {
            std::construct_at(data() + i, std::forward<decltype(e)>(e));
            ++i;
        }
    }

    constexpr dynamic_fixed_vector& operator=(const std::initializer_list<T> il)
    {
        assign_range(il);
        return *this;
    }

    constexpr void swap(dynamic_fixed_vector& other) noexcept
    {
        using std::swap;
        swap(size_, other.size_);
        swap(capacity_, other.capacity_);
        swap(data_, other.data_);
    }

    friend constexpr void swap(dynamic_fixed_vector& a, dynamic_fixed_vector& b) noexcept
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

    /// \note Does not destroy elements.
    constexpr void clear() noexcept { size_ = 0; }

    /// Resize to \a count elements
    /**
    * If \a count > \c size(), new elements are assigned \a value.
    * If \a count <= \c size(), removed elements are unchanged.
    * \note Does not destroy elements.
    */
    constexpr void resize(const std::size_t count, const T& value)
    {
        if (count > capacity_)
            throw std::bad_alloc{};

        if (count > size())
            (void)std::ranges::fill(data() + size(), data() + count, value);

        size_ = count;
    }

    constexpr void resize(const std::size_t count) { resize(count, T{}); }

    /// \note Does not destroy elements.
    /// \note No-op if empty (unlike \c std::inplace_vector::pop_back, where that is UB).
    constexpr void pop_back() noexcept
    {
        if (is_empty())
            return;

        --size_;
    }

    /// \pre \c !is_full()
    template <class... Args>
    requires std::constructible_from<T, Args...> && std::assignable_from<T&, T>
    constexpr void unchecked_emplace_back(Args&&... args)
    {
        *end() = T(std::forward<Args>(args)...);
        ++size_;
    }

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

    /// \pre \c !is_full()
    constexpr void unchecked_push_back(const T& value) { unchecked_emplace_back(value); }

    constexpr void unchecked_push_back(T&& value) { unchecked_emplace_back(std::move(value)); }

    constexpr void push_back(const T& value) { emplace_back(value); }

    constexpr void push_back(T&& value) { emplace_back(std::move(value)); }

    [[nodiscard]] constexpr bool try_push_back(const T& value) { return try_emplace_back(value); }

    [[nodiscard]] constexpr bool try_push_back(T&& value)
    {
        return try_emplace_back(std::move(value));
    }

    /// Fill all \c capacity() elements with \a value and set \c size() to \c capacity().
    constexpr void fill_capacity(const T& value)
    {
        (void)std::ranges::fill(std::span<T>{data(), capacity_}, value);
        size_ = capacity_;
    }

    /// Fill the live elements [0, \c size()) with \a value; \c size() is unchanged.
    constexpr void fill_size(const T& value) { (void)std::ranges::fill(span(), value); }

    /// \pre \a spn does not overlap this vector's storage.
    constexpr void append_range(const std::span<const T> spn)
    {
        if (std::size(spn) > remaining_space())
            throw std::bad_alloc{};

        common_append_range_(spn);
    }

    /// \note If the source size is computable up front (\c std::sized_sentinel_for), it is
    /// checked before writing (all-or-nothing).  Otherwise appends element-wise: the
    /// elements that fit are appended before \c std::bad_alloc is thrown.
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

    constexpr void append_range(const std::initializer_list<T> il)
    {
        append_range(std::span<const T>{std::data(il), std::size(il)});
    }

    /// \note Sized sources are checked up front (all-or-nothing); unsized sources append
    /// element-wise and may partially append before throwing \c std::bad_alloc.
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
    * \pre \a spn does not overlap this vector's storage.
    * \retval false if failure
    * \retval true if success
    */
    [[nodiscard]] constexpr bool try_append_range(const std::span<const T> spn)
    {
        if (std::size(spn) > remaining_space())
            return false;

        common_append_range_(spn);
        return true;
    }

    /// \note If the source size is computable up front (\c std::sized_sentinel_for), it is
    /// checked before writing (nothing appended on \c false).  Otherwise appends
    /// element-wise: on \c false, the elements that fit have already been appended
    /// (observe \c size()).
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

    /// \note Sized sources are checked up front (nothing appended on \c false); unsized
    /// sources append element-wise, so on \c false the elements that fit have already been
    /// appended (observe \c size()).
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

    /// \note Does not destroy elements.  Throws if the source exceeds \c capacity().
    /// \pre \a spn does not overlap this vector's storage.
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

    [[nodiscard]] constexpr std::span<const T> span() const noexcept { return {data(), size()}; }

    [[nodiscard]] constexpr explicit operator std::span<T>() noexcept { return span(); }

    [[nodiscard]] constexpr explicit operator std::span<const T>() const noexcept
    {
        return span();
    }

    [[nodiscard]] constexpr T* data() noexcept
    {
        T* const p = data_.get();
        return p != nullptr ? std::assume_aligned<Align>(p) : p;
    }

    [[nodiscard]] constexpr const T* data() const noexcept
    {
        const T* const p = data_.get();
        return p != nullptr ? std::assume_aligned<Align>(p) : p;
    }

    /// \pre \c !is_empty()
    [[nodiscard]] constexpr T& front() noexcept { return *begin(); }

    [[nodiscard]] constexpr const T& front() const noexcept { return *begin(); }

    /// \pre \c !is_empty()
    [[nodiscard]] constexpr T& back() noexcept { return *rbegin(); }

    [[nodiscard]] constexpr const T& back() const noexcept { return *rbegin(); }

    /// \pre \a i < \c capacity()
    /// \note Does not check bounds.  Indexes in [size(), capacity()) are valid reads (every
    /// capacity slot holds a live element); \c at() is the bounds-checked accessor.
    [[nodiscard]] constexpr T& operator[](const std::size_t i) noexcept { return data()[i]; }

    [[nodiscard]] constexpr const T& operator[](const std::size_t i) const noexcept
    {
        return data()[i];
    }

    [[nodiscard]] constexpr T& at(const std::size_t i)
    {
        check_idx_(i);
        return data()[i];
    }

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

    [[nodiscard]] constexpr bool operator==(const dynamic_fixed_vector& rhs) const
    requires std::equality_comparable<T>
    {
        return std::ranges::equal(span(), rhs.span());
    }

    [[nodiscard]] constexpr auto operator<=>(const dynamic_fixed_vector& rhs) const
    requires std::three_way_comparable<T>
    {
        return std::lexicographical_compare_three_way(begin(), end(), rhs.begin(), rhs.end());
    }
};
