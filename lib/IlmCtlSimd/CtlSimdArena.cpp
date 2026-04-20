///////////////////////////////////////////////////////////////////////////
// Copyright (c) 2013 Academy of Motion Picture Arts and Sciences
// ("A.M.P.A.S."). Portions contributed by others as indicated.
// All rights reserved.
//
// (License text omitted for brevity — see CtlSimdReg.h for the full
// ASWF BSD-style license that governs this file.)
///////////////////////////////////////////////////////////////////////////

#include <CtlSimdArena.h>

namespace Ctl {


SimdArena::SimdArena ()
    : _chunks(), _chunkIdx(0), _offset(0)
{
    addChunk (kInitialChunkBytes);
}


SimdArena::~SimdArena ()
{
    for (std::size_t i = 0; i < _chunks.size(); ++i)
	std::free (_chunks[i].base);
}


void
SimdArena::reset ()
{
    _chunkIdx = 0;
    _offset = 0;
}


bool
SimdArena::addChunk (std::size_t minBytes)
{
    std::size_t cap = kInitialChunkBytes;
    while (cap < minBytes)
	cap *= 2;

    void *mem = std::malloc (cap);
    if (!mem)
	return false;

    Chunk c;
    c.base = static_cast<char *>(mem);
    c.capacity = cap;
    _chunks.push_back (c);
    return true;
}


} // namespace Ctl
