//
// Nested variable-size-array parametrization for the Metal codegen
// fix. `mat23_weighted` and `mat33_writeback` exercise the
// `float[][3]` shape used by the ACES v2 gamut mapper; `grid_sample`
// exercises the full multi-dim VSArray shape (`float[][][]`) that the
// CPU SIMD reference consumes via stride arithmetic.
//
// Each helper is wrapped by a kernel-callable entry point that takes
// varying scalars, packs them into a fixed-size local, calls the
// VSArray helper, and writes the result back. The Metal backend
// lowers VSArray parameters to a flat `thread const float*` + length
// uniforms, so `arr[i][j]` and `arr[i][j][k]` must be emitted as
// explicit stride arithmetic rather than chained MSL `[]` operators.
// Evaluating these on both backends and checking bit-exact match is
// the regression guard for that codegen path.
//

namespace nestedVSA
{

//
// Weighted read across `float[][3]`. Multiplies every slot by a
// distinct power-of-ten weight so an incorrect row/column stride
// drift would immediately miscolor the sum. Caller fills the matrix
// with six deterministic values derived from the r/g/b sample.
//
float
mat23_weighted (float m[][3])
{
    return m[0][0] *      1.0 +
           m[0][1] *     10.0 +
           m[0][2] *    100.0 +
           m[1][0] *   1000.0 +
           m[1][1] *  10000.0 +
           m[1][2] * 100000.0;
}

//
// Writeback across `float[][3]`. Assigns each slot to a distinct
// polynomial of the input so an incorrect stride on the LHS would
// land values in the wrong cell and the caller's summed readback
// would drift.
//
void
mat33_writeback (output float m[][3], float x)
{
    m[0][0] =   x;
    m[0][1] =   x * 2.0;
    m[0][2] =   x * 3.0;
    m[1][0] =   x * 5.0;
    m[1][1] =   x * 7.0;
    m[1][2] =   x * 11.0;
    m[2][0] =   x * 13.0;
    m[2][1] =   x * 17.0;
    m[2][2] =   x * 19.0;
}

//
// Full multi-dim VSArray shape (`float[][][]`). Three leading
// variable dims with no fixed tail — the 3D grid is stored flat and
// indexed as `g[i][j][k]` by the callee. The caller passes a
// compile-time 2x3x4 grid whose cell values are `1000*i + 10*j + k`,
// so a wrong stride in ANY axis produces a reading that differs from
// the CPU reference by a multiple of 1, 10, or 1000.
//
float
grid_sample (float g[][][], int i, int j, int k)
{
    return g[i][j][k];
}

//
// Kernel-callable wrapper. Packs the r/g/b inputs into a 2x3 matrix
// and sums the weighted readback; caller asserts bit-exact match
// against CPU SIMD. The six matrix slots hold r, g+1, b+2, r+3,
// g+4, b+5 so every cell is distinguishable even when r == g == b.
//
void
testMat23Weighted (input varying float rIn,
                   input varying float gIn,
                   input varying float bIn,
                   output varying float sOut)
{
    float m[2][3] = {
        { rIn,       gIn + 1.0, bIn + 2.0 },
        { rIn + 3.0, gIn + 4.0, bIn + 5.0 }
    };
    sOut = mat23_weighted(m);
}

//
// Kernel-callable wrapper for the writable path. Calls the VSArray
// writer with `rIn`, then reads back every cell of the 3x3 local
// and folds them into a single scalar output with distinct
// power-of-ten weights — any stride error on the LHS of an
// assignment in `mat33_writeback` shows up as a diverged scalar.
//
void
testMat33Writeback (input varying float rIn,
                    input varying float gIn,
                    input varying float bIn,
                    output varying float sOut)
{
    float m[3][3] = {
        { 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0 }
    };
    mat33_writeback(m, rIn);

    sOut = m[0][0] *         1.0 +
           m[0][1] *        10.0 +
           m[0][2] *       100.0 +
           m[1][0] *      1000.0 +
           m[1][1] *     10000.0 +
           m[1][2] *    100000.0 +
           m[2][0] *   1000000.0 +
           m[2][1] *  10000000.0 +
           m[2][2] * 100000000.0 +
           gIn * 0.0 + bIn * 0.0;
}

//
// Kernel-callable wrapper for the three-dim multi-VSArray path.
// Reads one fixed lattice cell from the [2][3][4] grid and tags it
// with the input triplet so the per-sample output varies.
//
void
testGridSample (input varying float rIn,
                input varying float gIn,
                input varying float bIn,
                output varying float sOut)
{
    float g[2][3][4];
    int i = 0;
    while (i < 2)
    {
        int j = 0;
        while (j < 3)
        {
            int k = 0;
            while (k < 4)
            {
                //
                // CTL's implicit int→float conversion: assigning an
                // integer expression to a float local coerces the
                // value. CTL has no `float(i)` cast spelling, so use
                // an intermediate float local instead.
                //
                float fi; fi = i;
                float fj; fj = j;
                float fk; fk = k;
                g[i][j][k] = fi * 1000.0 + fj * 10.0 + fk;
                k = k + 1;
            }
            j = j + 1;
        }
        i = i + 1;
    }
    //
    // Sample three specific cells so every stride axis is exercised
    // on the reader side. Folding the result with the r/g/b
    // coefficients ties the per-sample output to the test inputs so
    // divergence is easy to trace back.
    //
    float a = grid_sample(g, 0, 0, 0);   // 0
    float b = grid_sample(g, 1, 0, 0);   // 1000
    float c = grid_sample(g, 0, 2, 0);   // 20
    float d = grid_sample(g, 0, 0, 3);   // 3
    float e = grid_sample(g, 1, 2, 3);   // 1023
    sOut = a + b * rIn + c * gIn + d * bIn + e;
}

} // namespace nestedVSA
