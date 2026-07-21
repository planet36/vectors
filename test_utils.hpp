// SPDX-FileCopyrightText: Steven Ward
// SPDX-License-Identifier: MPL-2.0

/// Helpers shared by the test-*.cpp programs.
/**
* The pass/fail contract is the exit status alone: a passing program prints nothing and exits
* EXIT_SUCCESS.  The first failed check prints one line to stderr and exits EXIT_FAILURE
* immediately, leaving the remaining checks unrun.
*
* Nothing here calls \c abort(), which is why \c assert is not used: a failing test must not dump
* core.  For the same reason \c run_tests catches everything, so an exception that escapes a test
* cannot reach \c terminate().
*/

#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <print>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

/// Report a failed check at \a file : \a line within \a func and exit EXIT_FAILURE.
[[noreturn]] inline void
test_fail(const std::string_view file, const int line, const std::string_view func,
          const std::string_view msg)
{
    std::println(stderr, "{}:{}: {}: {}", file, line, func, msg);
    std::exit(EXIT_FAILURE);
}

/// Fail unless the expression is true.
/**
* Variadic so that an expression containing an unparenthesized comma -- e.g.
* \c CHECK(fixed_vector<int,5>::capacity()==5) -- does not need the extra parens \c assert
* required.
*/
#define CHECK(...)                                                                  \
    do {                                                                            \
        if (!(__VA_ARGS__))                                                         \
            test_fail(__FILE__, __LINE__, __func__, "CHECK failed: " #__VA_ARGS__); \
    } while (false)

/// Fail unless evaluating the expression throws an exception of type \a Ex.
/**
* Reports "no exception" and "wrong exception type" distinctly: a throw of the wrong type is a
* different defect from no throw at all, and catching it here (rather than letting it escape) is
* what keeps the run from reaching \c terminate().
*/
#define CHECK_THROWS(Ex, ...)                                                        \
    do {                                                                             \
        try                                                                          \
        {                                                                            \
            __VA_ARGS__;                                                             \
            test_fail(__FILE__, __LINE__, __func__,                                  \
                      "CHECK_THROWS failed: no exception thrown, expected " #Ex      \
                      " from: " #__VA_ARGS__);                                       \
        }                                                                            \
        catch (const Ex&)                                                            \
        {                                                                            \
        }                                                                            \
        catch (...)                                                                  \
        {                                                                            \
            test_fail(__FILE__, __LINE__, __func__,                                  \
                      "CHECK_THROWS failed: wrong exception type, expected " #Ex     \
                      " from: " #__VA_ARGS__);                                       \
        }                                                                            \
    } while (false)

/// Run \a tests and return the value \c main should return.
/**
* An exception propagating out of \c main would call \c terminate -> \c abort and dump core; report
* it and exit non-zero instead.
*/
template <std::invocable Fn>
[[nodiscard]] int
run_tests(Fn&& tests)
{
    try
    {
        std::forward<Fn>(tests)();
        return EXIT_SUCCESS;
    }
    catch (const std::exception& ex)
    {
        std::println(stderr, "unexpected exception propagated: {}", ex.what());
    }
    catch (...)
    {
        std::println(stderr, "unexpected exception of unknown type propagated");
    }
    return EXIT_FAILURE;
}

/// The live [0, size()) elements of \a v as a std::vector<int>, for comparison with an expected
/// list.
/**
* std::byte does not implicitly convert to int, so it is unpacked with \c std::to_integer.
*/
template <typename V>
[[nodiscard]] std::vector<int>
to_ivec(const V& v)
{
    const auto as_int = [](const auto& x)
    {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(x)>, std::byte>)
            return std::to_integer<int>(x);
        else
            return static_cast<int>(x);
    };
    return std::vector<int>(std::from_range, v.span() | std::views::transform(as_int));
}

/// A std::byte with the value \a i.
[[nodiscard]] constexpr std::byte
to_byte(const int i) noexcept
{
    return static_cast<std::byte>(i);
}

/// A std::byte literal, e.g. 0xAB_b.
[[nodiscard]] constexpr std::byte
operator""_b(const unsigned long long v) noexcept
{
    return static_cast<std::byte>(v);
}

/// True if \a p is aligned to \a align bytes.
[[nodiscard]] inline bool
is_aligned(const void* const p, const std::size_t align) noexcept
{
    return reinterpret_cast<std::uintptr_t>(p) % align == 0;
}

/// True if the heap-backed containers' class invariant holds for \a v: \c data() is null
/// exactly when \c capacity() is 0.
/**
* A predicate rather than a CHECK of its own, so that a failure reports the caller's line --
* the point is *which state* broke the invariant.
*/
template <typename V>
[[nodiscard]] bool
data_null_iff_empty(const V& v) noexcept
{
    // Deliberately called on moved-from objects to check the class invariant survives the move.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
    return (v.data() == nullptr) == (v.capacity() == 0);
}
