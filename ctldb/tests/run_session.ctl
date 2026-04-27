namespace ctldb_test {

void
helper (output float r, input float x)
{
    r = x * 1.5 + 0.25;
}

void
main (output float rOut, input float rIn)
{
    float postHelper;
    helper (postHelper, rIn);
    rOut = postHelper * 2.0;
}

}
