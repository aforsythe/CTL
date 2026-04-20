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

#include <stdio.h>
#include <algorithm>
#include <atomic>
#include <exception>
#include <list>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <vector>
#ifndef _WIN32
	#include <sys/param.h>
#endif
#include <errno.h>
#include <ImfThreading.h>
#include "transform.hh"
#include <Iex.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#ifdef __linux__
#include <linux/limits.h>
#elif _WIN32
#include <windows.h>
#include <io.h>
#define PATH_MAX MAX_PATH
#ifndef S_ISDIR
#define S_ISDIR(mode)  (((mode) & S_IFMT) == S_IFDIR)
#endif

#ifndef S_ISREG
#define S_ISREG(mode)  (((mode) & S_IFMT) == S_IFREG)
#endif
#define F_OK 0

#else
#endif

#if !defined(TRUE)
#define TRUE 1
#endif
#if !defined(FALSE)
#define FALSE 0
#endif

double getfloat(const char *param, const char *name, ...)
{
	double result;
	char *end;

	end=NULL;
	result=strtod(param, &end);

	if (end != NULL && *end != 0)
	{
		va_list ap;
		va_start(ap, name);
		fprintf(stderr, "Unable to parse %s as a floating point number. Found as\n", param);
		vfprintf(stderr, name, ap);
		va_end(ap);
		exit(1);
	}

	return result;
}

ctl_parameter_t get_ctl_parameter(const char ***_argv, int *_argc, int start_argc, const char *type, int count)
{
	ctl_parameter_t new_ctl_param;
	const char **argv = *_argv;
	int argc = *_argc;

	memset(&new_ctl_param, 0, sizeof(new_ctl_param));

	argv++;
	argc--;

	new_ctl_param.name = argv[0];

	argv++;
	argc--;

	new_ctl_param.count = count;
	for (int i = 0; i < new_ctl_param.count; i++)
	{
		new_ctl_param.value[i] = getfloat(argv[0], "value %d of %s parameter %s (absolute parameter %d)", i + 1, type, new_ctl_param.name, start_argc - argc);
		argc--;
		argv++;
	}

	*_argv = argv - 1;
	*_argc = argc + 1;

	return new_ctl_param;
}

struct file_format_t
{
	const char *name;
	format_t output_fmt;
};

file_format_t allowed_formats[] =
{
	{ "exr",    format_t("exr",   0) },
    { "exr16",  format_t("exr",  16) },
    { "exr32",  format_t("exr",  32) },
    { "aces",   format_t("aces", 16) },
	{ "dpx",    format_t("dpx",   0) },
	{ "dpx8",   format_t("dpx",   8) },
	{ "dpx10",  format_t("dpx",  10) },
	{ "dpx12",  format_t("dpx",  12) },
	{ "dpx16",  format_t("dpx",  16) },
	{ "tif",    format_t("tif",   0) },
	{ "tiff",   format_t("tiff",  0) },
	{ "tiff32", format_t("tiff", 32) },
	{ "tiff16", format_t("tiff", 16) },
	{ "tiff8",  format_t("tiff",  8) },
	{ "tif32",  format_t("tif",  32) },
	{ "tif16",  format_t("tif",  16) },
	{ "tif8",   format_t("tif",   8) },
	{ NULL,     format_t()           }
};

const format_t &find_format(const char *fmt, const char *message = NULL)
{
	const file_format_t *current = allowed_formats;

	while (current->name != NULL)
	{
		if (!strcmp(current->name, fmt))
		{
			return current->output_fmt;
		}
		current++;
	}
	fprintf(stderr, "Unrecognized format '%s'%s", fmt, message ? message : ".");
	exit(1);
}

int verbosity = 1;

