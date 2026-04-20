///////////////////////////////////////////////////////////////////////////
// Copyright (c) 2013 Academy of Motion Picture Arts and Sciences 
// ("A.M.P.A.S."). Portions contributed by others as indicated.
// All rights reserved.
// 
// A worldwide, royalty-free, non-exclusive right to copy, modify, create
// derivatives, and use, in source and binary forms, is hereby granted, 
// subject to acceptance of this license. Performance of any of the 
// aforementioned acts indicates acceptance to be bound by the following 
// terms and conditions:
//
//  * Copies of source code, in whole or in part, must retain the 
//    above copyright notice, this list of conditions and the 
//    Disclaimer of Warranty.
//
//  * Use in binary form must retain the above copyright notice, 
//    this list of conditions and the Disclaimer of Warranty in the
//    documentation and/or other materials provided with the distribution.
//
//  * Nothing in this license shall be deemed to grant any rights to 
//    trademarks, copyrights, patents, trade secrets or any other 
//    intellectual property of A.M.P.A.S. or any contributors, except 
//    as expressly stated herein.
//
//  * Neither the name "A.M.P.A.S." nor the name of any other 
//    contributors to this software may be used to endorse or promote 
//    products derivative of or based on this software without express 
//    prior written permission of A.M.P.A.S. or the contributors, as 
//    appropriate.
// 
// This license shall be construed pursuant to the laws of the State of 
// California, and any disputes related thereto shall be subject to the 
// jurisdiction of the courts therein.
//
// Disclaimer of Warranty: THIS SOFTWARE IS PROVIDED BY A.M.P.A.S. AND 
// CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, 
// BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS 
// FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT ARE DISCLAIMED. IN NO 
// EVENT SHALL A.M.P.A.S., OR ANY CONTRIBUTORS OR DISTRIBUTORS, BE LIABLE 
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, RESITUTIONARY, 
// OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF 
// THE POSSIBILITY OF SUCH DAMAGE.
//
// WITHOUT LIMITING THE GENERALITY OF THE FOREGOING, THE ACADEMY 
// SPECIFICALLY DISCLAIMS ANY REPRESENTATIONS OR WARRANTIES WHATSOEVER 
// RELATED TO PATENT OR OTHER INTELLECTUAL PROPERTY RIGHTS IN THE ACADEMY 
// COLOR ENCODING SYSTEM, OR APPLICATIONS THEREOF, HELD BY PARTIES OTHER 
// THAN A.M.P.A.S., WHETHER DISCLOSED OR UNDISCLOSED.
///////////////////////////////////////////////////////////////////////////

#if !defined(CTLRENDER_TRANSFORM_INCLUDE)
#define CTLRENDER_TRANSFORM_INCLUDE

#include <list>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include "main.hh"

namespace Ctl { class SimdInterpreter; }

// Per-process cache of parsed+codegen'd CTL modules, keyed on
// ctl_operation_t::filename.  Populated lazily by run_ctl_transform();
// lets a multi-file batch pay module load (150-500 ms for aces_combined)
// once per distinct CTL script instead of once per (input file x op).
// Each CTL script gets its own SimdInterpreter so that unqualified
// symbol lookups (e.g. newFunctionCall("main")) do not collide across
// scripts in the same pipeline.
struct InterpreterCache
{
    std::map<std::string, std::unique_ptr<Ctl::SimdInterpreter>> byFilename;
    InterpreterCache();
    ~InterpreterCache();

    // Eagerly parse+codegen `filename` into byFilename.  Main-thread only.
    // Call once per distinct ctl script before any parallel dispatch; after
    // that, worker threads can read byFilename without synchronization
    // because run_ctl_transform's find will always hit an existing entry.
    void preWarm(const char *filename);
};

// structure to capture a CTL parameter.
// A parameter consists of a name and up to 4 floating point values.
// The count member holds the number of floating point values for this parameter
struct ctl_parameter_t
{
	ctl_parameter_t()
	{
		name = 0;
		count = 0;
		memset(value, 0, sizeof(value));
	}

	const char *name;
	uint8_t count;
	float value[4];
};

typedef std::list<ctl_parameter_t> CTLParameters;

// structure to capture the CTL filename plus parameters for the CTL
struct ctl_operation_t
{
	ctl_operation_t()
	{
		filename = 0;
	}

	const char *filename;
	CTLParameters local;
};

typedef std::list<ctl_operation_t> CTLOperations;

void transform(const char *inputFile, const char *outputFile,
		       float input_scale, float output_scale,
		       format_t *format,
               Compression *compression,
		       const CTLOperations &ops, const CTLParameters &global,
		       InterpreterCache *cache);

#endif
