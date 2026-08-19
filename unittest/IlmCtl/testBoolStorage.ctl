// Fixture for testBoolStorage.  The struct puts a bool ahead of an int so the
// bool is followed by padding, which is what a wrongly-sized read walks into.

struct Flagged
{
    bool flag;
    int  marker;
};

void
boolStorage (output Flagged o, input Flagged i)
{
    o.flag   = i.flag;
    o.marker = i.marker;
}
