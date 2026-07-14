# SPDX-FileCopyrightText: Steven Ward
# SPDX-License-Identifier: MPL-2.0

export LC_ALL = C

# Write the dep file next to the binary being built ($@.d) instead of letting the driver
# derive it from the source basename.  Both builds of one source would otherwise pick the
# same name, and the second to run would clobber the first's dep file.
DEPFLAGS = -MMD -MP -MF $@.d

#CPPFLAGS =

CXXFLAGS = -std=c++26
CXXFLAGS += -pipe -Wall -Wextra -Wpedantic -Wfatal-errors
CXXFLAGS += -Wno-unused-function

RELEASE_CXXFLAGS = -O3 -flto=auto
RELEASE_CXXFLAGS += -march=native

# DEBUG turns on the headers' precondition asserts.  -UNDEBUG keeps them on even if NDEBUG
# arrives from the environment's CPPFLAGS: assert() obeys NDEBUG, so without this a debug build
# could quietly check nothing.  It only wins because the recipe puts DEBUG_CXXFLAGS after
# CPPFLAGS -- -D/-U are applied in command-line order.
DEBUG_CXXFLAGS = -Og -ggdb3
DEBUG_CXXFLAGS += -DDEBUG -UNDEBUG

# _GLIBCXX_DEBUG is what checks the std::vector iterators the tests hand to the range APIs.
# That covers the one precondition the headers cannot assert for themselves -- "[first, last)
# is a valid range" -- which otherwise surfaces as a bogus std::bad_alloc from the capacity
# check, an invalid iterator having produced a garbage distance.  _GLIBCXX_ASSERTIONS is
# implied by it, kept explicit to say so.  SANITIZE_VECTOR only reaches the tests' own
# std::vector; the containers here have no unused capacity for it to poison.
DEBUG_CXXFLAGS += -D_GLIBCXX_ASSERTIONS -D_GLIBCXX_DEBUG -D_GLIBCXX_DEBUG_PEDANTIC
DEBUG_CXXFLAGS += -D_GLIBCXX_SANITIZE_VECTOR

# Fortifies the memcpy/memset that aligned_byte_buffer leans on.  Needs the -Og above: at -O0
# it warns and silently degrades to level 0.  It stays live under ASan (verified: level 3).
DEBUG_CXXFLAGS += -D_FORTIFY_SOURCE=3

# Cover what no assert can: the aligned heap block, and the byte buffer's reads of its
# uninitialized reserved tail.
DEBUG_CXXFLAGS += -fsanitize=address -fsanitize=undefined

#LDFLAGS =

#LDLIBS =

SRCS = $(wildcard *.cpp)
BINS = $(basename $(SRCS))
DEBUG_BINS = $(addsuffix .debug,$(BINS))
DEPS = $(addsuffix .d,$(BINS) $(DEBUG_BINS))

all: $(BINS) $(DEBUG_BINS)

# Must precede the match-anything rule below, which would otherwise take x.debug and look for
# x.debug.cpp.
# The built-in recipe for the implicit rule uses $^ instead of $<
%.debug: %.cpp
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) $(CXXFLAGS) $(DEBUG_CXXFLAGS) $(LDFLAGS) $< -o $@ $(LDLIBS)

%: %.cpp
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) $(CXXFLAGS) $(RELEASE_CXXFLAGS) $(LDFLAGS) $< -o $@ $(LDLIBS)

test: test-release test-debug

test-release: $(BINS)
	@set -e; for bin in $^; do ./$$bin; done

test-debug: $(DEBUG_BINS)
	@set -e; for bin in $^; do ./$$bin; done

clean:
	@$(RM) --verbose -- $(DEPS) $(BINS) $(DEBUG_BINS)

lint:
	-clang-tidy --quiet $(SRCS) -- $(CPPFLAGS) $(CXXFLAGS) $(RELEASE_CXXFLAGS)

# https://www.gnu.org/software/make/manual/make.html#Phony-Targets
.PHONY: all test test-release test-debug clean lint

# https://www.gnu.org/software/make/manual/html_node/Special-Targets.html#index-removing-targets-on-failure
.DELETE_ON_ERROR:

-include $(DEPS)
