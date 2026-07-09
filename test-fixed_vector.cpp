/*

gpp $OPTIMIZE_OPTIONS -I ../include test-fixed_vector.cpp && d ./a.out

clear ; ./a.out

*/

#include "fixed_vector.hpp"

#include <cassert>
#include <fmt/ranges.h>
#include <ranges>
#include <vector>

template <typename T, std::size_t N>
void
print_fixed_vector(const fixed_vector<T, N>& vec)
{
    fmt::println("fixed_vector: span={}  size={}  remaining_space={}  max_size/capacity={}  is_empty={}  is_full={}  sizeof={}",
            vec.span(),
            vec.size(),
            vec.remaining_space(),
            vec.max_size(),
            vec.is_empty(),
            vec.is_full(),
            sizeof(vec));
}

// Compile-time check: zeroize_remaining_space() is usable in constant expressions (the
// runtime explicit-zeroing path is replaced by value-assignment during constant evaluation).
static_assert([] {
    fixed_vector<int, 5> v;
    v.fill_capacity(9);
    v.resize(2); // the tail slots [2, 5) still hold 9
    v.zeroize_remaining_space();
    // operator[] is capacity-based: the tail is now zero
    return v.size() == 2 && v[0] == 9 && v[1] == 9 && v[2] == 0 && v[3] == 0 && v[4] == 0;
}());

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    using namespace std::literals;

    {
        fmt::println("\n# Test default constructor");
        const fixed_vector<int, 5> vec;
        print_fixed_vector(vec);
    }

    {
        fmt::println("\n# Test count constructor");
        const fixed_vector<int, 5> vec(3);
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({0, 0, 0}));
    }

    {
        fmt::println("\n# Test count and value constructor");
        const fixed_vector<int, 5> vec(3, 42);
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({42, 42, 42}));
    }

    {
        fmt::println("\n# Test std::span constructor");
        constexpr std::array arr{1, 2, 3};
        const fixed_vector<int, 5> vec(std::span<const int>{arr});
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# Test iterator range constructor");
        constexpr std::array arr{1, 2, 3};
        const fixed_vector<int, 5> vec(std::cbegin(arr), std::cend(arr));
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# Test iterator and count constructor");
        constexpr std::array arr{1, 2, 3};
        const fixed_vector<int, 5> vec(std::cbegin(arr), std::size(arr));
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# Test initializer list constructor");
        const fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# Test range constructor");
        const auto rg = std::views::iota(1, 4);
        const fixed_vector<int, 5> vec(std::from_range, rg);
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# Test initializer list assignment operator");
        const auto il = {1, 2, 3};
        fixed_vector<int, 5> vec;
        vec = il;
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# Test static max_size() function");
        const fixed_vector<int, 5> vec;
        assert(vec.max_size() == 5);
    }

    {
        fmt::println("\n# Test size() function");
        const fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        assert(vec.size() == 3);
    }

    {
        fmt::println("\n# Test remaining_space() function");
        const fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        assert(vec.remaining_space() == 2);
    }

    {
        fmt::println("\n# Test is_empty() function");
        const fixed_vector<int, 5> vec;
        print_fixed_vector(vec);
        assert(vec.is_empty());
    }

    {
        fmt::println("\n# Test is_full() function");
        const fixed_vector<int, 3> vec{1, 2, 3};
        print_fixed_vector(vec);
        assert(vec.is_full());
    }

    {
        fmt::println("\n# Test clear() function");
        fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        vec.clear();
        print_fixed_vector(vec);
        assert(vec.is_empty());
    }

    {
        fmt::println("\n# Test pop_back() function");
        fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        vec.pop_back();
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1, 2}));
    }

    {
        fmt::println("\n# Test push_back() function");
        fixed_vector<int, 5> vec;
        print_fixed_vector(vec);
        vec.push_back(1);
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1}));
        vec.push_back(2);
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1, 2}));
    }

    {
        fmt::println("\n# Test fill_capacity() function");
        fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        vec.fill_capacity(42);
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({42, 42, 42, 42, 42}));
        assert(vec.is_full());
    }

    {
        fmt::println("\n# Test fill_size() function");
        fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        vec.fill_size(42);
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({42, 42, 42}));
        assert(!vec.is_full());
    }

    {
        fmt::println("\n# Test zeroize_remaining_space() function");
        fixed_vector<int, 5> vec{1, 2, 3};
        vec.fill_capacity(9);
        vec.resize(2); // the tail slots [2, 5) still hold 9
        print_fixed_vector(vec);
        vec.zeroize_remaining_space();
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({9, 9}));
        // operator[] is capacity-based: the tail is now zero
        for (std::size_t i = vec.size(); i < vec.max_size(); ++i)
            assert(vec[i] == 0);
    }

    {
        fmt::println("\n# Test append_range(std::span<T>) function");
        fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        constexpr std::array arr{4, 5};
        vec.append_range(std::span<const int>{arr});
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1, 2, 3, 4, 5}));
    }

    {
        fmt::println("\n# Test append_range(InputIterator, std::size_t) function");
        fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        constexpr std::array arr{4, 5};
        vec.append_range(std::begin(arr), std::size(arr));
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1, 2, 3, 4, 5}));
    }

    {
        fmt::println("\n# Test append_range(InputIterator, InputIterator) function");
        fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        constexpr std::array arr{4, 5};
        vec.append_range(std::begin(arr), std::end(arr));
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1, 2, 3, 4, 5}));
    }

    {
        fmt::println("\n# Test append_range(std::initializer_list) function");
        fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        vec.append_range({4, 5});
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1, 2, 3, 4, 5}));
    }

    {
        fmt::println("\n# Test append_range(range) function");
        const auto rg = std::views::iota(4, 6);
        fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        vec.append_range(rg);
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1, 2, 3, 4, 5}));
    }

    {
        fmt::println("\n# Test assign_range(std::span<T>) function");
        fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        constexpr std::array arr{4, 5};
        vec.assign_range(std::span<const int>{arr});
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({4, 5}));
    }

    {
        fmt::println("\n# Test assign_range(InputIterator, std::size_t) function");
        fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        constexpr std::array arr{4, 5};
        vec.assign_range(std::begin(arr), std::size(arr));
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({4, 5}));
    }

    {
        fmt::println("\n# Test assign_range(InputIterator, InputIterator) function");
        fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        constexpr std::array arr{4, 5};
        vec.assign_range(std::begin(arr), std::end(arr));
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({4, 5}));
    }

    {
        fmt::println("\n# Test assign_range(std::initializer_list) function");
        fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        vec.assign_range({4, 5});
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({4, 5}));
    }

    {
        fmt::println("\n# Test assign_range(range) function");
        const auto rg = std::views::iota(4, 6);
        fixed_vector<int, 5> vec{1, 2, 3};
        print_fixed_vector(vec);
        vec.assign_range(rg);
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({4, 5}));
    }

    {
        fmt::println("\n# Test span() function");
        const fixed_vector<int, 5> vec{1, 2, 3, 4};
        print_fixed_vector(vec);
        const auto spn = vec.span();
        fmt::println("spn = {}", spn);
        assert(std::vector(std::from_range, vec.span()) == std::vector(std::from_range, spn));
    }

    {
        fmt::println("\n# Test exception in push_back() function when array is full");
        fixed_vector<int, 3> vec{1, 2, 3};
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1, 2, 3}));
        try {
            vec.push_back(4);
        } catch (const std::exception& ex) {
            fmt::println("Caught exception: {}", ex.what());
        }
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1, 2, 3}));
    }

    {
        fmt::println("\n# Test exception in append_range(std::span<T>) when span exceeds capacity");
        fixed_vector<int, 3> vec{1, 2, 3};
        print_fixed_vector(vec);
        try {
            vec.append_range({4, 5});
        } catch (const std::exception& ex) {
            fmt::println("Caught exception: {}", ex.what());
        }
        print_fixed_vector(vec);
        assert(std::vector(std::from_range, vec.span()) == std::vector({1, 2, 3}));
    }

    return 0;
}

