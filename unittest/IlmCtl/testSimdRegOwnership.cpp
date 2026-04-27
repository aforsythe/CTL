///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Tests for the four documented ownership states of SimdReg, and the
// transitions between them — particularly the transferData=true paths
// where ownership of the data buffer is moved from one register to
// another.  Ownership bugs in this area are a classic source of double-
// free / use-after-free in vectorized interpreters; testSimdRegAddr
// covers the value-register and non-transfer reference-register cases,
// but the transferData=true paths and the resulting "transferred-out"
// state were uncovered.
//
// Documented states (from CtlSimdReg.h):
//
//  1) Value register: _ref=0, _data=own buffer, _offsets=zeroOffset
//  2) Reference into another register: _ref=other, _data=0, _offsets=own
//  3) Reference register that absorbed a temporary's data via
//     transferData=true: _ref=this, _data=transferred buffer, _offsets=own
//  4) Transferred-out: _data=0; can ONLY be safely destroyed
//
// Tests below construct each state, exercise the documented operations,
// and destroy in different orders to verify no double-free / leak / UAF.
//

#include <CtlSimdReg.h>
#include <testSimdRegOwnership.h>
#include <testRequire.h>

#include <iostream>
#include <cstring>
#include <cstdint>

using namespace Ctl;
using namespace std;


