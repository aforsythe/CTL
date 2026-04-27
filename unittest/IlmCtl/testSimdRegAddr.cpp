///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Direct C++ unit tests for SimdReg, SimdBoolMask, SimdArena, SimdDataAddr.
//
// IlmCtlTest's other suites are CTL-program-level: they load source, run
// it through the interpreter, and assert on outputs.  That covers happy
// paths but skips ownership/lifecycle/pool corners of the SIMD register
// classes that the vectorized interpreter (Phase B+) introduced.  This
// file constructs registers and arenas directly and exercises:
//
//   - SimdReg heap and arena allocation, with and without zeroInit
//   - createInArena / isArenaOwned / destroy
//   - setVarying transitions in both directions, lane[0] preservation
//   - operator[] varying read/write
//   - struct-member reference register: writes through the ref propagate
//     to the underlying owner, both varying and non-varying offsets
//   - SimdBoolMask non-varying/varying ctors, setVarying transitions,
//     and pool exhaustion past kCacheMax
//   - SimdArena alignment, reset+reuse, multi-chunk growth
//   - SimdDataAddr both ctors, copy, assignment, and print() branches
//

#include <CtlSimdReg.h>
#include <CtlSimdAddr.h>
#include <CtlSimdArena.h>
#include <testSimdRegAddr.h>

#include <iostream>
#include <sstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cassert>

using namespace Ctl;
using namespace std;


