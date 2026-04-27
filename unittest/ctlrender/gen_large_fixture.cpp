///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Generate a deterministic gradient EXR fixture of arbitrary size.  Used
// by the parallel float->half coverage tests in this directory: the
// parallel path in exr_file.cc only fires above kMinToThread = 128 KiB
// of floats, and every checked-in EXR fixture in this directory is well
// below that threshold.  Rather than commit a large binary fixture,
// generate one at build/test time so the parallel write code is
// actually exercised.
//
// Usage:
//   gen_large_fixture <output.exr> <width> <height>
//
// Output is RGB FLOAT, NO_COMPRESSION (binary-deterministic across
// runs and platforms — important for the byte-parity comparison).
//

#include <cstdio>
#include <cstdlib>
#include <vector>

#include <ImfOutputFile.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>


int
main (int argc, char *argv[])
{
    if (argc != 4)
    {
	std::fprintf(stderr,
	             "usage: %s <output.exr> <width> <height>\n", argv[0]);
	return 1;
    }
    const char *outPath = argv[1];
    int width  = std::atoi(argv[2]);
    int height = std::atoi(argv[3]);
    if (width <= 0 || height <= 0)
    {
	std::fprintf(stderr,
	             "width and height must be positive integers\n");
	return 1;
    }

    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<float> r(n), g(n), b(n);

    // Deterministic gradient.  Same dimensions => identical bytes.
    const float invMaxX = (width  > 1) ? 1.0f / float(width  - 1) : 0.0f;
    const float invMaxY = (height > 1) ? 1.0f / float(height - 1) : 0.0f;
    for (int y = 0; y < height; ++y)
    {
	const float ny = float(y) * invMaxY;
	for (int x = 0; x < width; ++x)
	{
	    const float nx = float(x) * invMaxX;
	    const size_t i = size_t(y) * size_t(width) + size_t(x);
	    r[i] = nx;
	    g[i] = ny;
	    b[i] = (nx + ny) * 0.5f;
	}
    }

    Imf::Header header(width, height);
    header.channels().insert("R", Imf::Channel(Imf::FLOAT));
    header.channels().insert("G", Imf::Channel(Imf::FLOAT));
    header.channels().insert("B", Imf::Channel(Imf::FLOAT));
    header.compression() = Imf::NO_COMPRESSION;

    Imf::FrameBuffer fb;
    const int xstride = sizeof(float);
    const int ystride = sizeof(float) * width;
    fb.insert("R", Imf::Slice(Imf::FLOAT, (char*)r.data(), xstride, ystride));
    fb.insert("G", Imf::Slice(Imf::FLOAT, (char*)g.data(), xstride, ystride));
    fb.insert("B", Imf::Slice(Imf::FLOAT, (char*)b.data(), xstride, ystride));

    try
    {
	Imf::OutputFile out(outPath, header);
	out.setFrameBuffer(fb);
	out.writePixels(height);
    }
    catch (const std::exception &e)
    {
	std::fprintf(stderr, "OpenEXR write failed: %s\n", e.what());
	return 1;
    }
    return 0;
}
