// Per-pixel exercise of scatteredDataToGrid3D on a more realistic
// input distribution than the toy 5-point identity in
// `scatterKernel.ctl`. The transform defines 9 scattered samples
// representing a color-warp (identity at the cube corners plus a
// mid-gray highlight bias), builds a 6^3 RBF grid from them, and
// interpolates the grid at the input pixel's RGB position.
//
// Every lane does the full RBF solve (CPU sidecar parses this for
// its reference, Metal runs the MSL port). Matches the shape of a
// "per-frame gamut warp" workload: the data is uniform across lanes
// but the grid evaluation + subsequent lookup runs per-pixel.

namespace testMetalScatterMarci
{

// Trilinear lookup from a grid that spans [pMin..pMax] in each axis.
// The grid was produced by scatteredDataToGrid3D with the same
// pMin/pMax, so fetching at any query `p` in-bounds gives back the
// RBF's value at `p` (up to grid-resolution error).
float[3]
lookup3D (float grid[6][6][6][3],
          float pMin[3],
          float pMax[3],
          float p[3])
{
    // Map p → continuous index in [0, size-1].
    float idx[3];
    idx[0] = (p[0] - pMin[0]) / (pMax[0] - pMin[0]) * 5.0;
    idx[1] = (p[1] - pMin[1]) / (pMax[1] - pMin[1]) * 5.0;
    idx[2] = (p[2] - pMin[2]) / (pMax[2] - pMin[2]) * 5.0;

    if (idx[0] < 0.0) idx[0] = 0.0;
    if (idx[1] < 0.0) idx[1] = 0.0;
    if (idx[2] < 0.0) idx[2] = 0.0;
    if (idx[0] > 5.0) idx[0] = 5.0;
    if (idx[1] > 5.0) idx[1] = 5.0;
    if (idx[2] > 5.0) idx[2] = 5.0;

    int i0 = idx[0];
    int j0 = idx[1];
    int k0 = idx[2];
    int i1 = i0 + 1; if (i1 > 5) i1 = 5;
    int j1 = j0 + 1; if (j1 > 5) j1 = 5;
    int k1 = k0 + 1; if (k1 > 5) k1 = 5;

    float fi = idx[0] - i0;
    float fj = idx[1] - j0;
    float fk = idx[2] - k0;

    float c00[3]; float c01[3]; float c10[3]; float c11[3];
    float c0[3]; float c1[3]; float result[3];

    for (int ch = 0; ch < 3; ch = ch + 1)
    {
        c00[ch] = grid[i0][j0][k0][ch] * (1 - fk) + grid[i0][j0][k1][ch] * fk;
        c01[ch] = grid[i0][j1][k0][ch] * (1 - fk) + grid[i0][j1][k1][ch] * fk;
        c10[ch] = grid[i1][j0][k0][ch] * (1 - fk) + grid[i1][j0][k1][ch] * fk;
        c11[ch] = grid[i1][j1][k0][ch] * (1 - fk) + grid[i1][j1][k1][ch] * fk;
        c0[ch]  = c00[ch] * (1 - fj) + c01[ch] * fj;
        c1[ch]  = c10[ch] * (1 - fj) + c11[ch] * fj;
        result[ch] = c0[ch] * (1 - fi) + c1[ch] * fi;
    }

    return result;
}

void
main (input varying float rIn,
      input varying float gIn,
      input varying float bIn,
      output varying float rOut,
      output varying float gOut,
      output varying float bOut,
      output varying float aOut,
      input varying float aIn = 1.0)
{
    // 9 scattered data points: 8 cube corners identity-mapped + 1
    // mid-gray point boosted by +0.1 on each channel. This gives the
    // RBF a mild nonlinearity that a linear affine fit can't absorb,
    // forcing the CG solve to produce non-zero lambdas — the real
    // exercise for the Metal port.
    float data[9][2][3] =
    {
        {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},
        {{1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}},
        {{0.0, 1.0, 0.0}, {0.0, 1.0, 0.0}},
        {{1.0, 1.0, 0.0}, {1.0, 1.0, 0.0}},
        {{0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}},
        {{1.0, 0.0, 1.0}, {1.0, 0.0, 1.0}},
        {{0.0, 1.0, 1.0}, {0.0, 1.0, 1.0}},
        {{1.0, 1.0, 1.0}, {1.0, 1.0, 1.0}},
        {{0.5, 0.5, 0.5}, {0.6, 0.6, 0.6}}
    };

    float pMin[3] = {0.0, 0.0, 0.0};
    float pMax[3] = {1.0, 1.0, 1.0};

    float grid[6][6][6][3];
    scatteredDataToGrid3D (data, pMin, pMax, grid);

    float p[3] = {rIn, gIn, bIn};
    float q[3] = lookup3D (grid, pMin, pMax, p);

    rOut = q[0];
    gOut = q[1];
    bOut = q[2];
    aOut = aIn;
}

} // namespace testMetalScatterMarci
