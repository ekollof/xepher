# Check the selected compiler AND standard library before building dependencies.
# A newer Clang can still select older system libstdc++ headers.
include(CheckCXXSourceCompiles)

check_cxx_source_compiles([=[
    #include <algorithm>
    #include <array>
    #include <expected>
    #include <iterator>
    #include <ranges>
    #include <span>
    #include <string>
    #include <vector>

    static_assert(__cplusplus > 202002L, "Xepher requires C++23 or newer");

    int main()
    {
        std::expected<int, std::string> value = 1;
        const std::vector<int> input{1, 2, 3};
        auto selected = input
            | std::views::filter([](int n) { return n > 1; })
            | std::views::transform([](int n) { return n * 2; });
        std::vector<int> output;
        std::ranges::copy(selected, std::back_inserter(output));
        const std::span<const int> view{output};
        const std::string text = "xepher";
        static_assert(std::ranges::contains(std::array{1, 2, 3}, 2));
        auto numbered = input | std::views::enumerate;
        auto chunks = input | std::views::chunk(2);
        (void)numbered;
        (void)chunks;
        return value.value_or(0) == 1 && view.size() == 2 && text.contains("x") ? 0 : 1;
    }
]=] XEPHER_HAS_REQUIRED_CXX23_FEATURES)

if(NOT XEPHER_HAS_REQUIRED_CXX23_FEATURES)
    message(FATAL_ERROR
        "Xepher requires C++23 compiler/library support for std::expected, ranges, "
        "std::span, std::string::contains, std::ranges::contains, views::enumerate "
        "and views::chunk. The selected compiler is "
        "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER}). "
        "On Linux with libstdc++, use Clang 19+ and GCC 13+ C++ development headers "
        "and library. Updating Clang alone may leave old standard-library headers "
        "selected. See README.md: C++ toolchain requirements. Configure in a new "
        "build directory after changing the toolchain.")
endif()
