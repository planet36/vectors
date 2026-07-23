// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

/**
* \file
* \author Steven Ward
* \sa https://github.com/planet36/vectors
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
#include <type_traits>
#include <utility>

#include "byte_compare.hpp"

/// A non-owning, run-time-capacity buffer of \c std::byte overlaying borrowed storage.
/**
* Same append/cursor interface as \c aligned_byte_buffer (\c is_full, \c remaining_space,
* \c append_range, \c push_back, ...), but it \b borrows the bytes it operates on instead of
* owning them: construction takes a pointer/span to memory whose lifetime the caller manages, and
* the buffer never allocates or frees.  It is the run-time-capacity, non-owning counterpart to
* \c fixed_vector<std::byte, N>.
*
* Differences from \c aligned_byte_buffer:
*   - \b Non-owning.  Just a pointer + capacity + size; no allocation, a trivial destructor, and
*     the special members are defaulted -- copy and move are shallow (both objects then view the
*     same bytes; move does not empty the source).  The type is trivially copyable and cheap to
*     pass by value.  The caller is responsible for keeping the borrowed storage alive for the
*     buffer's lifetime; a destroyed source leaves a dangling view.
*   - \b Capacity is supplied, not allocated.  There is no reserve / fill / iterator /
*     initializer-list / from-range \e element-copying constructor (those exist on
*     \c aligned_byte_buffer to size and fill owned storage).  A borrowed buffer is instead built
*     directly over existing memory -- a pointer, or a contiguous range whose storage it
*     \e overlays rather than copies -- then filled via \c append_range / \c assign_range.
*   - \b Construction leaves \c size()==0 -- the region is treated as empty scratch to build into.
*     To instead adopt bytes already present in the region (for reading, iterating, or comparing),
*     use the \c adopting named constructors, which start \c size()==capacity().
*   - \b No \a Align parameter.  \c aligned_byte_buffer over-aligns its own allocation and applies
*     \c std::assume_aligned; borrowed memory carries no such promise, so \c data() returns the
*     raw pointer unadorned.
*   - \b No \c operator=(initializer_list): writing through a view-like type via assignment reads
*     as rebinding rather than a bulk store.  Use \c assign_range for that.
*
* The element type of a source is constrained to \c std::is_trivially_copyable_v (its object
* representation is what gets written and later read back) and must be non-\c const (the view
* writes through it).
*
* Like \c aligned_byte_buffer: the reserved tail [\c size(), \c capacity()) is left as-is
* (borrowed bytes are neither zeroed nor read on construction; reading one back via \c operator[]
* yields an \e unspecified -- not indeterminate -- \c std::byte), \c operator[] is unchecked and
* capacity-based, \c at() is bounds-checked, capacity overflow throws \c std::bad_alloc, and the
* \c try_* family returns \c bool.  \c zeroize_remaining_space() and the free \c constant_time_equal
* (from \c byte_compare.hpp) are available.  Nearly the whole interface is \c constexpr, but forming
* a byte view over an object needs a \c reinterpret_cast, which is barred in constant evaluation,
* so only the default (empty) instance is usable in constant expressions.
*
* \invariant \c size() \c <= \c capacity().
* \note Unlike \c aligned_byte_buffer, \c data() is \b not guaranteed null exactly when
* \c capacity() is 0: a caller may borrow a zero-length region at a non-null address.  The mutating
* members index \c data() only under \c !is_full() / \c !is_empty() / <code>i < capacity()</code>,
* each of which implies <code>capacity() > 0</code> and therefore -- by the constructor
* precondition that the source points to at least \c capacity() writable bytes -- a non-null,
* indexable block.
*
* \sa aligned_byte_buffer
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

    /// \pre \a spn does not overlap this buffer's storage.
    constexpr void common_append_range_(const std::span<const std::byte> spn) noexcept
    {
        if (!spn.empty())
            std::memcpy(end(), spn.data(), spn.size());
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

    /// True if \a R is a sized, contiguous range of \c std::byte.
    /**
    * Such a range is handed to the \c std::span overload for its \c std::memcpy.  Overload
    * resolution will not do this on its own: for example \c std::vector<std::byte> the \c R&&
    * template is an exact match while the \c std::span overload needs a user-defined
    * conversion, so the template wins and the \c memcpy is dead code for callers who do not
    * hand-write a span.
    */
    template <typename R>
    static constexpr bool is_bulk_appendable_ =
        std::ranges::contiguous_range<R> && std::ranges::sized_range<R> &&
        std::same_as<std::ranges::range_value_t<R>, std::byte>;

    /// \pre \a rg satisfies \c is_bulk_appendable_.
    template <typename R>
    requires is_bulk_appendable_<R>
    [[nodiscard]] static constexpr std::span<const std::byte> as_span_(R& rg)
    {
        return std::span{rg};
    }

    /// True if a \b contiguous range \a R is one this buffer may safely borrow and write through.
    /**
    * The range-borrowing constructors state \c std::ranges::contiguous_range<R> as their
    * template-parameter constraint and add this trait via \c requires.  That split is not
    * cosmetic: a \c constexpr \c bool variable template instantiates \e every operand of its
    * initializer (it is one expression, not a short-circuiting constraint), so \c range_value_t /
    * \c range_reference_t below would hard-error for a non-range \a R (e.g. the \c T* overload's
    * \c unsigned \c int*).  Concept conjunction on the template parameter \e does short-circuit,
    * so gating on \c contiguous_range there means this trait is only ever instantiated for actual
    * ranges, where those aliases are well-formed.  (A concept would read better still, but a
    * concept cannot be a class member.)
    *
    * Each remaining clause earns its place:
    *   - \c sized_range -- the byte size is known up front (\c std::ranges::size), so an unsized
    *     contiguous range is rejected cleanly rather than hard-erroring in the body.
    *   - not \c borrowed_byte_buffer itself -- \b required, not cosmetic: without it, constructing
    *     from a non-\c const \c borrowed_byte_buffer lvalue would prefer this template over the
    *     copy constructor (it binds a less-cv-qualified reference) and reinterpret the source's
    *     bytes.  The exclusion makes copy/move win.
    *   - trivially-copyable, non-\c const elements -- the object representation is what gets
    *     written and later read back, and the view writes through it.
    *   - a \c borrowed_range \b or an lvalue -- an rvalue owning container (e.g. a temporary
    *     \c std::vector) would leave a dangling view, so only non-owning rvalues (\c std::span)
    *     and lvalues are accepted.
    */
    template <typename R>
    static constexpr bool is_writable_borrow_ =
        std::ranges::sized_range<R> &&
        !std::same_as<std::remove_cvref_t<R>, borrowed_byte_buffer> &&
        std::is_trivially_copyable_v<std::ranges::range_value_t<R>> &&
        !std::is_const_v<std::remove_reference_t<std::ranges::range_reference_t<R>>> &&
        (std::ranges::borrowed_range<R> || std::is_lvalue_reference_v<R>);

    /// Byte size of a contiguous range accepted by \c is_writable_borrow_.
    template <typename R>
    requires is_writable_borrow_<R>
    [[nodiscard]] static constexpr std::size_t range_size_bytes_(R&& r) noexcept
    {
        return std::span{r}.size_bytes();
    }

    /// True if \a P is a pointer to a single writable, trivially-copyable object.
    /**
    * Constrains the single-object constructor, which takes a \e forwarding reference rather than
    * a plain \c T* on purpose: a \c T* parameter is (by partial ordering) more specialized than
    * the range constructor's \c R&&, so a C array would decay to it and overlay only its first
    * element.  Deducing \c P from the un-decayed argument and requiring \c std::is_pointer here
    * rejects arrays (they reach only the range constructor) while still accepting a genuine
    * single-object pointer such as \c &obj.  Unlike \c is_writable_borrow_, every trait below is
    * total, so this can be a plain \c bool variable template.
    */
    template <typename P>
    static constexpr bool is_object_ptr_ =
        std::is_pointer_v<std::remove_cvref_t<P>> &&
        std::is_trivially_copyable_v<std::remove_pointer_t<std::remove_cvref_t<P>>> &&
        !std::is_const_v<std::remove_pointer_t<std::remove_cvref_t<P>>>;

    /// The pointee size of a \c P accepted by \c is_object_ptr_ (the single-object capacity).
    template <typename P>
    requires is_object_ptr_<P>
    static constexpr std::size_t object_ptr_size_ =
        sizeof(std::remove_pointer_t<std::remove_cvref_t<P>>);

    /// Zero \a n bytes at \a p with stores the compiler must not optimize away.
    /**
    * Uses \c ::memset_explicit (C23) or \c explicit_bzero (glibc, BSDs) when the C library
    * declares one, else writes through a \c volatile pointer.  Neither has a feature-test
    * macro, so availability is probed by unqualified name lookup on the dependent parameter
    * \a P.
    *
    * \note The lookup must stay unqualified; do \b not "modernize" it to
    * \c std::memset_explicit.  libstdc++ 16 does not define that C++26 spelling at any
    * \c -std, and a qualified name into a namespace lacking the member is a hard error rather
    * than a substitution failure -- so the \c requires probe cannot reject it, and the build
    * fails outright instead of reaching the branches below.
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

    /// Borrow \a capacity bytes at \a data; the buffer starts empty (\c size()==0).
    /// \pre \a data points to at least \a capacity writable bytes.
    borrowed_byte_buffer(void* const data, const std::size_t capacity) noexcept
        : data_{static_cast<std::byte*>(data)}, capacity_{capacity}
    {}

    /// Borrow \a capacity bytes of the range \a r; the buffer starts empty (\c size()==0).
    /**
    * \a r is any contiguous, sized range of writable, trivially-copyable elements -- a
    * \c std::array, \c std::vector, \c std::span, C array, etc. (see \c is_borrowable_range_).
    * Its storage is overlaid, not copied.
    * \pre \a r's byte size is at least \a capacity.
    */
    template <std::ranges::contiguous_range R>
    requires is_writable_borrow_<R>
    explicit borrowed_byte_buffer(R&& r, const std::size_t capacity) noexcept
        : data_{reinterpret_cast<std::byte*>(std::ranges::data(r))}, capacity_{capacity}
    {
#if defined(DEBUG)
        assert(capacity_ <= range_size_bytes_(r));
#endif
    }

    /// Borrow all of the range \a r; capacity is its byte size, the buffer starts empty.
    /// \copydetails borrowed_byte_buffer(R&&, std::size_t)
    template <std::ranges::contiguous_range R>
    requires is_writable_borrow_<R>
    explicit borrowed_byte_buffer(R&& r) noexcept
        : data_{reinterpret_cast<std::byte*>(std::ranges::data(r))},
          capacity_{range_size_bytes_(r)}
    {}

    /// Overlay a single object \a data; capacity is the pointee's \c sizeof, the buffer starts
    /// empty.  Accepts a genuine object pointer (e.g. \c &obj), not an array (which the range
    /// constructor handles).
    template <typename P>
    requires is_object_ptr_<P>
    explicit borrowed_byte_buffer(P&& data) noexcept
        : data_{reinterpret_cast<std::byte*>(data)}, capacity_{object_ptr_size_<P>}
    {}

    /// Adopt bytes already present in the borrowed region: \c size()==capacity().
    /**
    * The \c adopting family mirrors the value constructors' source forms but starts the buffer
    * \e full, so \c span(), the iterators and \c operator== immediately see the bytes already in
    * the region -- for reading, iterating, or comparing memory that is already populated (a
    * header, a received packet, a key), rather than building into empty scratch.
    * \pre The source points to at least \c capacity() readable bytes.
    */
    [[nodiscard]] static borrowed_byte_buffer
    adopting(void* const data, const std::size_t capacity) noexcept
    {
        borrowed_byte_buffer b{data, capacity};
        b.size_ = b.capacity_;
        return b;
    }

    /// \copydoc adopting(void*,std::size_t)
    template <std::ranges::contiguous_range R>
    requires is_writable_borrow_<R>
    [[nodiscard]] static borrowed_byte_buffer adopting(R&& r, const std::size_t capacity) noexcept
    {
        borrowed_byte_buffer b{std::forward<R>(r), capacity};
        b.size_ = b.capacity_;
        return b;
    }

    /// \copydoc adopting(void*,std::size_t)
    template <std::ranges::contiguous_range R>
    requires is_writable_borrow_<R>
    [[nodiscard]] static borrowed_byte_buffer adopting(R&& r) noexcept
    {
        borrowed_byte_buffer b{std::forward<R>(r)};
        b.size_ = b.capacity_;
        return b;
    }

    /// \copydoc adopting(void*,std::size_t)
    template <typename P>
    requires is_object_ptr_<P>
    [[nodiscard]] static borrowed_byte_buffer adopting(P&& data) noexcept
    {
        borrowed_byte_buffer b{std::forward<P>(data)};
        b.size_ = b.capacity_;
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

    [[nodiscard]] constexpr std::size_t remaining_space() const noexcept
    {
        return capacity() - size();
    }

    [[nodiscard]] constexpr bool is_empty() const noexcept { return size() == 0; }

    [[nodiscard]] constexpr bool is_full() const noexcept { return size() == capacity(); }

    /// \note Does not zero the bytes: they stay in the borrowed region, readable through
    /// \c operator[] as the now-reserved tail.  \c clear() followed by
    /// \c zeroize_remaining_space() scrubs them.
    constexpr void clear() noexcept { size_ = 0; }

    /// Resize to \a count bytes
    /**
    * Growing sets the new bytes to \a value; shrinking leaves the removed ones unchanged.
    */
    constexpr void resize(const std::size_t count, const std::byte value)
    {
        if (count > capacity())
            throw std::bad_alloc{};

        if (count > size())
            std::memset(end(), std::to_integer<int>(value), count - size());

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
    /// \note "Emplace" is assignment here: the slot already holds a live byte, so this is
    /// equivalent to \c push_back(std::byte(args...)).
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
        if (size() != 0)
            std::memset(data(), std::to_integer<int>(value), size());
    }

    /// Zero the reserved tail [\c size(), \c capacity()); \c size() is unchanged.
    /**
    * Replaces the reserved bytes with zeros -- e.g. to pad to an alignment boundary before
    * reading whole SIMD lanes past \c size(), or to keep stale bytes from leaking through
    * beyond-size reads.  The stores happen even if nothing reads the tail afterward, so
    * \c clear() followed by this scrubs the whole borrowed region -- for sensitive contents,
    * where a plain \c memset is a dead store the optimizer may elide.
    */
    constexpr void zeroize_remaining_space() noexcept
    {
        if (remaining_space() != 0)
            zero_explicit_(static_cast<void*>(end()), remaining_space());
    }

    /// \pre \a spn does not overlap this buffer's storage.
    constexpr void append_range(const std::span<const std::byte> spn)
    {
        if (std::size(spn) > remaining_space())
            throw std::bad_alloc{};

        common_append_range_(spn);
    }

    /// \pre <code>[first, last)</code> is a valid range.  For a \c std::sized_sentinel_for this
    /// keeps <code>last - first</code> non-negative, so the size check's cast to \c std::size_t
    /// is well-defined.
    /// \note A \c std::sized_sentinel_for source is checked up front (all-or-nothing);
    /// otherwise the bytes that fit are appended before \c std::bad_alloc is thrown.
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
        append_range(std::span{std::data(il), std::size(il)});
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

    /// \pre \a spn does not overlap this buffer's storage.
    [[nodiscard]] constexpr bool try_append_range(const std::span<const std::byte> spn) noexcept
    {
        if (std::size(spn) > remaining_space())
            return false;

        common_append_range_(spn);
        return true;
    }

    /// \pre <code>[first, last)</code> is a valid range.  For a \c std::sized_sentinel_for this
    /// keeps <code>last - first</code> non-negative, so the size check's cast to \c std::size_t
    /// is well-defined.
    /// \note A \c std::sized_sentinel_for source is checked up front (nothing appended on
    /// \c false); otherwise the bytes that fit have already been appended when \c false is
    /// returned (observe \c size()).
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

    [[nodiscard]] constexpr bool
    try_append_range(const std::initializer_list<std::byte> il) noexcept
    {
        return try_append_range(std::span{std::data(il), std::size(il)});
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

            // The size check above covers every element, so skip the per-element repeat.
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

    /// \returns The borrowed pointer (raw; no alignment is assumed, unlike \c aligned_byte_buffer).
    [[nodiscard]] constexpr std::byte* data() noexcept { return data_; }

    /// \copydoc data()
    [[nodiscard]] constexpr const std::byte* data() const noexcept { return data_; }

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
    /// \note Unchecked and capacity-based: reading an index in [size(), capacity()) is valid
    /// but yields an unspecified (not indeterminate) byte.  \c at() is the bounds-checked
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
    /// \note The only bounds-checked accessor, and checked against \c size(), not
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

    /// \note Compares the live [0, \c size()) bytes by value (variable-time, per ordinary
    /// container semantics); use the free \c constant_time_equal for secret-dependent data.
    [[nodiscard]] constexpr bool operator==(const borrowed_byte_buffer& rhs) const noexcept
    {
        return std::ranges::equal(span(), rhs.span());
    }

    [[nodiscard]] constexpr std::strong_ordering
    operator<=>(const borrowed_byte_buffer& rhs) const noexcept
    {
        return std::lexicographical_compare_three_way(begin(), end(), rhs.begin(), rhs.end());
    }
};
