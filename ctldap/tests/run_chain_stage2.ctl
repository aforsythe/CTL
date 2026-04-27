// Stage 2: add 7 to rIn → rOut  (rIn comes from stage1's rOut)
namespace chain_stage2 {

void
main (output float rOut, input float rIn)
{
    rOut = rIn + 7.0;
}

}
