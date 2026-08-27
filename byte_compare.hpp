// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

/**
* \file
* \author Steven Ward
*
* Defines \c equal_constant_time, an equality comparison of two \c std::byte spans with no
* early exit on the first difference.
* Both \c aligned_byte_buffer.hpp and \c borrowed_byte_buffer.hpp include it.
*/

#pragma once

#include <cstddef>
#include <span>

/// Compare two byte spans of equal size with no early exit on the first difference
/**
* Use this in place of \c operator== or \c std::memcmp when either operand is secret.
*
* Every byte is examined whatever the contents.  The time to compare therefore does not
* reveal how many leading bytes matched.
*
* Spans of unequal size compare unequal immediately, so a difference in length is not
* concealed.  This function is for data whose length is not secret.
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
