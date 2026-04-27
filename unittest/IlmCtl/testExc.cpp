///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Direct C++ tests for the CtlExc.{h,cpp} exception hierarchy.  Pre-
// existing tests never construct or throw these classes from C++ —
// CtlExc.cpp had 0% line coverage — so a regression in the constructor
// overloads or the macro-generated subclasses would slip through CI
// silently.
//
// Note on scope: these tests intentionally exercise only the std::string
// and std::stringstream constructor variants, which are the paths the
// interpreter itself uses (`THROW(IndexOutOfRangeExc, "..."`) wraps the
// stringstream form via Iex's THROW macro).  The printf-style variadic
// constructor + _explain() formatter path turns out to crash
// deterministically inside vsnprintf -> localeconv_l on Apple Silicon
// when given a non-literal format with %-specifiers — likely a latent
// va_list ABI issue that's never been hit because no caller in
// lib/IlmCtl* uses that constructor with format args.  Documented but
// not exercised here; fixing it is a separate task.
//

#include <CtlExc.h>
#include <testExc.h>

#include <iostream>
#include <sstream>
#include <string>
#include <cassert>

using namespace Ctl;
using namespace std;


namespace {

void
testStringConstructor ()
{
    cout << "  std::string ctor preserves message" << endl;

    const string msg = "owned-string error message";
    try
    {
	throw CtlExc(msg);
    }
    catch (const CtlExc &e)
    {
	assert(string(e.what()) == msg);
    }
}


void
testStringstreamConstructor ()
{
    cout << "  std::stringstream ctor preserves message" << endl;

    stringstream ss;
    ss << "stream-built error " << 7 << " (" << 3.14 << ")";
    try
    {
	throw CtlExc(ss);
    }
    catch (const CtlExc &e)
    {
	const string what = e.what();
	assert(what.find("stream-built error 7") != string::npos);
	assert(what.find("3.14") != string::npos);
    }
}


void
testSubclassDispatch ()
{
    cout << "  CTL_DEFINE_EXC subclasses each catch as themselves AND as CtlExc" << endl;

    // Each macro-defined subclass of CtlExc must:
    //   1) be catchable as its own exact type (RTTI sanity)
    //   2) be catchable as CtlExc (inheritance preserved)
    //   3) preserve its message through what()

    // MaxInstExc as its own type.
    try { throw MaxInstExc(string("max inst hit at depth 12")); }
    catch (const MaxInstExc &e)
    {
	assert(string(e.what()) == "max inst hit at depth 12");
    }
    // MaxInstExc as CtlExc base — inheritance preserved.
    try { throw MaxInstExc(string("for base catch")); }
    catch (const CtlExc &e)
    {
	assert(string(e.what()) == "for base catch");
    }

    // IndexOutOfRangeExc.
    try { throw IndexOutOfRangeExc(string("index 5 of 3")); }
    catch (const IndexOutOfRangeExc &e)
    {
	assert(string(e.what()) == "index 5 of 3");
    }

    // Three sibling stack exceptions — RTTI must distinguish them
    // from each other even though they share the macro body.
    try { throw StackOverflowExc(string("overflow")); }
    catch (const StackOverflowExc &e) { assert(string(e.what()) == "overflow"); }
    catch (...) { assert(false && "StackOverflowExc not caught as itself"); }

    try { throw StackUnderflowExc(string("underflow")); }
    catch (const StackUnderflowExc &e) { assert(string(e.what()) == "underflow"); }
    catch (...) { assert(false && "StackUnderflowExc not caught as itself"); }

    try { throw StackLogicExc(string("logic error")); }
    catch (const StackLogicExc &e) { assert(string(e.what()) == "logic error"); }
    catch (...) { assert(false && "StackLogicExc not caught as itself"); }

    // Remaining subclasses share the same macro-generated body, so a
    // representative sample is enough to pin the codegen.
    try { throw LoadModuleExc(string("module load")); }
    catch (const LoadModuleExc &e) { (void)e; }
    catch (...) { assert(false && "LoadModuleExc not caught as itself"); }

    try { throw AbortExc(string("aborted")); }
    catch (const AbortExc &e) { (void)e; }
    catch (...) { assert(false && "AbortExc not caught as itself"); }

    try { throw InvalidSizeExc(string("bad size")); }
    catch (const InvalidSizeExc &e) { (void)e; }
    catch (...) { assert(false && "InvalidSizeExc not caught as itself"); }

    try { throw ArrayMismatchExc(string("mismatch")); }
    catch (const ArrayMismatchExc &e) { (void)e; }
    catch (...) { assert(false && "ArrayMismatchExc not caught as itself"); }

    try { throw StructAccessExc(string("struct")); }
    catch (const StructAccessExc &e) { (void)e; }
    catch (...) { assert(false && "StructAccessExc not caught as itself"); }

    try { throw RuntimeExc(string("compile-time issue")); }
    catch (const RuntimeExc &e) { (void)e; }
    catch (...) { assert(false && "RuntimeExc not caught as itself"); }

    try { throw DatatypeExc(string("type conv")); }
    catch (const DatatypeExc &e) { (void)e; }
    catch (...) { assert(false && "DatatypeExc not caught as itself"); }
}


void
testStdExceptionInteroperability ()
{
    cout << "  every Ctl exception is catchable as std::exception" << endl;

    // CtlExc -> Iex::BaseExc -> std::exception.  Anything that catches
    // std::exception in user code must see Ctl errors.
    try { throw IndexOutOfRangeExc(string("ioor")); }
    catch (const std::exception &e)
    {
	assert(string(e.what()) == "ioor");
    }
}


void
testStringstreamSubclass ()
{
    cout << "  subclass stringstream ctor (the form THROW() wraps)" << endl;

    // Iex's THROW macro builds a stringstream and passes it; that's the
    // pattern the actual interpreter uses everywhere it raises a Ctl
    // exception.  Pin it specifically.
    stringstream ss;
    ss << "computed: idx=" << 42 << " size=" << 16;
    try
    {
	throw IndexOutOfRangeExc(ss);
    }
    catch (const IndexOutOfRangeExc &e)
    {
	const string what = e.what();
	assert(what.find("idx=42") != string::npos);
	assert(what.find("size=16") != string::npos);
    }
}

} // anonymous namespace


void
testExc ()
{
    cout << endl;
    cout << "Testing CtlExc hierarchy (std::string + stringstream paths)" << endl;

    testStringConstructor();
    testStringstreamConstructor();
    testSubclassDispatch();
    testStdExceptionInteroperability();
    testStringstreamSubclass();

    cout << "ok" << endl;
}
