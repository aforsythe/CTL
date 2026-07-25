///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
///////////////////////////////////////////////////////////////////////////

//
// A CTL bool occupies sizeof(bool), which SimdBoolType reports as its
// objectSize.  TypeStorage picks the width of its copy from the type's
// cDataType(), so a bool that advertises IntTypeEnum makes the typed
// accessors read four bytes out of a one-byte object and take the three
// bytes after it into the value.
//
// Existing tests never caught that because they reach the interpreter's
// buffers through FunctionArg::data() and memcpy, which bypasses the typed
// path entirely.  This test goes through set() and get(), and poisons the
// struct first so the bytes after the bool are non-zero.  With a
// wrongly-sized read the poison is what comes back, and the check below is
// the difference between reading one byte and reading four.
//

#include <CtlSimdInterpreter.h>
#include <CtlFunctionCall.h>
#include <CtlType.h>

#include <testBoolStorage.h>

#include <iostream>
#include <stdlib.h>
#include <string.h>

//
// Release builds define NDEBUG, which turns assert() into nothing, so a test
// written with assert() reports success no matter what it observed.  Check
// through something that survives the optimizer settings instead.
//
#define REQUIRE(x)                                                      \
    do {                                                                \
        if (!(x)) {                                                     \
            std::cerr << "REQUIRE failed: " #x " at " << __FILE__       \
                      << ":" << __LINE__ << std::endl;                  \
            exit (1);                                                   \
        }                                                               \
    } while (0)

using namespace Ctl;
using namespace std;

namespace {

void
testFalseSurvivesPoisonedPadding ()
{
    cout << "  bool read back through the typed accessors" << endl;

    SimdInterpreter interp;
    interp.loadFile ("testBoolStorage.ctl");

    FunctionCallPtr func = interp.newFunctionCall ("boolStorage");
    FunctionArgPtr  in   = func->findInputArg ("i");
    REQUIRE (in);

    // Fill the whole struct, padding included, with a non-zero pattern.  Any
    // read wider than the bool now returns something non-zero.
    memset (in->data(), 0xff, in->type()->objectSize());

    const bool f = false;
    in->set (&f, 0, 0, 1, "flag");

    bool readBack = true;
    in->get (&readBack, 0, 0, 1, "flag");

    // Reading four bytes here yields 0x00ffffff, which is true.
    REQUIRE (readBack == false);

    const bool t = true;
    in->set (&t, 0, 0, 1, "flag");
    readBack = false;
    in->get (&readBack, 0, 0, 1, "flag");
    REQUIRE (readBack == true);
}


void
testBoolDoesNotDisturbItsNeighbour ()
{
    cout << "  writing a bool leaves the next member intact" << endl;

    SimdInterpreter interp;
    interp.loadFile ("testBoolStorage.ctl");

    FunctionCallPtr func = interp.newFunctionCall ("boolStorage");
    FunctionArgPtr  in   = func->findInputArg ("i");
    REQUIRE (in);

    StructTypePtr st = in->type().cast<StructType>();
    REQUIRE (st);

    // Reach the second member through its recorded offset rather than through
    // a "/" path.  Addressing it by path goes via Type::childElementV, which
    // has its own offset bug on this branch, and this test is about the width
    // of the bool write, not about path resolution.
    size_t markerOffset = 0;
    for (size_t i = 0; i < st->members().size(); ++i)
        if (st->members()[i].name == "marker")
            markerOffset = st->members()[i].offset;
    REQUIRE (markerOffset != 0);

    char* base = (char*) in->data();
    const int marker = 0x5a5a5a5a;
    memcpy (base + markerOffset, &marker, sizeof (marker));

    const bool f = false;
    in->set (&f, 0, 0, 1, "flag");

    // A write wider than the bool would have reached into marker.
    int readBack = 0;
    memcpy (&readBack, base + markerOffset, sizeof (readBack));
    REQUIRE (readBack == marker);
}

} // namespace


void
testBoolStorage ()
{
    cout << "Testing bool storage width" << endl;

    testFalseSurvivesPoisonedPadding ();
    testBoolDoesNotDisturbItsNeighbour ();

    cout << "ok" << endl;
}