//-----------------------------------------------------------------------------
// Parallelism model: two orthogonal axes, composed as jobs × threads.
//
// -threads controls parallelism WITHIN a single transform().  One input
// image is split into tiles of maxSamples() lanes; worker threads pull
// tiles from an atomic counter and run the SIMD interpreter on each.
// Scales well for large images up to the memory-bandwidth ceiling
// (measured: ~6× on 4K, Phase B in benchmarks/report.md).  Not useful
// when there is only enough work to fill one tile.
//
// -jobs controls parallelism ACROSS input files.  N whole transforms run
// concurrently, each with its own FunctionCall / output buffer / image
// IO.  Scales near-linearly with core count up to the point I/O or
// memory bandwidth saturates, because each worker has its own working
// set and the two layers of the stack (decode / compute / encode)
// overlap naturally across files.
//
// They compose multiplicatively: total active workers ≈ jobs × threads.
// On an N-core machine the right split depends on the batch shape:
//
//   1 input file      →  jobs=1, threads=N      (single-file parallelism
//                                                can only come from the
//                                                tile loop)
//   M files, M ≥ N    →  jobs=N, threads=1      (file parallelism
//                                                dominates; tile
//                                                threading adds
//                                                coordination cost for
//                                                no gain)
//   few files, M < N  →  jobs=M, threads=N/M    (split cores evenly;
//                                                both layers contribute)
//
// Both flags default to autodetect (0) and the auto logic below picks
// from this table using hardware_concurrency() and the input count.
// Explicit values are respected; an explicit -threads survives the
// jobs-resolver's even split.  Measured on a 16-core M4 Max, 100 × 2K
// ACES v2 → tiff8: the auto defaults match hand-tuned within noise
// (57.31 s vs 56.30 s for -jobs 16 -threads 1).
//-----------------------------------------------------------------------------

// Number of worker threads for the CPU per-tile dispatch loop.
//  0  => autodetect: hardware_concurrency() / resolved-file_jobs
//  1  => single-threaded (bit-exact with pre-threading behaviour)
//  >1 => fixed count
int num_threads = 0;

// Number of input files to process concurrently (file-level parallelism).
//  0  => autodetect (default): min(num_input_files,
//        hardware_concurrency()).  See the block comment above for how
//        this composes with num_threads.
//  1  => serial (bit-exact with pre-jobs behaviour)
//  >1 => fixed count
int file_jobs = 0;

