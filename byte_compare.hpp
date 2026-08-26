// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

/**
* \file
* \author Steven Ward
*
* Defines \c equal_constant_time, a timing-safe equality comparison of two \c std::byte spans.
* Both \c aligned_byte_buffer.hpp and \c borrowed_byte_buffer.hpp include it.
*/

#pragma once

#include <cstddef>
#include <span>

/// Compare two byte spans without an early exit on the first difference (for equal lengths)
/**
* Every byte is examined whatever the contents, so a verifier is not usable as a timing oracle
* for an expected digest.  That matters when the digest is a MAC.
*
* C++ cannot express a timing guarantee.  \c diff is \c volatile so the compiler must perform
* every accumulation, in order, rather than stop at the first difference.
*/
[[nodiscard]] inline bool
equal_constant_time(const std::span<const std::byte> a,
                    const std::span<const std::byte> b) noexcept
{
    if (std::size(a) != std::size(b))
        return false;

    volatile unsigned int diff = 0;

    for (std::size_t i = 0; i < std::size(a); ++i)
    {
        diff |= std::to_integer<unsigned int>(a[i] ^ b[i]);
    }

    return diff == 0;
}