/*

Output:

# Test default constructor
fixed_vector: span=[]  size=0  remaining_space=5  max_size/capacity=5  is_empty=true  is_full=false  sizeof=32

# Test count constructor
fixed_vector: span=[0, 0, 0]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test count and value constructor
fixed_vector: span=[42, 42, 42]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test std::span constructor
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test iterator range constructor
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test iterator and count constructor
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test initializer list constructor
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test range constructor
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test initializer list assignment operator
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test static max_size() function

# Test size() function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test remaining_space() function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test is_empty() function
fixed_vector: span=[]  size=0  remaining_space=5  max_size/capacity=5  is_empty=true  is_full=false  sizeof=32

# Test is_full() function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=0  max_size/capacity=3  is_empty=false  is_full=true  sizeof=24

# Test clear() function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
fixed_vector: span=[]  size=0  remaining_space=5  max_size/capacity=5  is_empty=true  is_full=false  sizeof=32

# Test pop_back() function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
fixed_vector: span=[1, 2]  size=2  remaining_space=3  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test push_back() function
fixed_vector: span=[]  size=0  remaining_space=5  max_size/capacity=5  is_empty=true  is_full=false  sizeof=32
fixed_vector: span=[1]  size=1  remaining_space=4  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
fixed_vector: span=[1, 2]  size=2  remaining_space=3  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test fill_capacity() function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
fixed_vector: span=[42, 42, 42, 42, 42]  size=5  remaining_space=0  max_size/capacity=5  is_empty=false  is_full=true  sizeof=32

# Test fill_size() function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
fixed_vector: span=[42, 42, 42]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test zeroize_remaining_space() function
fixed_vector: span=[9, 9]  size=2  remaining_space=3  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
fixed_vector: span=[9, 9]  size=2  remaining_space=3  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test append_range(std::span<T>) function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
fixed_vector: span=[1, 2, 3, 4, 5]  size=5  remaining_space=0  max_size/capacity=5  is_empty=false  is_full=true  sizeof=32

# Test append_range(InputIterator, std::size_t) function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
fixed_vector: span=[1, 2, 3, 4, 5]  size=5  remaining_space=0  max_size/capacity=5  is_empty=false  is_full=true  sizeof=32

# Test append_range(InputIterator, InputIterator) function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
fixed_vector: span=[1, 2, 3, 4, 5]  size=5  remaining_space=0  max_size/capacity=5  is_empty=false  is_full=true  sizeof=32

# Test append_range(std::initializer_list) function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
fixed_vector: span=[1, 2, 3, 4, 5]  size=5  remaining_space=0  max_size/capacity=5  is_empty=false  is_full=true  sizeof=32

# Test append_range(range) function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
fixed_vector: span=[1, 2, 3, 4, 5]  size=5  remaining_space=0  max_size/capacity=5  is_empty=false  is_full=true  sizeof=32

# Test assign_range(std::span<T>) function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
fixed_vector: span=[4, 5]  size=2  remaining_space=3  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test assign_range(InputIterator, std::size_t) function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
fixed_vector: span=[4, 5]  size=2  remaining_space=3  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test assign_range(InputIterator, InputIterator) function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
fixed_vector: span=[4, 5]  size=2  remaining_space=3  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test assign_range(std::initializer_list) function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
fixed_vector: span=[4, 5]  size=2  remaining_space=3  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test assign_range(range) function
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=2  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
fixed_vector: span=[4, 5]  size=2  remaining_space=3  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32

# Test span() function
fixed_vector: span=[1, 2, 3, 4]  size=4  remaining_space=1  max_size/capacity=5  is_empty=false  is_full=false  sizeof=32
spn = [1, 2, 3, 4]

# Test exception in push_back() function when array is full
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=0  max_size/capacity=3  is_empty=false  is_full=true  sizeof=24
Caught exception: std::bad_alloc
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=0  max_size/capacity=3  is_empty=false  is_full=true  sizeof=24

# Test exception in append_range(std::span<T>) when span exceeds capacity
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=0  max_size/capacity=3  is_empty=false  is_full=true  sizeof=24
Caught exception: std::bad_alloc
fixed_vector: span=[1, 2, 3]  size=3  remaining_space=0  max_size/capacity=3  is_empty=false  is_full=true  sizeof=24

*/
