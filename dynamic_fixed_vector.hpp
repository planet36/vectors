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
#if defined(DEBUG)
#include <cassert>
#endif
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
#include <string.h> // memset_explicit, explicit_bzero
#include <type_traits>
#include <utility>

/// A resizable array container whose fixed capacity is set at run time
/**
* The capacity (a.k.a. \c max_size()) is a constructor argument rather than a template
* parameter, so \c capacity() and \c max_size() are non-static.  It is settled at construction
* and never changes, since changing it would mean reallocating.
*
* The properties that shape the interface:
*   - Storage is an over-alignable heap block allocated with the aligned \c ::operator \c new
*     and owned by a \c std::unique_ptr.  \a Align may exceed \c alignof(T) (e.g. \c std::byte
*     data aligned like a 16-byte SIMD lane).  \c data() applies \c std::assume_aligned<Align>
*     (guarded for the null/empty case) so caller loops can vectorize on the known alignment.
*   - All \c capacity() elements are alive from construction onward.  The reserve constructor
*     value-initializes them.  The copy, fill, and range constructors construct them directly
*     from the source.
*   - Elements are never explicitly destroyed.  \c clear(), \c pop_back(), and \c resize()
*     only change the size.
*   - \c operator[] is unchecked, and its bound is \c capacity() rather than \c size().
*     An index in [\c size(), \c capacity()) legitimately reads a live element.  \c at() is
*     the bounds-checked accessor.
*   - The single-argument constructor reserves capacity and starts \b empty (\c size()==0).
*   - Range and iterator-sentinel constructors require \b forward iterators, since the capacity
*     must be computed up front.  An input-only source needs `dynamic_fixed_vector(capacity)`
*     followed by \c append_range.
*   - Copy makes a deep copy.  Move construction transfers ownership and leaves the source
*     empty (capacity 0).  Move \e assignment swaps, so the source is left holding this
*     vector's former buffer (freed when the source is destroyed).
*   - Capacity overflow throws \c std::bad_alloc.  The \c try_* family returns \c bool
*     instead of throwing.
*
* The interface is annotated \c constexpr, but over-aligned allocation is not usable in constant
* evaluation, so only empty (non-allocating) instances are usable in constant expressions.
*
* \note \a Align defaults to <code>max(alignof(std::size_t), alignof(T))</code>, which is at
* least a word.  The block is therefore word-aligned even for a narrow \a T.
*
* \invariant \c size() \c <= \c capacity().
* \invariant \c data() is null \b exactly when \c capacity() is 0.  A capacity of 0 allocates
* nothing, and the aligned \c ::operator \c new never returns null (it throws), so no other
* state holds a null block.
*
* Together those make the preconditions below sufficient on their own.  \c !is_full(),
* \c !is_empty(), and <code>i < capacity()</code> each imply a non-null, \a Align-aligned block,
* so the members carrying them index \c data() without re-checking it for null.
*
* \warning This container is only suitable for trivially destructible types.
*/
template <typename T,
          std::size_t Align = std::max(alignof(std::size_t), alignof(T))>
requires std::default_initializable<T> && std::movable<T> &&
         std::is_trivially_destructible_v<T> &&
         (std::has_single_bit(Align)) && (Align >= alignof(T))
class dynamic_fixed_vector
{
private:
    /// Stateless deleter that frees a block from the aligned \c ::operator \c new
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

