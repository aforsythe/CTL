///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_TEST_REQUIRE_H
#define INCLUDED_TEST_REQUIRE_H

#include <cstdio>
#include <cstdlib>

// REQUIRE(cond) — runtime check that survives Release/NDEBUG builds.
//
// CI builds the project under -DCMAKE_BUILD_TYPE=Release (PGO etc.),
// which #defines NDEBUG and makes <cassert>'s assert() expand to (void)0.
// That silently neutralises every test in this directory that relies on
// assert(), turning them into "did the binary crash?" checks rather than
// "did the assertion hold?" checks.  Mutation testing surfaced this:
// mutations that change observable behaviour passed every assert()-only
// test in Release.
//
// REQUIRE() always evaluates and aborts on failure, regardless of NDEBUG.
//
#define REQUIRE(cond)                                                   \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr,                                        \
                "REQUIRE failed: %s\n  at %s:%d\n",                     \
                #cond, __FILE__, __LINE__);                             \
            std::abort();                                               \
        }                                                               \
    } while (0)

#endif
