// Fixture for testEdgeValues — exercises SIMD float arithmetic with
// IEEE-754 special values (NaN, Inf, denormals, signed zero).  The
// SIMD vector path and the scalar fallback must produce bit-identical
// outputs for every input combination, including non-finite values.

namespace edge_test
{

void
arith (input varying float a,
       input varying float b,
       output varying float sum,
       output varying float diff,
       output varying float prod,
       output varying float quot)
{
    sum  = a + b;
    diff = a - b;
    prod = a * b;
    quot = a / b;
}

void
compare_branch (input varying float a,
                input varying float b,
                output varying float r)
{
    // IEEE-754: NaN comparisons all return false.  This branch should
    // therefore take the else path on any lane where either operand is
    // NaN, regardless of the comparison operator used.
    if (a < b)
        r = 1.0;
    else
        r = 0.0;
}

void
masked_loop (input varying int n_in,
             input varying bool active,
             output varying int result)
{
    // Branch-around-loop: lanes with `active=false` must not enter the
    // while loop AT ALL.  This pins the SimdLoopInst's invariant that
    // the per-lane loop mask is the AND of the outer mask and the loop
    // condition — not just the condition, which would silently iterate
    // for lanes the surrounding branch already filtered out.
    int count = 0;
    if (active)
    {
        int i = 0;
        while (i < n_in)
        {
            count = count + 1;
            i = i + 1;
        }
    }
    result = count;
}

}
