///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Concurrent-callFunction smoke + race test for the SIMD interpreter.
//
// The vectorized interpreter and arena allocation introduced on the
// cpu-perf branch (commit 2521270) made the interpreter call path
// thread-safe when each thread holds its own FunctionCall (and
// therefore its own SimdXContext + SimdArena).  IlmCtlTest's other
// suites are entirely single-threaded; ctlrender's tile-parallel
// dispatch is the only thing that exercises this property in normal
// CI, and a regression there shows up as flaky output rather than a
// hard failure.
//
// This test pre-creates N FunctionCall objects on the main thread,
// then spawns N std::threads that simultaneously call callFunction()
// on their own FunctionCall against thread-specific input data, with
// a release-barrier countdown so every thread starts the call in the
// same window.  Each thread's expected output is a pure function of
// its seed, so divergence (corrupted lane, leaked state across
// arenas, double-free in shared bool-pool) shows up as a per-lane
// assertion failure inside the worker.
//
// Repeats the cycle N_ITER times to give TSan and any genuine race
// multiple chances to fire.  Under TSan a single race report from any
// iteration is a regression.
//

#include <CtlSimdInterpreter.h>
#include <CtlFunctionCall.h>
#include <CtlType.h>
#include <testConcurrentCalls.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <cassert>
#include <cstring>
#include <cstdio>

using namespace Ctl;
using namespace std;


namespace {

// Per-thread workload: own FunctionCall, own buffers, own seed.
struct Worker
{
    FunctionCallPtr	func;
    int			threadId;
    size_t		nSamples;
    float		k;            // uniform multiplier
    vector<float>	input;        // per-thread varying input
    vector<float>	output;       // per-thread varying output (read-back)
    bool		ok;
    string		failMessage;
};


void
workerBody (Worker *w,
            atomic<int> *startCount,
            int totalThreads)
{
    try
    {
	// Find input/output args.
	FunctionArgPtr xInArg  = w->func->findInputArg("xIn");
	FunctionArgPtr xOutArg = w->func->findOutputArg("xOut");
	FunctionArgPtr kArg    = w->func->findInputArg("k");
	if (!xInArg || !xOutArg || !kArg)
	{
	    w->ok = false;
	    w->failMessage = "missing arg in function signature";
	    return;
	}

	// Populate inputs with thread-distinct values.
	float *xIn = (float*)(xInArg->data());
	for (size_t i = 0; i < w->nSamples; ++i)
	    xIn[i] = w->input[i];

	float *kPtr = (float*)(kArg->data());
	kPtr[0] = w->k;

	// Release-barrier: every worker hits this point, then waits for
	// the last one to arrive before calling callFunction.  Maximizes
	// the window where multiple threads are simultaneously inside
	// the SIMD interpreter.
	startCount->fetch_add(1, memory_order_acq_rel);
	while (startCount->load(memory_order_acquire) < totalThreads)
	    this_thread::yield();

	w->func->callFunction(w->nSamples);

	// Read back outputs to the worker's own buffer (so the main
	// thread can verify after join).
	float *xOut = (float*)(xOutArg->data());
	w->output.assign(xOut, xOut + w->nSamples);

	w->ok = true;
    }
    catch (const exception &e)
    {
	w->ok = false;
	w->failMessage = string("exception: ") + e.what();
    }
    catch (...)
    {
	w->ok = false;
	w->failMessage = "unknown exception";
    }
}


void
runOneIteration (SimdInterpreter &interp,
                 const string &funcName,
                 int nThreads,
                 size_t nSamples,
                 int iterIdx)
{
    // Pre-create FunctionCalls on the main thread so newFunctionCall()
    // itself doesn't have to be called concurrently.  (newFunctionCall
    // mutates interpreter state; running it from many threads would
    // be a separate test of a different invariant.)
    vector<Worker> workers(nThreads);
    for (int t = 0; t < nThreads; ++t)
    {
	workers[t].func = interp.newFunctionCall(funcName);
	workers[t].threadId = t;
	workers[t].nSamples = nSamples;
	workers[t].k = 1.0f + 0.5f * static_cast<float>(t);
	workers[t].input.resize(nSamples);
	workers[t].ok = false;
	for (size_t i = 0; i < nSamples; ++i)
	{
	    // Distinct per-thread, per-lane input pattern.
	    workers[t].input[i] =
		static_cast<float>(t * 1000) + static_cast<float>(i);
	}
    }

    atomic<int> startCount(0);
    vector<thread> threads;
    threads.reserve(nThreads);
    for (int t = 0; t < nThreads; ++t)
    {
	threads.emplace_back(workerBody, &workers[t], &startCount, nThreads);
    }
    for (auto &th : threads)
	th.join();

    // Verify: each lane equals input * k + 1.0 with that thread's k.
    for (int t = 0; t < nThreads; ++t)
    {
	if (!workers[t].ok)
	{
	    fprintf(stderr,
		    "iter %d, thread %d failed: %s\n",
		    iterIdx, t, workers[t].failMessage.c_str());
	    assert(false && "worker reported failure");
	}
	for (size_t i = 0; i < nSamples; ++i)
	{
	    float expected = workers[t].input[i] * workers[t].k + 1.0f;
	    float got = workers[t].output[i];
	    if (expected != got)
	    {
		fprintf(stderr,
		        "iter %d, thread %d, lane %zu: "
		        "expected %.7f got %.7f (in=%.7f, k=%.7f)\n",
		        iterIdx, t, i, expected, got,
		        workers[t].input[i], workers[t].k);
		assert(false && "per-lane output diverges from serial expectation");
	    }
	}
    }
}

} // anonymous namespace


void
testConcurrentCalls ()
{
    cout << endl;
    cout << "Testing concurrent FunctionCall::callFunction "
	 << "across multiple threads" << endl;

    SimdInterpreter interp;

    // Look for the .ctl fixture in the binary dir like other tests do.
    interp.loadModule("testConcurrentCalls");
    const string funcName = "concurrent_test::multiply_add";

    // Sweep a range of (threads, samples) shapes.  Smaller sample
    // counts hit the non-tiled path; larger counts (>4096) hit the
    // tile-parallel dispatch inside callFunction.  Repeat each shape
    // multiple times to give TSan and any genuine race repeated
    // chances to fire.
    const int kIterations = 8;
    struct Shape { int threads; size_t samples; };
    Shape shapes[] = {
	{1,  64},      // single-thread sanity
	{2,  64},      // tiny
	{4,  1024},    // sub-tile
	{8,  4096},    // mid-range
	{8,  8192},    // == interp.maxSamples()
    };

    const size_t cap = interp.maxSamples();
    for (size_t s = 0; s < sizeof(shapes)/sizeof(shapes[0]); ++s)
    {
	if (shapes[s].samples > cap)
	{
	    cerr << "test bug: shape " << s << " sample count "
		 << shapes[s].samples << " exceeds maxSamples=" << cap
		 << endl;
	    assert(false);
	}
	cout << "  threads=" << shapes[s].threads
	     << " samples=" << shapes[s].samples
	     << " (" << kIterations << " iterations)" << endl;
	for (int it = 0; it < kIterations; ++it)
	{
	    runOneIteration(interp, funcName,
			    shapes[s].threads, shapes[s].samples, it);
	}
    }

    cout << "ok" << endl;
}