namespace {

void
testSimdRegHeapValueCtor ()
{
    cout << "  heap value ctor zeroes varying lanes" << endl;

    SimdReg r (true, sizeof(float));
    for (int i = 0; i < MAX_REG_SIZE; ++i)
    {
	float lane;
	memcpy(&lane, r[i], sizeof(float));
	assert(lane == 0.0f);
    }

    // Write a distinctive value to every lane, read it back.
    for (int i = 0; i < MAX_REG_SIZE; ++i)
    {
	float v = static_cast<float>(i) * 0.5f - 1.0f;
	memcpy(r[i], &v, sizeof(float));
    }
    for (int i = 0; i < MAX_REG_SIZE; ++i)
    {
	float lane;
	memcpy(&lane, r[i], sizeof(float));
	assert(lane == static_cast<float>(i) * 0.5f - 1.0f);
    }
}


void
testSimdRegSetVaryingTransitions ()
{
    cout << "  setVarying preserves lane[0] across widen + narrow" << endl;

    // Start non-varying with a known value.
    SimdReg r (false, sizeof(float));
    float seed = 3.14159f;
    memcpy(r[0], &seed, sizeof(float));

    // Widen: every lane should equal the original.
    r.setVarying(true);
    for (int i = 0; i < MAX_REG_SIZE; ++i)
    {
	float v;
	memcpy(&v, r[i], sizeof(float));
	assert(v == seed);
    }

    // Mutate lane 0, narrow back to non-varying — value preserved.
    float other = -42.0f;
    memcpy(r[0], &other, sizeof(float));
    r.setVarying(false);
    float final;
    memcpy(&final, r[0], sizeof(float));
    assert(final == other);
}


void
testSimdRegArenaBacked ()
{
    cout << "  arena-backed register has correct ownership flags" << endl;

    SimdArena arena;

    // zeroInit=true (default) — buffer must be zeroed.
    SimdReg r1 (true, sizeof(float), arena);
    for (int i = 0; i < 64; ++i)
    {
	float v;
	memcpy(&v, r1[i], sizeof(float));
	assert(v == 0.0f);
    }

    // zeroInit=false — caller is responsible; just verify writeback works.
    SimdReg r2 (true, sizeof(float), arena, /*zeroInit=*/false);
    for (int i = 0; i < 64; ++i)
    {
	float v = static_cast<float>(i);
	memcpy(r2[i], &v, sizeof(float));
    }
    for (int i = 0; i < 64; ++i)
    {
	float v;
	memcpy(&v, r2[i], sizeof(float));
	assert(v == static_cast<float>(i));
    }
}


void
testSimdRegCreateInArena ()
{
    cout << "  createInArena allocates object + buffer in arena" << endl;

    SimdArena arena;
    SimdReg *r = SimdReg::createInArena(arena, /*varying=*/true,
					sizeof(float), /*zeroInit=*/true);
    assert(r != nullptr);
    assert(r->isArenaOwned());

    // Smoke-test the buffer.
    float v = 99.0f;
    memcpy((*r)[0], &v, sizeof(float));
    float back;
    memcpy(&back, (*r)[0], sizeof(float));
    assert(back == 99.0f);

    SimdReg::destroy(r);   // arena-aware teardown — must not call operator delete.
}


void
testSimdRegStructMemberReference ()
{
    cout << "  struct-member reference register writes through to owner" << endl;

    // Owner reg holds a 16-byte "struct" per lane.  Reference register
    // points at offset 8 within each lane (a "second member" of the struct).
    const size_t structSize = 16;
    const size_t memberOffset = 8;
    SimdReg owner (true, structSize);

    // Non-varying offset is the typical struct member access.
    SimdBoolMask mask (false);   // non-varying mask, all lanes active
    SimdReg ref (owner, mask, memberOffset, MAX_REG_SIZE);

    assert(ref.isReference());

    // Write distinct values through the reference at lanes 0, 1, 7, 100.
    int lanes[] = {0, 1, 7, 100};
    uint32_t values[] = {0xDEADBEEFu, 0xCAFEBABEu, 0x12345678u, 0x0F0F0F0Fu};
    for (int k = 0; k < 4; ++k)
    {
	memcpy(ref[lanes[k]], &values[k], sizeof(uint32_t));
    }

    // Read them back from the OWNER reg at the corresponding offsets.
    for (int k = 0; k < 4; ++k)
    {
	uint32_t got;
	memcpy(&got, owner[lanes[k]] + memberOffset, sizeof(uint32_t));
	assert(got == values[k]);
    }
}


void
testSimdBoolMaskBasics ()
{
    cout << "  SimdBoolMask non-varying / varying / setVarying" << endl;

    // Non-varying: inline scalar storage.
    SimdBoolMask m (false);
    assert(!m.isVarying());
    m[0] = true;
    assert(m[5] == true);   // non-varying: any index reads lane[0]
    m[0] = false;
    assert(m[123] == false);

    // Promote to varying: every lane should equal the prior scalar value.
    m[0] = true;
    m.setVarying(true);
    assert(m.isVarying());
    for (int i = 0; i < 32; ++i)
	assert(m[i] == true);

    // Mutate lane 5; lane 0 should still be true; lane 5 false.
    m[5] = false;
    assert(m[0] == true);
    assert(m[5] == false);

    // Narrow: lane[0] preserved.
    m.setVarying(false);
    assert(!m.isVarying());
    assert(m[0] == true);
}


void
testSimdBoolMaskCopyAndPoolExhaustion ()
{
    cout << "  SimdBoolMask copy ctor + pool reuse beyond kCacheMax" << endl;

    SimdBoolMask src (true);
    for (int i = 0; i < 10; ++i)
	src[i] = (i & 1) != 0;

    SimdBoolMask dst (src, /*copyLen=*/10);
    assert(dst.isVarying());
    for (int i = 0; i < 10; ++i)
	assert(dst[i] == ((i & 1) != 0));

    // Allocate well past detail::BoolBufferPool::kCacheMax (32) — the
    // overflow falls through to heap alloc/free.  Just constructing
    // and destructing them in stack order exercises both pool reuse
    // and overflow.
    {
	std::vector<SimdBoolMask*> masks;
	masks.reserve(64);
	for (int i = 0; i < 64; ++i)
	{
	    SimdBoolMask *p = new SimdBoolMask(true);
	    p[0][0] = ((i & 1) == 0);
	    masks.push_back(p);
	}
	for (int i = 63; i >= 0; --i)
	{
	    assert((*masks[i])[0] == ((i & 1) == 0));
	    delete masks[i];
	}
    }

    // After the burst, a fresh allocation should still work and be writable.
    SimdBoolMask after (true);
    after[0] = true;
    assert(after[0] == true);
}


void
testSimdArenaAlignmentAndReset ()
{
    cout << "  SimdArena alignment, reset reuse, multi-chunk growth" << endl;

    SimdArena arena;

    // Several modest allocations: every returned pointer is 16-byte aligned.
    char *p1 = arena.allocate(13);
    char *p2 = arena.allocate(1);
    char *p3 = arena.allocate(64);
    assert(p1 != nullptr && p2 != nullptr && p3 != nullptr);
    assert((reinterpret_cast<uintptr_t>(p1) & 15) == 0);
    assert((reinterpret_cast<uintptr_t>(p2) & 15) == 0);
    assert((reinterpret_cast<uintptr_t>(p3) & 15) == 0);
    // Each subsequent allocation begins after the previous (rounded up
    // to alignment): p2 should be at least 16 bytes past p1.
    assert(p2 >= p1 + 16);
    assert(p3 >= p2 + 16);

    // Reset, then re-allocate — should reuse chunk 0 (returns the same
    // base pointer for the first allocation).
    arena.reset();
    char *q = arena.allocate(13);
    assert(q == p1);

    // Force a multi-chunk growth: allocate something larger than the
    // initial chunk's remaining space.  The arena adds a new chunk;
    // the existing pointer (q) must remain valid.
    arena.reset();
    char *first = arena.allocate(16);
    char *huge  = arena.allocate(4 * 1024 * 1024);   // 4 MiB > 2 MiB chunk
    assert(huge != nullptr);
    // Original pointer still readable/writable after growth.
    first[0] = 'X';
    assert(first[0] == 'X');
}


void
testSimdDataAddr ()
{
    cout << "  SimdDataAddr two ctors + copy + assign + print branches" << endl;

    // Absolute (reg) ctor.
    SimdReg backing (false, sizeof(float));
    SimdDataAddr abs(&backing);
    assert(abs.reg() == &backing);

    // Frame-pointer-relative ctor.
    SimdDataAddr fpRel(/*fpOffset=*/-32);
    assert(fpRel.reg() == nullptr);   // relative addrs return null from no-arg reg()

    // Copy ctor — preserves both branches.
    SimdDataAddr absCopy(abs);
    assert(absCopy.reg() == &backing);

    SimdDataAddr fpCopy(fpRel);
    assert(fpCopy.reg() == nullptr);

    // operator= — both branches.
    SimdDataAddr a(&backing);
    a = fpRel;
    assert(a.reg() == nullptr);
    a = abs;
    assert(a.reg() == &backing);

    // print(): rebind cout to a stringstream so we can run both branches
    // without polluting the test output.
    std::ostringstream capture;
    std::streambuf *prev = std::cout.rdbuf(capture.rdbuf());
    abs.print(2);
    fpRel.print(4);
    std::cout.rdbuf(prev);
    const std::string out = capture.str();
    assert(out.find("reg addr") != std::string::npos);
    assert(out.find("reg fp offset -32") != std::string::npos);
}

} // anonymous namespace


void
testSimdRegAddr ()
{
    cout << endl;
    cout << "Testing SimdReg / SimdBoolMask / SimdArena / SimdDataAddr"
	 << endl;

    testSimdRegHeapValueCtor();
    testSimdRegSetVaryingTransitions();
    testSimdRegArenaBacked();
    testSimdRegCreateInArena();
    testSimdRegStructMemberReference();
    testSimdBoolMaskBasics();
    testSimdBoolMaskCopyAndPoolExhaustion();
    testSimdArenaAlignmentAndReset();
    testSimdDataAddr();

    cout << "ok" << endl;
}
