# SPDX-FileCopyrightText: Steven Ward
# SPDX-License-Identifier: MPL-2.0

export LC_ALL = C

# Write the dep file next to the binary being built ($@.d) instead of letting the driver
# derive it from the source basename.  Both builds of one source would otherwise pick the
# same name, and the second to run would clobber the first's dep file.
DEPFLAGS = -MMD -MP -MF $@.d

CPPFLAGS = -I ./include

CXXFLAGS = -std=c++23
CXXFLAGS += -pipe -Wall -Wextra -Wpedantic -Wfatal-errors
CXXFLAGS += -Wno-unused-function

RELEASE_CXXFLAGS = -O3 -flto=auto
RELEASE_CXXFLAGS += -march=native

# DEBUG turns on the headers' precondition asserts.  -UNDEBUG guards them: assert() obeys
# NDEBUG, so an NDEBUG reaching this build would disable every assert while the debug build
# still looked like it worked.  The -U only wins because the recipe puts DEBUG_CXXFLAGS after
# CPPFLAGS, and -D/-U are applied in command-line order, so do not reorder them.
DEBUG_CXXFLAGS = -Og -ggdb3
DEBUG_CXXFLAGS += -DDEBUG -UNDEBUG

# _GLIBCXX_DEBUG is what checks the std::vector iterators the tests hand to the range APIs.
# That covers the one precondition the headers cannot assert for themselves, that [first, last)
# is a valid range.  A violation otherwise surfaces as a bogus std::bad_alloc from the capacity
# check, because an invalid iterator produces a garbage distance.  _GLIBCXX_ASSERTIONS is
# implied by it and is kept explicit to say so.
DEBUG_CXXFLAGS += -D_GLIBCXX_ASSERTIONS -D_GLIBCXX_DEBUG -D_GLIBCXX_DEBUG_PEDANTIC

# This reaches only the tests' own std::vector, since the containers here have no unused
# capacity for it to poison.
DEBUG_CXXFLAGS += -D_GLIBCXX_SANITIZE_VECTOR

# This fortifies the memcpy/memset that the byte buffers lean on.  It needs the -Og above,
# because at -O0 it warns and silently degrades to level 0.  It stays live under ASan (verified
# at level 3).
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

# This rule must precede the match-anything rule below, which would otherwise take x.debug and
# look for x.debug.cpp.
#
# Both recipes use $< where the built-in recipe for the implicit rule uses $^, which would also
# pass along the headers that the dep files add as prerequisites.
%.debug: %.cpp
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) $(CXXFLAGS) $(DEBUG_CXXFLAGS) $(LDFLAGS) $< -o $@ $(LDLIBS)

%: %.cpp
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) $(CXXFLAGS) $(RELEASE_CXXFLAGS) $(LDFLAGS) $< -o $@ $(LDLIBS)

test: $(BINS) $(DEBUG_BINS)
	@set -e; for bin in $^; do ./$$bin; done

clean:
	@$(RM) --verbose -- $(DEPS) $(BINS) $(DEBUG_BINS)

lint:
	-clang-tidy --quiet $(SRCS) -- $(CPPFLAGS) $(CXXFLAGS) $(RELEASE_CXXFLAGS)

# https://www.gnu.org/software/make/manual/make.html#Phony-Targets
.PHONY: all test clean lint

# https://www.gnu.org/software/make/manual/html_node/Special-Targets.html#index-removing-targets-on-failure
.DELETE_ON_ERROR:

-include $(DEPS)
