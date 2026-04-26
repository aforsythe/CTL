namespace coverage_smoke
{

void
add_or_negate (output float r, input float a, input bool negate)
{
    if (negate)
    {
        r = -a;
    }
    else
    {
        r = a + 1.0;
    }
}

} // namespace coverage_smoke
