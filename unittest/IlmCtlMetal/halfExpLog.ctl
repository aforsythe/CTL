//
// Exercises the halfExpLog MTLBuffer hoisting path. Every function here
// reaches at least one of `exp_h` / `log_h` / `log10_h` / `pow10_h` /
// `pow_h` directly or transitively, so the Metal backend emits the
// unconditional `__ctl_half_log10_tbl` / `__ctl_half_log_tbl` /
// `__ctl_half_exp_tbl` pointer args on every user helper and forwards
// them through the whole call graph.
//
// The chain `top -> mid -> leaf` ensures the three table pointers are
// forwarded across two user-to-user call sites before the stdlib helper
// consumes them — that is the exact shape real ACES v2 modules rely on.
//

namespace halfExpLogTest
{
    //
    // Leaf: uses the table directly via all five helpers. `pow_h` is
    // composed as `exp_h(y * log_h(x))` on both backends, so this one
    // call exercises three of the five table accessors in sequence.
    // Casts between `half` and `float` are done via locals with typed
    // declarations (CTL's only cast shape — no C-style conversions).
    //
    float leaf(float a, float b)
    {
        half ea = exp_h(a);
        half hx = fabs(a) + 1.0;
        half hb = fabs(b) + 1.0;
        half la  = log_h(hx);
        half l10 = log10_h(hb);
        half p10 = pow10_h(a * 0.25);
        half pw  = pow_h(hx, b * 0.125);

        float fea  = ea;
        float fla  = la;
        float fl10 = l10;
        float fp10 = p10;
        float fpw  = pw;
        return fea + fla + fl10 + fp10 + fpw;
    }

    //
    // Mid: calls leaf. No direct table use — this forwarder is the
    // case the plumbing guards against. If the three table pointers
    // aren't threaded through `mid`'s signature, leaf's call site in
    // mid's body will fail to compile.
    //
    float mid(float a, float b)
    {
        return leaf(a, b) + leaf(b, a);
    }

    //
    // Top-level kernel. Two mid() calls so the per-dispatch flag plus
    // the three table pointers are forwarded in two different call
    // sites on top-level entry.
    //
    void top(input varying float a,
             input varying float b,
             output varying float out)
    {
        out = mid(a, b) + mid(a * 0.5, b * 0.5);
    }
}
