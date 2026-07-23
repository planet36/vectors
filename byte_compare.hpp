// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

/**
* \file
* \author Steven Ward
* \sa https://github.com/planet36/vectors
*
* Defines \c constant_time_equal, a timing-safe equality comparison of two \c std::byte spans,
* shared by the byte-buffer containers.
*/

#pragma once

#include <cstddef>
#include <span>

/// Constant-time equality comparison of two byte spans.
/**
* Runs in time dependent only on the spans' sizes, never on their contents: every byte pair is
* examined and the XORed differences are OR-accumulated, with no data-dependent branch or early
* exit.  Use this instead of \c operator== / \c std::memcmp when comparing secret-dependent data
* (e.g. verifying a MAC / authentication tag), where a first-mismatch early exit leaks the
* position of the first differing byte through timing.  Spans of unequal size compare unequal
* immediately; sizes are normally public (e.g. a fixed tag length).
*
* \note \c aligned_byte_buffer::operator== is deliberately variable-time, per ordinary container
* semantics, and does \b not use this.
* \note Branch-freedom is a property of this source, not one the language guarantees: nothing
* forbids a compiler from proving \c diff can only accumulate and exiting the loop early.  Note
* the asymmetry with \c zeroize_remaining_space(), which defeats the optimizer outright, whereas
* this relies on it declining a transformation it is permitted to make.  Verified for GCC 16 at
* \c -O3 \c -march=native: the loop vectorizes to a \c vpxor / \c vpor accumulation with a
* horizontal reduce, and every surviving conditional branch tests a size, not a content byte.
* Re-check if the compiler or flags change; should one ever short-circuit here, the fix is a
* barrier on \c diff (an empty \c asm volatile reading it, or a volatile accumulator), not a
* rewrite.
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