int main(int argc, const char **argv)
{
	try
	{
		// list of ctl filenames and associated parameters
		CTLOperations ctl_operations;

		CTLParameters global_ctl_parameters;
		ctl_operation_t new_ctl_operation;

		// list of input images on which to operate
		std::list<const char *> input_image_files;
		char output_path[PATH_MAX];

        Compression compression = Compression::compressionNamed("PIZ");
		format_t desired_format;
		format_t actual_format;
		float input_scale = 0.0;
		float output_scale = 0.0;
		bool force_overwrite_output_file = FALSE;
		bool noalpha = FALSE;

		int start_argc = argc;

		argc--;
		argv++;

		new_ctl_operation.filename = NULL;
		while (argc > 0)
		{
			if (!strncmp(argv[0], "-help", 2))
			{
				if (argc > 1)
				{
					usage(argv[1]);
				}
				else
				{
					usage(NULL);
				}
				exit(1);
			}
			else if (!strncmp(argv[0], "-input_scale", 2))
			{
				if (argc == 1)
				{
					fprintf(stderr,
							"The -input_scale option requires an "
							"additional option specifying a scale\nvalue for the "
							"input file. see '-help scale' for additional "
							"details.\n");
					exit(1);
				}
				else
				{
					char *end = NULL;
					input_scale = strtof(argv[1], &end);
					if (end != NULL && *end != 0)
					{
						fprintf(stderr,
								"Unable to parse '%s' as a floating "
								"point number for the '-input_scale'\nargument\n",
								argv[1]);
						exit(1);
					}
					argv++;
					argc--;
				}
			}
			else if (!strncmp(argv[0], "-output_scale", 2))
			{
				if (argc == 1)
				{
					fprintf(stderr,
							"The -output_scale option requires an "
							"additional option specifying a scale\nvalue for the "
							"output file. see '-help scale' for additional "
							"details.\n");
					exit(1);
				}
				else
				{
					char *end = NULL;
					output_scale = strtof(argv[1], &end);
					if (end != NULL && *end != 0)
					{
						fprintf(stderr,
								"Unable to parse '%s' as a floating "
								"point number for the '-output_scale'\nargument\n",
								argv[1]);
						exit(1);
					}
					argv++;
					argc--;
				}
			}
			else if (!strncmp(argv[0], "-ctl", 3))
			{
				if (argc == 1)
				{
					fprintf(stderr,
							"the -ctl option requires an additional "
							"option specifying a file containing a\nctl script.\n"
							"see '-help ctl' for more details.\n");
					exit(1);
				}

				if (new_ctl_operation.filename != NULL)
				{
					ctl_operations.push_back(new_ctl_operation);
				}
				new_ctl_operation.local.clear();
				new_ctl_operation.filename = argv[1];

				argv++;
				argc--;
			}
			else if (!strncmp(argv[0], "-format", 5))
			{
				if (argc == 1)
				{
					fprintf(stderr,
							"the -format option requires an additional "
							"argument specifying the destination file\nformat. "
							"this may be one of the following: 'dpx10', 'dpx16', "
							"'aces', 'tiff8',\n'tiff16', or 'exr'.\nSee '-help "
							"format' for more details.\n");
					exit(1);
				}
				desired_format = find_format(argv[1]," for parameter '-format'.\nSee '-help format' for more details.");
				argv++;
				argc--;
			}
            else if (!strncmp(argv[0], "-compression", 3))
            {
                if (argc == 1)
                {
                    fprintf(stderr,
                            "the -compression option requires an additional "
                            "argument specifying a compression scheme to be "
                            "used.\n See '-help compression' for more details.\n");
                    exit(1);
                }
				const int compression_name_max_length = 9; // length of longest supported compression name plus null terminator
				char compression_name[compression_name_max_length] = { '\0' };
                for(int i = 0; i < compression_name_max_length && argv[1][i]; ++i) {
                    compression_name[i] = toupper(argv[1][i]);
                }
                compression = Compression::compressionNamed(compression_name);
                if (!strcmp(compression.name, Compression::no_compression.name)) {
                    fprintf(stderr, "Unrecognized compression scheme '%s'. Turning off compression.\n", compression_name);
                }
				desired_format.is_compression_set = true;
                argv++;
                argc--;
            }
			else if (!strcmp(argv[0], "-param1") || !strcmp(argv[0], "-p1"))
			{
				if (argc < 3)
				{
					fprintf(stderr,
							"the -param1 option requires two additional "
							"arguments specifying the\nparameter name and value."
							"\nSee '-help param' for more details.\n");
					exit(1);
				}
				if (new_ctl_operation.filename == NULL)
				{
					THROW(Iex::ArgExc, "the -param1 argument must occur *after* a -ctl option.");
				}
				new_ctl_operation.local.push_back(get_ctl_parameter(&argv, &argc, start_argc, "local", 1));
			}
			else if (!strcmp(argv[0], "-param2") || !strcmp(argv[0], "-p2"))
			{
				if (argc < 4)
				{
					fprintf(stderr,
							"the -param2 option requires three additional "
							"arguments specifying the\nparameter name and value."
							"\nSee '-help param' for more details.\n");
					exit(1);

				}
				if (new_ctl_operation.filename == NULL)
				{
					THROW(Iex::ArgExc, "the -param2 argument must occur *after* a -ctl option.");
				}
				new_ctl_operation.local.push_back(get_ctl_parameter(&argv, &argc, start_argc, "local", 3));
			}
			else if (!strcmp(argv[0], "-param3") || !strcmp(argv[0], "-p3"))
			{
				if (argc < 5)
				{
					fprintf(stderr,
							"the -param3 option requires four additional "
							"arguments specifying the\nparameter name and value."
							"\nSee '-help param' for more details.\n");
					exit(1);
				}
				if (new_ctl_operation.filename == NULL)
				{
					THROW(Iex::ArgExc, "the -param3 argument must occur *after* a -ctl option.");
				}
				new_ctl_operation.local.push_back(get_ctl_parameter(&argv, &argc, start_argc, "local", 3));
			}
			else if (!strcmp(argv[0], "-global_param1") || !strcmp(argv[0], "-gp1"))
			{
				if (argc < 3)
				{
					fprintf(stderr, "the -global_param1 option requires two "
							"additional arguments specifying the\nparameter "
							"name and value.\nSee '-help param' for more "
							"details.\n");
					exit(1);
				}
				global_ctl_parameters.push_back(get_ctl_parameter(&argv, &argc, start_argc, "global", 1));
			}
			else if (!strcmp(argv[0], "-global_param2") || !strcmp(argv[0], "-gp2"))
			{
				if (argc < 4)
				{
					fprintf(stderr, "the -global_param2 option requires three "
							"additional arguments specifying the\nparameter "
							"name and value.\nSee '-help param' for more "
							"details.\n");
					exit(1);
				}
				global_ctl_parameters.push_back(get_ctl_parameter(&argv, &argc, start_argc, "global", 2));
			}
			else if (!strcmp(argv[0], "-global_param3") || !strcmp(argv[0], "-gp3"))
			{
				if (argc < 5)
				{
					fprintf(stderr, "the -global_param3 option requires four "
							"additional arguments specifying the\nparameter "
							"name and value.\nSee '-help param' for more "
							"details.\n");
					exit(1);

				}
				global_ctl_parameters.push_back(get_ctl_parameter(&argv, &argc, start_argc, "global", 2));
			}
			else if (!strncmp(argv[0], "-verbose", 2))
			{
				verbosity++;
			}
			else if (!strncmp(argv[0], "-quiet", 2))
			{
				verbosity--;
			}
			else if (!strncmp(argv[0], "-force", 5))
			{
				force_overwrite_output_file = TRUE;
			}
			else if (!strncmp(argv[0], "-noalpha", 2))
			{
				noalpha = TRUE;
			}
			else if (!strcmp(argv[0], "-threads"))
			{
				if (argc == 1)
				{
					fprintf(stderr,
							"The -threads option requires an additional "
							"integer argument: the number of CPU worker "
							"threads (0 = autodetect, 1 = single-threaded).\n");
					exit(1);
				}
				char *end = NULL;
				long n = strtol(argv[1], &end, 10);
				if (end != NULL && *end != 0)
				{
					fprintf(stderr,
							"Unable to parse '%s' as an integer for the "
							"'-threads' argument.\n", argv[1]);
					exit(1);
				}
				if (n < 0)
				{
					fprintf(stderr, "-threads must be >= 0 (got %ld).\n", n);
					exit(1);
				}
				num_threads = static_cast<int>(n);
				argv++;
				argc--;
			}
			else if (!strcmp(argv[0], "-jobs"))
			{
				if (argc == 1)
				{
					fprintf(stderr,
							"The -jobs option requires an additional "
							"integer argument: the number of input files "
							"to process in parallel (0 = autodetect, "
							"1 = serial).\n");
					exit(1);
				}
				char *end = NULL;
				long n = strtol(argv[1], &end, 10);
				if (end != NULL && *end != 0)
				{
					fprintf(stderr,
							"Unable to parse '%s' as an integer for the "
							"'-jobs' argument.\n", argv[1]);
					exit(1);
				}
				if (n < 0)
				{
					fprintf(stderr, "-jobs must be >= 0 (got %ld).\n", n);
					exit(1);
				}
				file_jobs = static_cast<int>(n);
				argv++;
				argc--;
			}
			else if (!strncmp(argv[0], "-", 1))
			{
				fprintf(stderr,
						"unrecognized option %s. see -help for a list "
						"of available options.\n", argv[0]);
				exit(1);
			}
			else
			{
				input_image_files.push_back(argv[0]);
			}
			argv++;
			argc--;

		}  // end while

		if (new_ctl_operation.filename != NULL)
		{
			ctl_operations.push_back(new_ctl_operation);
		}

		if (input_image_files.size() < 2)
		{
			usage(NULL);
			
			exit(1);
		}

		char *output_slash = NULL;
		const char *outputFile = input_image_files.back();
		input_image_files.pop_back();

		struct stat file_status;
		if (stat(outputFile, &file_status) >= 0)
		{
			if (S_ISDIR(file_status.st_mode))
			{
				memset(output_path, 0, sizeof(output_path));
				strncpy(output_path, outputFile, PATH_MAX -1);
				outputFile = output_path;
				output_slash = output_path + strlen(output_path);
				if (*output_slash != '/')
				{
					*(output_slash++) = '/';
					*output_slash = 0;
				}
			}
			else if (S_ISREG(file_status.st_mode))
			{
				if (input_image_files.size() > 1)
				{
					fprintf(stderr,
							"When providing more than one source "
							"image the destination must be a\ndirectory.\n");
					exit(1);
				}
				else
				{
					if (!force_overwrite_output_file)
					{
						fprintf(stderr,
								"The destination file %s already exists. Refusing to overwrite the existing file.\n"
								"To overwrite the existing file use the -force option.\n"
								, outputFile);
						exit(1);
					}
					else
					{
						// File exists, but we treat it as if it doesn't (see
						// if(output_slash==NULL) {...} down below...
						output_slash = NULL;
					}
				}
			}
			else
			{
				fprintf(stderr,
						"Specified destination is something other than "
						"a file or directory. That's\nprobably a bad idea.\n");
				exit(1);
			}
		}
		else
		{
			if (errno != ENOENT)
			{
				fprintf(stderr, "Unable to get information about %s (%s).\n", outputFile, strerror(errno));
				exit(1);
			}
			if (input_image_files.size() != 1)
			{
				fprintf(stderr,
						"When specifying more than one source file "
						"you must specify the destination as\na directory "
						"that already exists.\nUnable to stat '%s' (%s)\n",
						outputFile, strerror(errno));
				exit(1);
			}
		}

		if (output_slash == NULL)
		{
			// This is the case when our outputFile is a single file. We do a bunch
			// of sanity checking between the extension of the specified file
			// (if any) and the -format option (if any).
			char *dot = (char *) strrchr(outputFile, '.');
			if (dot == NULL && desired_format.ext == NULL)
			{
				fprintf(stderr,
						"You have not explicitly provided an output "
						"format, and the output file name\ndoes not not contain "
						"an extension. Please add an extension to the output "
						"file\nor use the -format option to specify the desired "
						"output format.\n");
				exit(1);
			}
			else if (dot != NULL)
			{
				if (desired_format.ext == NULL)
				{
					actual_format = find_format(dot + 1,
							                    " specified implicitly (by "
									            "the extension) for\noutput file "
									            "format. Please fix this or use\n"
									            "the -format option to specify "
									            "the desired output format.\n");
				}
				else
				{
					// HACK aces format file type check
                    const char *ext = desired_format.ext;
                    static const char exrext[] = "exr";
                    if (!strcmp(ext, "aces"))
                        ext = exrext;
                    if (strcmp(ext, dot + 1) && !force_overwrite_output_file)
					{
						fprintf(stderr,
								"You have specified a destination file "
								"type with the -format option, but the\noutput "
								"file extension does not match the format "
								"specified by the -format option.\nThis behavior "
								"can be overridden by specifing the "
								"-force option (which\nwill make the -format "
								"option take priority).\n");
						exit(1);
					}
					actual_format = desired_format;
				}
			}
		}

		if (verbosity > 1)
		{
			fprintf(stderr, "global ctl parameters:\n");

			CTLParameters temp_ctl_parameters;
			temp_ctl_parameters = global_ctl_parameters;

			while (temp_ctl_parameters.size() > 0)
			{
				ctl_parameter_t new_ctl_parameter = temp_ctl_parameters.front();
				temp_ctl_parameters.pop_front();
				fprintf(stderr, "%17s:", new_ctl_parameter.name);
				for (int i = 0; i < new_ctl_parameter.count; i++)
				{
					fprintf(stderr, " %f", new_ctl_parameter.value[i]);
				}
				fprintf(stderr, "\n");
			}
			fprintf(stderr, "\n");
		}

		// Shared across the multi-file loop so each distinct CTL script is
		// parsed + codegen'd once for the whole batch rather than once per
		// input file.
		InterpreterCache interpreter_cache;

		// Pre-warm so worker threads can read byFilename without locking.
		// Also amortizes module load time before the first transform().
		for (const ctl_operation_t &op : ctl_operations)
		{
			interpreter_cache.preWarm(op.filename);
		}

		// Resolve -jobs 0 (autodetect). Cap at the input count so a
		// single-file batch stays serial (letting per-tile threads take
		// all cores); cap at hardware_concurrency() so we never spawn
		// more file workers than there are CPUs.
		if (file_jobs == 0)
		{
			unsigned hw = std::thread::hardware_concurrency();
			if (hw == 0) hw = 1;
			unsigned nfiles = static_cast<unsigned>(input_image_files.size());
			unsigned j = (nfiles < hw) ? nfiles : hw;
			if (j == 0) j = 1;
			file_jobs = static_cast<int>(j);
		}

		// When running multiple files in parallel, split hardware_concurrency
		// between file-level and tile-level workers so the two layers don't
		// oversubscribe.  Only adjust when num_threads is in its default-auto
		// state; an explicit -threads value is respected.
		if (file_jobs > 1 && num_threads == 0)
		{
			unsigned hw = std::thread::hardware_concurrency();
			if (hw == 0) hw = 1;
			unsigned per = hw / static_cast<unsigned>(file_jobs);
			if (per == 0) per = 1;
			num_threads = static_cast<int>(per);
		}

		// OpenEXR's PIZ/ZIP/ZIPS/DWA codecs split scanline blocks into
		// independent compression tasks submitted to Imf's global thread
		// pool. The default pool size is 0 (single-threaded), so a 4K PIZ
		// write serializes 270 blocks onto one core — on the cpu-perf
		// path that's the dominant stage on compressed outputs (measured
		// 4K 30 PIZ = 62 s vs 40 s for NONE). Size the pool to
		// hardware_concurrency() so the encode/decode work can fan out
		// across all available cores. This composes with -threads/-jobs
		// rather than oversubscribing: while a file worker is inside
		// writePixels/readPixels it's blocked on the Imf pool, so its
		// CPU slot is free for pool threads to use. The net is at most
		// hardware_concurrency() active threads at any instant, whether
		// the bottleneck is compute (jobs × threads) or I/O (pool).
		// Previously capped at 16 to match typical laptop topologies; the
		// cap was removed because it throttled encode on 28-/64-core
		// workstations where file workers still benefit from a full-
		// width pool during aligned encode-heavy phases.
		{
			unsigned hw = std::thread::hardware_concurrency();
			if (hw == 0) hw = 1;
			Imf::setGlobalThreadCount(static_cast<int>(hw));
		}

		const bool is_directory_output = (output_slash != NULL);
		const size_t output_prefix_len =
		    is_directory_output
		        ? static_cast<size_t>(output_slash - output_path)
		        : 0;

		// Snapshot the arg-parsing side-effects each file needs.  The main
		// `output_path`/`outputFile`/`actual_format` are mutated per-file in
		// the body below; workers must use local copies so they don't race.
		const format_t template_format = actual_format;

		auto process_one_file = [&](const char *inputFile)
		{
			char output_path_local[PATH_MAX];
			const char *outputFileLocal;
			format_t format_local = template_format;

			if (is_directory_output)
			{
				memcpy(output_path_local, output_path, output_prefix_len);
				output_path_local[output_prefix_len] = 0;
				char *slash_local = output_path_local + output_prefix_len;

				const char *input_slash = strrchr(inputFile, '/');
				input_slash = input_slash ? input_slash + 1 : inputFile;
				strcpy(slash_local, input_slash);

				char *dot = strrchr(output_path_local, '.');
				if (dot != NULL)
				{
					dot++;
					if (desired_format.ext != NULL)
					{
						// HACK aces format file type check
						const char *ext = desired_format.ext;
						static const char exrext[] = "exr";
						if (!strcmp(ext, "aces"))
							ext = exrext;
						strcpy(dot, ext);
						format_local = desired_format;
					}
					else
					{
						format_local = find_format(dot, " (determined from destination file extension).");
					}
				}
				outputFileLocal = output_path_local;
			}
			else
			{
				outputFileLocal = outputFile;
			}

			if (force_overwrite_output_file)
			{
				if (unlink(outputFileLocal) < 0)
				{
					if (errno != ENOENT)
					{
						fprintf(stderr, "Unable to remove existing file named "
								"'%s' (%s).\n", outputFileLocal, strerror(errno));
						exit(1);
					}
				}
			}
			if (access(outputFileLocal, F_OK) >= 0)
			{
				fprintf(stderr, "Can not overwrite the file '%s'.\n", outputFileLocal);
				exit(1);
			}
			format_local.squish = noalpha;
			if (desired_format.is_compression_set)
			{
				format_local.is_compression_set = true;
			}
			transform(inputFile, outputFileLocal, input_scale, output_scale, &format_local, &compression, ctl_operations, global_ctl_parameters, &interpreter_cache);
		};

		if (file_jobs == 1)
		{
			while (input_image_files.size() > 0)
			{
				process_one_file(input_image_files.front());
				input_image_files.pop_front();
			}
		}
		else
		{
			std::vector<const char *> file_vec(
			    input_image_files.begin(), input_image_files.end());
			input_image_files.clear();

			std::atomic<size_t> next_index(0);
			std::mutex err_mutex;
			std::exception_ptr first_err;
			std::atomic<bool> aborted(false);

			auto worker = [&]()
			{
				try
				{
					while (!aborted.load(std::memory_order_relaxed))
					{
						size_t idx = next_index.fetch_add(
						    1, std::memory_order_relaxed);
						if (idx >= file_vec.size()) break;
						process_one_file(file_vec[idx]);
					}
				}
				catch (...)
				{
					std::lock_guard<std::mutex> g(err_mutex);
					if (!first_err) first_err = std::current_exception();
					aborted.store(true, std::memory_order_relaxed);
				}
			};

			int extras = file_jobs - 1;
			std::vector<std::thread> pool;
			pool.reserve(static_cast<size_t>(extras));
			for (int i = 0; i < extras; i++) pool.emplace_back(worker);
			worker();  // main thread is worker 0
			for (auto &t : pool) t.join();

			if (first_err) std::rethrow_exception(first_err);
		}

		return 0;

	} catch (std::exception &e)
	{
		fprintf(stderr, "\nexception thrown (oops...): %s\n", e.what());
		return 1;
	}
}
