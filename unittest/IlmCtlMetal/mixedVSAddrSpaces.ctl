//
// Mixed-address-space fixture for the templated VSArray pointer formal.
// `positionalSum` is called with a module-`const` actual and a
// function-local actual in the same kernel, so the emitted MSL must
// hold both `constant` and `thread` instantiations of the helper.
//

namespace mixedVSA
{

const float WEIGHTS[6] = { 1.0,
                           10.0,
                           100.0,
                           1000.0,
                           10000.0,
                           100000.0 };

float
positionalSum (float arr[])
{
    return arr[0] *      1.0 +
           arr[1] *     10.0 +
           arr[2] *    100.0 +
           arr[3] *   1000.0 +
           arr[4] *  10000.0 +
           arr[5] * 100000.0;
}

void
testMixed (input varying float rIn,
           input varying float gIn,
           input varying float bIn,
           output varying float sOut)
{
    float fromConst = positionalSum(WEIGHTS);

    float local[6];
    local[0] = rIn;
    local[1] = gIn;
    local[2] = bIn;
    local[3] = rIn * 2.0;
    local[4] = gIn * 3.0;
    local[5] = bIn * 4.0;
    float fromLocal = positionalSum(local);

    sOut = fromConst + fromLocal;
}

} // namespace mixedVSA