namespace {

// Helper: write a known per-lane sentinel into a varying value reg.
// Useful so we can later verify the same buffer is observed through
// a reference (transfer or otherwise).
void
fillSentinel (SimdReg &r, uint32_t base)
{
    // Treat each lane as a uint32_t.  Element size is sizeof(uint32_t).
    for (int i = 0; i < MAX_REG_SIZE; ++i)
    {
	const uint32_t v = base + static_cast<uint32_t>(i);
	memcpy(r[i], &v, sizeof(uint32_t));
    }
}


void
testNonTransferReferenceLifetimes ()
{
    cout << "  state 1 + state 2: ref into static survives owner up to its dtor"
	 << endl;

    // State 1: build an owned value register.  Sentinel data lets us
    // catch bugs that would zero the buffer or copy from elsewhere.
    SimdReg owner (true, sizeof(uint32_t));
    fillSentinel(owner, 0xCAFE0000);

    // State 2: build a reference into `owner` via the struct-member ctor
    // with offset=0 and transferData=false (default).  The ref does NOT
    // own data; it indirects through owner.
    {
	SimdBoolMask mask (false);
	SimdReg ref (owner, mask, /*offset=*/0, MAX_REG_SIZE);
	REQUIRE(ref.isReference());
	REQUIRE(!ref.isArenaOwned());

	// Reading through the ref returns the same per-lane sentinel.
	for (int i = 0; i < MAX_REG_SIZE; i += 1024)   // sample a few lanes
	{
	    uint32_t got;
	    memcpy(&got, ref[i], sizeof(uint32_t));
	    REQUIRE(got == 0xCAFE0000u + static_cast<uint32_t>(i));
	}

	// Writing through the ref propagates back to the owner.
	uint32_t marker = 0xDEAFBEEF;
	memcpy(ref[42], &marker, sizeof(uint32_t));
	uint32_t echo;
	memcpy(&echo, owner[42], sizeof(uint32_t));
	REQUIRE(echo == 0xDEAFBEEFu);

	// ref dies first when this scope exits — must NOT delete owner's
	// data buffer (that would corrupt subsequent reads from owner).
    }

    // After ref destruction, the owner's data is still intact and usable.
    uint32_t survivor;
    memcpy(&survivor, owner[100], sizeof(uint32_t));
    REQUIRE(survivor == 0xCAFE0000u + 100u);
}


void
testTransferDataMakesOriginalSafelyDestructible ()
{
    cout << "  state 1 + transferData=true: original becomes state 4, both safe to dtor"
	 << endl;

    // Build owner in state 1.  Its data buffer holds the sentinel.
    SimdReg *owner = new SimdReg (true, sizeof(uint32_t));
    fillSentinel(*owner, 0x1234ABCD);

    // The reference ctor with transferData=true MOVES ownership of the
    // buffer into the new ref reg.  After construction:
    //   - ref is in state 3: _ref=this, _data=transferred buffer, owns it
    //   - owner is in state 4: _data=null, can ONLY be destroyed safely
    SimdBoolMask mask (false);
    SimdReg *ref = new SimdReg (*owner, mask, /*offset=*/0,
				MAX_REG_SIZE, /*transferData=*/true);
    REQUIRE(ref->isReference());

    // The transferred buffer's contents are still observable through
    // the ref — bytes weren't disturbed, just re-pointed.
    uint32_t got;
    memcpy(&got, (*ref)[7], sizeof(uint32_t));
    REQUIRE(got == 0x1234ABCDu + 7u);

    // Destroy ref FIRST: this frees the transferred buffer (since the
    // ref now owns it).  Then destroy owner (state 4): its dtor runs,
    // sees _dataOwned=true but _data=nullptr, calls delete[] nullptr
    // which is well-defined and a no-op.  No double-free.
    delete ref;
    delete owner;
}


void
testTransferDataReverseDestructionOrder ()
{
    cout << "  state 4 then state 3: destroying owner first must not poison ref"
	 << endl;

    // Same setup as above, but destroy owner (state 4) FIRST.  After
    // transfer, owner has _data=null and _offsets=zeroOffset, so its
    // dtor is a no-op for both.  ref must remain fully usable until
    // we destroy it ourselves.
    SimdReg *owner = new SimdReg (true, sizeof(uint32_t));
    fillSentinel(*owner, 0x55AA00FF);

    SimdBoolMask mask (false);
    SimdReg *ref = new SimdReg (*owner, mask, /*offset=*/0,
				MAX_REG_SIZE, /*transferData=*/true);

    // Owner now in state 4 — destroy it.  Must not invalidate ref.
    delete owner;

    // ref still observes the original data (which it now owns).
    uint32_t got;
    memcpy(&got, (*ref)[15], sizeof(uint32_t));
    REQUIRE(got == 0x55AA00FFu + 15u);

    // Mutate through ref to confirm the buffer is still valid memory.
    uint32_t mark = 0xFFFFFFFF;
    memcpy((*ref)[15], &mark, sizeof(uint32_t));
    memcpy(&got, (*ref)[15], sizeof(uint32_t));
    REQUIRE(got == 0xFFFFFFFFu);

    delete ref;
}


void
testReferenceMethodTransfersData ()
{
    cout << "  reference() member: in-place transition to state 3 via transferData"
	 << endl;

    // The reference() member function is the in-place equivalent of the
    // reference ctor.  Build a value reg, then call reference() on a
    // separate value reg with transferData=true to absorb the first
    // reg's buffer.
    SimdReg source (true, sizeof(uint32_t));
    fillSentinel(source, 0xAABBCCDD);

    // `target` starts as its own value reg.  reference(source, true)
    // should free target's own buffer and absorb source's.
    SimdReg target (true, sizeof(uint32_t));
    target.reference(source, /*transferData=*/true);

    REQUIRE(target.isReference());

    // target now sees the sentinel data from source.
    uint32_t got;
    memcpy(&got, target[3], sizeof(uint32_t));
    REQUIRE(got == 0xAABBCCDDu + 3u);

    // source is now state 4 — operations on it would be undefined.
    // Just verifying its destruction (at scope exit) is safe.
}


void
testValueRegBasicReadWrite ()
{
    cout << "  state 1 isolated: value reg owns and frees its own buffer"
	 << endl;

    // Sanity check that a state-1 register with a non-trivial element
    // size manages its own buffer correctly through the dtor.  Each
    // `r` in scope below allocates and frees a 64 KiB buffer
    // (8192 lanes * 8 bytes); a leak shows up under ASan/valgrind.
    for (int iter = 0; iter < 16; ++iter)
    {
	SimdReg r (true, sizeof(uint64_t));
	uint64_t marker = 0xDEADBEEFCAFEBABEull;
	memcpy(r[0],          &marker, sizeof(uint64_t));
	memcpy(r[MAX_REG_SIZE - 1], &marker, sizeof(uint64_t));

	uint64_t echo0, echoN;
	memcpy(&echo0, r[0], sizeof(uint64_t));
	memcpy(&echoN, r[MAX_REG_SIZE - 1], sizeof(uint64_t));
	REQUIRE(echo0 == marker);
	REQUIRE(echoN == marker);
    }
}

} // anonymous namespace


void
testSimdRegOwnership ()
{
    cout << endl;
    cout << "Testing SimdReg ownership state transitions" << endl;

    testValueRegBasicReadWrite();
    testNonTransferReferenceLifetimes();
    testTransferDataMakesOriginalSafelyDestructible();
    testTransferDataReverseDestructionOrder();
    testReferenceMethodTransfersData();

    cout << "ok" << endl;
}
