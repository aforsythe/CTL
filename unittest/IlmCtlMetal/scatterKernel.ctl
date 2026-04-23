// Kernel-reachable call to scatteredDataToGrid3D: exercises the Metal
// backend's port of the RBF solve + grid eval (CtlMetalCodegen.cpp). The
// function is called per-pixel, so the MSL stdlib helper runs on every
// lane. Data / pMin / pMax are module-scope constants (uniform) so the
// uniform-input assumption holds.

namespace testMetalScatterKernel
{

// Five scattered identity points (pos == val): interpolant should be
// close to the identity map at any grid point within the pMin..pMax box.
const float kData[5][2][3] =
{
    {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},
    {{1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}},
    {{0.0, 1.0, 0.0}, {0.0, 1.0, 0.0}},
    {{0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}},
    {{1.0, 1.0, 1.0}, {1.0, 1.0, 1.0}}
};

const float kPMin[3] = {0.0, 0.0, 0.0};
const float kPMax[3] = {1.0, 1.0, 1.0};

// Kernel-reachable entry: builds a 4^3 grid per lane, samples one cell,
// emits that cell's three components as rOut/gOut/bOut. Inputs r/g/b are
// unused (varying), but their presence forces the function into the
// kernel-reachable path the test is meant to exercise.
void
eval (input varying float rIn,
      input varying float gIn,
      input varying float bIn,
      output varying float rOut,
      output varying float gOut,
      output varying float bOut)
{
    float grid[4][4][4][3];
    scatteredDataToGrid3D (kData, kPMin, kPMax, grid);

    // Tap the middle cell (1,1,1) of the 4^3 grid.  For the identity
    // data set above, the expected grid[i][j][k] ≈ (pMin + s*(pMax-pMin))
    // with s = index/(size-1), so this cell ≈ (1/3, 1/3, 1/3).
    //
    // Also fold the input channels in at zero scale so the SIMD CPU
    // reference and the Metal GPU path both see a live varying read
    // (and the backend can't hoist the whole function out of the
    // per-lane call graph).
    rOut = grid[1][1][1][0] + 0.0 * rIn;
    gOut = grid[1][1][1][1] + 0.0 * gIn;
    bOut = grid[1][1][1][2] + 0.0 * bIn;
}

} // namespace testMetalScatterKernel