    /// Allocate an over-aligned block for \a cap elements without beginning any lifetimes
    /**
    * The caller must begin the lifetime of every element (e.g. with the \c std::uninitialized_*
    * algorithms or \c std::construct_at) before the container is used.  A capacity of 0
    * allocates nothing.
    */
    [[nodiscard]] static constexpr storage_ptr allocate_raw_(const std::size_t cap)
    {
        // This early return is not an optimization.  ::operator new(0) returns a non-null
        // block, so without it the invariant "capacity 0 implies null data()" would not hold.
        if (cap == 0)
            return nullptr;

        // Guard the size computation.  The aligned ::operator new is called with a size we
        // compute ourselves, so the language's array-new overflow check does not apply.
        if (cap > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_alloc{};

        return storage_ptr{
            static_cast<T*>(::operator new(cap * sizeof(T), std::align_val_t{Align}))};
    }

    /// Allocate an over-aligned block of \a cap value-initialized elements
    /**
    * \note The elements are never individually destroyed.  \c aligned_deleter frees the whole
    * block.
    */
    [[nodiscard]] static constexpr storage_ptr allocate_(const std::size_t cap)
    {
        // Own the raw block first so a throwing value-init still frees it.
        storage_ptr up = allocate_raw_(cap);
        if (cap != 0)
            std::uninitialized_value_construct_n(up.get(), cap);
        return up;
    }

    /// Tag for the constructor that allocates without beginning element lifetimes
    struct raw_alloc_t {};

    /// Allocate \a capacity slots with \c size()==capacity but no lifetimes begun
    /**
    * The delegating constructor's body must begin the lifetime of every element.  A throwing
    * element constructor still frees the block (owned by \c data_).  The already-constructed
    * elements need no destruction (\c T is trivially destructible).
    */
    constexpr dynamic_fixed_vector(raw_alloc_t, const std::size_t capacity)
        : size_{capacity}, capacity_{capacity}, data_{allocate_raw_(capacity)}
    {}

    constexpr void check_idx_(const std::size_t i) const
    {
        if (i >= size())
            throw std::out_of_range("dynamic_fixed_vector: index >= size");
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

    /// Zero \a n bytes at \a p with stores the compiler must not elide
    /**
    * Uses \c ::memset_explicit (C23) or \c explicit_bzero (glibc, BSDs) when the C library
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

    constexpr dynamic_fixed_vector() noexcept = default;

    /**
    * \exception std::bad_alloc if the allocation fails.
    */
    constexpr dynamic_fixed_vector(const dynamic_fixed_vector& other)
        : size_{other.size_}, capacity_{other.capacity_}, data_{allocate_raw_(other.capacity_)}
    {
        // Copy the entire capacity buffer (faithful to beyond-size operator[] reads),
        // beginning each element's lifetime directly, with no value-init-then-overwrite.
        if (this->capacity() != 0)
            std::uninitialized_copy_n(other.data(), this->capacity(), data());
    }

    constexpr dynamic_fixed_vector(dynamic_fixed_vector&& other) noexcept
        : size_{std::exchange(other.size_, 0)},
          capacity_{std::exchange(other.capacity_, 0)},
          data_{std::move(other.data_)}
    {}

    /**
    * \exception std::bad_alloc if the allocation fails.
    */
    constexpr dynamic_fixed_vector& operator=(const dynamic_fixed_vector& other)
    {
        if (this == &other)
        {
            return *this;
        }
        dynamic_fixed_vector tmp{other};
        swap(tmp);
        return *this;
    }

    /// Swap-based move assignment
    /**
    * \a other is left holding this vector's former buffer rather than being emptied.  That
    * buffer is freed when \a other is destroyed.
    */
    constexpr dynamic_fixed_vector& operator=(dynamic_fixed_vector&& other) noexcept
    {
        swap(other);
        return *this;
    }

    ~dynamic_fixed_vector() = default;

    /// Reserve capacity \a capacity, leaving the vector empty
    /**
    * \exception std::bad_alloc if the allocation fails, or if <code>capacity * sizeof(T)</code>
    * would overflow \c std::size_t.
    */
    constexpr explicit dynamic_fixed_vector(const std::size_t capacity)
        : capacity_{capacity}, data_{allocate_(capacity)}
    {}

    /// Reserve capacity \a capacity and fill it with \a value (\c size()==capacity)
    /**
    * \copydetails dynamic_fixed_vector(std::size_t)
    */
    constexpr explicit dynamic_fixed_vector(const std::size_t capacity, const T& value)
        : dynamic_fixed_vector(raw_alloc_t{}, capacity)
    {
        if (this->capacity() != 0)
            std::uninitialized_fill_n(data(), this->capacity(), value);
    }

    /// Capacity is the size of \a spn
    /**
    * \exception std::bad_alloc if the allocation fails, or if the byte count would overflow
    *            \c std::size_t.
    */
    constexpr explicit dynamic_fixed_vector(const std::span<const T> spn)
        : dynamic_fixed_vector(raw_alloc_t{}, std::size(spn))
    {
        if (capacity() != 0)
            std::uninitialized_copy_n(std::data(spn), capacity(), data());
    }

    /// Capacity is the distance between \a first and \a last
    /**
    * \exception std::bad_alloc if the allocation fails, or if the byte count would overflow
    *            \c std::size_t.
    */
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

    /// Capacity is \a count
    /**
    * \exception std::bad_alloc if the allocation fails, or if the byte count would overflow
    *            \c std::size_t.
    */
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

    /// Capacity is the size of \a il
    /**
    * \exception std::bad_alloc if the allocation fails, or if the byte count would overflow
    *            \c std::size_t.
    */
    constexpr dynamic_fixed_vector(const std::initializer_list<T> il)
        : dynamic_fixed_vector(std::data(il), std::size(il))
    {}

    /// Capacity is the size of \a rg
    /**
    * \exception std::bad_alloc if the allocation fails, or if the byte count would overflow
    *            \c std::size_t.
    */
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

    /**
    * \exception std::bad_alloc if \a il does not fit in \c capacity().
    */
    constexpr dynamic_fixed_vector& operator=(const std::initializer_list<T> il)
    {
        assign_range(il);
        return *this;
    }

    constexpr void swap(dynamic_fixed_vector& other) noexcept
    {
        std::swap(size_, other.size_);
        std::swap(capacity_, other.capacity_);
        std::swap(data_, other.data_);
    }

    friend constexpr void swap(dynamic_fixed_vector& a, dynamic_fixed_vector& b) noexcept
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
    * \note The elements are not destroyed.
    */
    constexpr void clear() noexcept { size_ = 0; }

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
    * \note "Emplace" cannot construct in place here.  The slot already holds a live element,
    * so a temporary \c T is constructed from \a args and move-assigned in, which is equivalent
    * to \c push_back(T(args...)).  Kept for API parity with \c std::inplace_vector.
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
    */
    constexpr void fill_capacity(const T& value)
        noexcept(std::is_nothrow_copy_assignable_v<T>)
    {
        (void)std::ranges::fill(std::span<T>{data(), capacity()}, value);
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
    * \c clear() followed by this scrubs the whole buffer.
    */
    constexpr void zeroize_reserved_unused() noexcept
    requires std::is_trivially_copyable_v<T>
    {
        if (reserved_unused() != 0)
            zero_explicit_(static_cast<void*>(end()), reserved_unused() * sizeof(T));
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
                unchecked_emplace_back(std::forward<decltype(e)>(e));
        }
        else
        {
            for (auto&& e : std::forward<R>(rg))
                emplace_back(std::forward<decltype(e)>(e));
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
    * \pre The source does not overlap this vector's storage.
    * \note The replaced elements are not destroyed, and the capacity is kept rather than
    * resized to the source.
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

    [[nodiscard]] constexpr std::span<const T> span() const noexcept { return {data(), size()}; }

    [[nodiscard]] constexpr explicit operator std::span<T>() noexcept { return span(); }

    [[nodiscard]] constexpr explicit operator std::span<const T>() const noexcept
    {
        return span();
    }

    /**
    * \return A pointer to the block, aligned to \a Align, or \c nullptr if \c capacity()
    * is 0.
    * \note The null test is not defensive.  \c std::assume_aligned requires a pointer to a
    * real object, so it may not be applied to the empty container's null block.
    */
    [[nodiscard]] constexpr T* data() noexcept
    {
        T* const p = data_.get();
        return p != nullptr ? std::assume_aligned<Align>(p) : p;
    }

    /// \copydoc data()
    [[nodiscard]] constexpr const T* data() const noexcept
    {
        const T* const p = data_.get();
        return p != nullptr ? std::assume_aligned<Align>(p) : p;
    }

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

    [[nodiscard]] constexpr bool operator==(const dynamic_fixed_vector& rhs) const
        noexcept(noexcept(std::declval<const T&>() == std::declval<const T&>()))
    requires std::equality_comparable<T>
    {
        return std::ranges::equal(span(), rhs.span());
    }

    [[nodiscard]] constexpr auto operator<=>(const dynamic_fixed_vector& rhs) const
        noexcept(noexcept(std::declval<const T&>() <=> std::declval<const T&>()))
    requires std::three_way_comparable<T>
    {
        return std::lexicographical_compare_three_way(begin(), end(), rhs.begin(), rhs.end());
    }
};
