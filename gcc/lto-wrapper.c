/* Wrapper to call lto.  Used by collect2 and the linker plugin.
   Copyright (C) 2009-2015 Free Software Foundation, Inc.

   Factored out of collect2 by Rafael Espindola <espindola@google.com>

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */


/* This program is passed a gcc, a list of gcc arguments and a list of
   object files containing IL. It scans the argument list to check if
   we are in whopr mode or not modifies the arguments and needed and
   prints a list of output files on stdout.

   Example:

   $ lto-wrapper gcc/xgcc -B gcc a.o b.o -o test -flto

   The above will print something like
   /tmp/ccwbQ8B2.lto.o

   If WHOPR is used instead, more than one file might be produced
   ./ccXj2DTk.lto.ltrans.o
   ./ccCJuXGv.lto.ltrans.o
*/

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "intl.h"
#include "diagnostic.h"
#include "obstack.h"
#include "opts.h"
#include "options.h"
#include "simple-object.h"
#include "lto-section-names.h"
#include "collect-utils.h"

/* Environment variable, used for passing the names of offload targets from GCC
   driver to lto-wrapper.  */
#define OFFLOAD_TARGET_NAMES_ENV	"OFFLOAD_TARGET_NAMES"

enum lto_mode_d {
  LTO_MODE_NONE,			/* Not doing LTO.  */
  LTO_MODE_LTO,				/* Normal LTO.  */
  LTO_MODE_WHOPR			/* WHOPR.  */
};

/* Current LTO mode.  */
static enum lto_mode_d lto_mode = LTO_MODE_NONE;

static char *ltrans_output_file;
static char *flto_out;
static unsigned int nr;
static char **input_names;
static char **output_names;
static char **offload_names;
static const char *offloadbegin, *offloadend;
static char *makefile;

const char tool_name[] = "lto-wrapper";

/* Delete tempfiles.  Called from utils_cleanup.  */

void
tool_cleanup (bool)
{
  unsigned int i;

  if (ltrans_output_file)
    maybe_unlink (ltrans_output_file);
  if (flto_out)
    maybe_unlink (flto_out);
  if (makefile)
    maybe_unlink (makefile);
  for (i = 0; i < nr; ++i)
    {
      maybe_unlink (input_names[i]);
      if (output_names[i])
	maybe_unlink (output_names[i]);
    }
}

static void
lto_wrapper_cleanup (void)
{
  utils_cleanup (false);
}

/* Unlink a temporary LTRANS file unless requested otherwise.  */

void
maybe_unlink (const char *file)
{
  if (!save_temps)
    {
      if (unlink_if_ordinary (file)
	  && errno != ENOENT)
	fatal_error (input_location, "deleting LTRANS file %s: %m", file);
    }
  else if (verbose)
    fprintf (stderr, "[Leaving LTRANS %s]\n", file);
}

/* Template of LTRANS dumpbase suffix.  */
#define DUMPBASE_SUFFIX ".ltrans18446744073709551615"

/* Create decoded options from the COLLECT_GCC and COLLECT_GCC_OPTIONS
   environment according to LANG_MASK.  */

static void
get_options_from_collect_gcc_options (const char *collect_gcc,
				      const char *collect_gcc_options,
				      unsigned int lang_mask,
				      struct cl_decoded_option **decoded_options,
				      unsigned int *decoded_options_count)
{
  struct obstack argv_obstack;
  char *argv_storage;
  const char **argv;
  int j, k, argc;

  argv_storage = xstrdup (collect_gcc_options);
  obstack_init (&argv_obstack);
  obstack_ptr_grow (&argv_obstack, collect_gcc);

  for (j = 0, k = 0; argv_storage[j] != '\0'; ++j)
    {
      if (argv_storage[j] == '\'')
	{
	  obstack_ptr_grow (&argv_obstack, &argv_storage[k]);
	  ++j;
	  do
	    {
	      if (argv_storage[j] == '\0')
		fatal_error (input_location, "malformed COLLECT_GCC_OPTIONS");
	      else if (strncmp (&argv_storage[j], "'\\''", 4) == 0)
		{
		  argv_storage[k++] = '\'';
		  j += 4;
		}
	      else if (argv_storage[j] == '\'')
		break;
	      else
		argv_storage[k++] = argv_storage[j++];
	    }
	  while (1);
	  argv_storage[k++] = '\0';
	}
    }

  obstack_ptr_grow (&argv_obstack, NULL);
  argc = obstack_object_size (&argv_obstack) / sizeof (void *) - 1;
  argv = XOBFINISH (&argv_obstack, const char **);

  decode_cmdline_options_to_array (argc, (const char **)argv,
				   lang_mask,
				   decoded_options, decoded_options_count);
  obstack_free (&argv_obstack, NULL);
}

/* Append OPTION to the options array DECODED_OPTIONS with size
   DECODED_OPTIONS_COUNT.  */

static void
append_option (struct cl_decoded_option **decoded_options,
	       unsigned int *decoded_options_count,
	       struct cl_decoded_option *option)
{
  ++*decoded_options_count;
  *decoded_options
    = (struct cl_decoded_option *)
	xrealloc (*decoded_options,
		  (*decoded_options_count
		   * sizeof (struct cl_decoded_option)));
  memcpy (&(*decoded_options)[*decoded_options_count - 1], option,
	  sizeof (struct cl_decoded_option));
}

/* Try to merge and complain about options FDECODED_OPTIONS when applied
   ontop of DECODED_OPTIONS.  */

static void
merge_and_complain (struct cl_decoded_option **decoded_options,
		    unsigned int *decoded_options_count,
		    struct cl_decoded_option *fdecoded_options,
		    unsigned int fdecoded_options_count)
{
  unsigned int i, j;

  /* ???  Merge options from files.  Most cases can be
     handled by either unioning or intersecting
     (for example -fwrapv is a case for unioning,
     -ffast-math is for intersection).  Most complaints
     about real conflicts between different options can
     be deferred to the compiler proper.  Options that
     we can neither safely handle by intersection nor
     unioning would need to be complained about here.
     Ideally we'd have a flag in the opt files that
     tells whether to union or intersect or reject.
     In absence of that it's unclear what a good default is.
     It's also difficult to get positional handling correct.  */

  /* The following does what the old LTO option code did,
     union all target and a selected set of common options.  */
  for (i = 0; i < fdecoded_options_count; ++i)
    {
      struct cl_decoded_option *foption = &fdecoded_options[i];
      switch (foption->opt_index)
	{
	case OPT_SPECIAL_unknown:
	case OPT_SPECIAL_ignore:
	case OPT_SPECIAL_program_name:
	case OPT_SPECIAL_input_file:
	  break;

	default:
	  if (!(cl_options[foption->opt_index].flags & CL_TARGET))
	    break;

	  /* Fallthru.  */
	case OPT_fdiagnostics_show_caret:
	case OPT_fdiagnostics_show_option:
	case OPT_fdiagnostics_show_location_:
	case OPT_fshow_column:
	case OPT_fPIC:
	case OPT_fpic:
	case OPT_fPIE:
	case OPT_fpie:
	case OPT_fcommon:
	case OPT_fexceptions:
	case OPT_fnon_call_exceptions:
	case OPT_fgnu_tm:
	  /* Do what the old LTO code did - collect exactly one option
	     setting per OPT code, we pick the first we encounter.
	     ???  This doesn't make too much sense, but when it doesn't
	     then we should complain.  */
	  for (j = 0; j < *decoded_options_count; ++j)
	    if ((*decoded_options)[j].opt_index == foption->opt_index)
	      break;
	  if (j == *decoded_options_count)
	    append_option (decoded_options, decoded_options_count, foption);
	  break;

	case OPT_ftrapv:
	case OPT_fstrict_overflow:
	case OPT_ffp_contract_:
	  /* For selected options we can merge conservatively.  */
	  for (j = 0; j < *decoded_options_count; ++j)
	    if ((*decoded_options)[j].opt_index == foption->opt_index)
	      break;
	  if (j == *decoded_options_count)
	    append_option (decoded_options, decoded_options_count, foption);
	  /* FP_CONTRACT_OFF < FP_CONTRACT_ON < FP_CONTRACT_FAST,
	     -fno-trapv < -ftrapv,
	     -fno-strict-overflow < -fstrict-overflow  */
	  else if (foption->value < (*decoded_options)[j].value)
	    (*decoded_options)[j] = *foption;
	  break;

	case OPT_fmath_errno:
	case OPT_fsigned_zeros:
	case OPT_ftrapping_math:
	case OPT_fwrapv:
	case OPT_fopenmp:
	case OPT_fopenacc:
	case OPT_fcheck_pointer_bounds:
	  /* For selected options we can merge conservatively.  */
	  for (j = 0; j < *decoded_options_count; ++j)
	    if ((*decoded_options)[j].opt_index == foption->opt_index)
	      break;
	  if (j == *decoded_options_count)
	    append_option (decoded_options, decoded_options_count, foption);
	  /* -fmath-errno > -fno-math-errno,
	     -fsigned-zeros > -fno-signed-zeros,
	     -ftrapping-math -> -fno-trapping-math,
	     -fwrapv > -fno-wrapv.  */
	  else if (foption->value > (*decoded_options)[j].value)
	    (*decoded_options)[j] = *foption;
	  break;

	case OPT_freg_struct_return:
	case OPT_fpcc_struct_return:
	case OPT_fshort_double:
	  for (j = 0; j < *decoded_options_count; ++j)
	    if ((*decoded_options)[j].opt_index == foption->opt_index)
	      break;
	  if (j == *decoded_options_count)
	    fatal_error (input_location,
			 "Option %s not used consistently in all LTO input"
			 " files", foption->orig_option_with_args_text);
	  break;

	case OPT_foffload_abi_:
	  for (j = 0; j < *decoded_options_count; ++j)
	    if ((*decoded_options)[j].opt_index == foption->opt_index)
	      break;
	  if (j == *decoded_options_count)
	    append_option (decoded_options, decoded_options_count, foption);
	  else if (foption->value != (*decoded_options)[j].value)
	    fatal_error (input_location,
			 "Option %s not used consistently in all LTO input"
			 " files", foption->orig_option_with_args_text);
	  break;

	case OPT_O:
	case OPT_Ofast:
	case OPT_Og:
	case OPT_Os:
	  for (j = 0; j < *decoded_options_count; ++j)
	    if ((*decoded_options)[j].opt_index == OPT_O
		|| (*decoded_options)[j].opt_index == OPT_Ofast
		|| (*decoded_options)[j].opt_index == OPT_Og
		|| (*decoded_options)[j].opt_index == OPT_Os)
	      break;
	  if (j == *decoded_options_count)
	    append_option (decoded_options, decoded_options_count, foption);
	  else if ((*decoded_options)[j].opt_index == foption->opt_index
		   && foption->opt_index != OPT_O)
	    /* Exact same options get merged.  */
	    ;
	  else
	    {
	      /* For mismatched option kinds preserve the optimization
	         level only, thus merge it as -On.  This also handles
		 merging of same optimization level -On.  */
	      int level = 0;
	      switch (foption->opt_index)
		{
		case OPT_O:
		  if (foption->arg[0] == '\0')
		    level = MAX (level, 1);
		  else
		    level = MAX (level, atoi (foption->arg));
		  break;
		case OPT_Ofast:
		  level = MAX (level, 3);
		  break;
		case OPT_Og:
		  level = MAX (level, 1);
		  break;
		case OPT_Os:
		  level = MAX (level, 2);
		  break;
		default:
		  gcc_unreachable ();
		}
	      switch ((*decoded_options)[j].opt_index)
		{
		case OPT_O:
		  if ((*decoded_options)[j].arg[0] == '\0')
		    level = MAX (level, 1);
		  else
		    level = MAX (level, atoi ((*decoded_options)[j].arg));
		  break;
		case OPT_Ofast:
		  level = MAX (level, 3);
		  break;
		case OPT_Og:
		  level = MAX (level, 1);
		  break;
		case OPT_Os:
		  level = MAX (level, 2);
		  break;
		default:
		  gcc_unreachable ();
		}
	      (*decoded_options)[j].opt_index = OPT_O;
	      char *tem;
	      tem = xasprintf ("-O%d", level);
	      (*decoded_options)[j].arg = &tem[2];
	      (*decoded_options)[j].canonical_option[0] = tem;
	      (*decoded_options)[j].value = 1;
	    }
	  break;

	case OPT_foffload_:
	  append_option (decoded_options, decoded_options_count, foption);
	  break;
	}
    }
}

/* Auxiliary function that frees elements of PTR and PTR itself.
   N is number of elements to be freed.  If PTR is NULL, nothing is freed.
   If an element is NULL, subsequent elements are not freed.  */

static void **
free_array_of_ptrs (void **ptr, unsigned n)
{
  if (!ptr)
    return NULL;
  for (unsigned i = 0; i < n; i++)
    {
      if (!ptr[i])
	break;
      free (ptr[i]);
    }
  free (ptr);
  return NULL;
}

/* Parse STR, saving found tokens into PVALUES and return their number.
   Tokens are assumed to be delimited by ':'.  If APPEND is non-null,
   append it to every token we find.  */

static unsigned
parse_env_var (const char *str, char ***pvalues, const char *append)
{
  const char *curval, *nextval;
  char **values;
  unsigned num = 1, i;

  curval = strchr (str, ':');
  while (curval)
    {
      num++;
      curval = strchr (curval + 1, ':');
    }

  values = (char**) xmalloc (num * sizeof (char*));
  curval = str;
  nextval = strchr (curval, ':');
  if (nextval == NULL)
    nextval = strchr (curval, '\0');

  int append_len = append ? strlen (append) : 0;
  for (i = 0; i < num; i++)
    {
      int l = nextval - curval;
      values[i] = (char*) xmalloc (l + 1 + append_len);
      memcpy (values[i], curval, l);
      values[i][l] = 0;
      if (append)
	strcat (values[i], append);
      curval = nextval + 1;
      nextval = strchr (curval, ':');
      if (nextval == NULL)
	nextval = strchr (curval, '\0');
    }
  *pvalues = values;
  return num;
}

/* Append options OPTS from lto or offload_lto sections to ARGV_OBSTACK.  */

static void
append_compiler_options (obstack *argv_obstack, struct cl_decoded_option *opts,
			 unsigned int count)
{
  /* Append compiler driver arguments as far as they were merged.  */
  for (unsigned int j = 1; j < count; ++j)
    {
      struct cl_decoded_option *option = &opts[j];

      /* File options have been properly filtered by lto-opts.c.  */
      switch (option->opt_index)
	{
	/* Drop arguments that we want to take from the link line.  */
	case OPT_flto_:
	case OPT_flto:
	case OPT_flto_partition_:
	  continue;

	default:
	  break;
	}

      /* For now do what the original LTO option code was doing - pass
	 on any CL_TARGET flag and a few selected others.  */
      switch (option->opt_index)
	{
	case OPT_fdiagnostics_show_caret:
	case OPT_fdiagnostics_show_option:
	case OPT_fdiagnostics_show_location_:
	case OPT_fshow_column:
	case OPT_fPIC:
	case OPT_fpic:
	case OPT_fPIE:
	case OPT_fpie:
	case OPT_fcommon:
	case OPT_fexceptions:
	case OPT_fnon_call_exceptions:
	case OPT_fgnu_tm:
	case OPT_freg_struct_return:
	case OPT_fpcc_struct_return:
	case OPT_fshort_double:
	case OPT_ffp_contract_:
	case OPT_fmath_errno:
	case OPT_fsigned_zeros:
	case OPT_ftrapping_math:
	case OPT_fwrapv:
	case OPT_fopenmp:
	case OPT_fopenacc:
	case OPT_ftrapv:
	case OPT_fstrict_overflow:
	case OPT_foffload_abi_:
	case OPT_O:
	case OPT_Ofast:
	case OPT_Og:
	case OPT_Os:
	case OPT_fcheck_pointer_bounds:
	  break;

	default:
	  if (!(cl_options[option->opt_index].flags & CL_TARGET))
	    continue;
	}

      /* Pass the option on.  */
      for (unsigned int i = 0; i < option->canonical_option_num_elements; ++i)
	obstack_ptr_grow (argv_obstack, option->canonical_option[i]);
    }
}

/* Append linker options OPTS to ARGV_OBSTACK.  */

static void
append_linker_options (obstack *argv_obstack, struct cl_decoded_option *opts,
		       unsigned int count)
{
  /* Append linker driver arguments.  Compiler options from the linker
     driver arguments will override / merge with those from the compiler.  */
  for (unsigned int j = 1; j < count; ++j)
    {
      struct cl_decoded_option *option = &opts[j];

      /* Do not pass on frontend specific flags not suitable for lto.  */
      if (!(cl_options[option->opt_index].flags
	    & (CL_COMMON|CL_TARGET|CL_DRIVER|CL_LTO)))
	continue;

      switch (option->opt_index)
	{
	case OPT_o:
	case OPT_flto_:
	case OPT_flto:
	  /* We've handled these LTO options, do not pass them on.  */
	  continue;

	case OPT_freg_struct_return:
	case OPT_fpcc_struct_return:
	case OPT_fshort_double:
	  /* Ignore these, they are determined by the input files.
	     ???  We fail to diagnose a possible mismatch here.  */
	  continue;

	default:
	  break;
	}

      /* Pass the option on.  */
      for (unsigned int i = 0; i < option->canonical_option_num_elements; ++i)
	obstack_ptr_grow (argv_obstack, option->canonical_option[i]);
    }
}

/* Extract options for TARGET offload compiler from OPTIONS and append
   them to ARGV_OBSTACK.  */

static void
append_offload_options (obstack *argv_obstack, const char *target,
			struct cl_decoded_option *options,
			unsigned int options_count)
{
  for (unsigned i = 0; i < options_count; i++)
    {
      const char *cur, *next, *opts;
      char **argv;
      unsigned argc;
      struct cl_decoded_option *option = &options[i];

      if (option->opt_index != OPT_foffload_)
	continue;

      /* If option argument starts with '-' then no target is specified.  That
	 means offload options are specified for all targets, so we need to
	 append them.  */
      if (option->arg[0] == '-')
	opts = option->arg;
      else
	{
	  opts = strchr (option->arg, '=');
	  /* If there are offload targets specified, but no actual options,
	     there is nothing to do here.  */
	  if (!opts)
	    continue;

	  cur = option->arg;

	  while (cur < opts)
	    {
	      next = strchr (cur, ',');
	      if (next == NULL)
		next = opts;
	      next = (next > opts) ? opts : next;

	      /* Are we looking for this offload target?  */
	      if (strlen (target) == (size_t) (next - cur)
		  && strncmp (target, cur, next - cur) == 0)
		break;

	      /* Skip the comma or equal sign.  */
	      cur = next + 1;
	    }

	  if (cur >= opts)
	    continue;

	  opts++;
	}

      argv = buildargv (opts);
      for (argc = 0; argv[argc]; argc++)
	obstack_ptr_grow (argv_obstack, argv[argc]);
    }
}

/* Check whether NAME can be accessed in MODE.  This is like access,
   except that it never considers directories to be executable.  */

static int
access_check (const char *name, int mode)
{
  if (mode == X_OK)
    {
      struct stat st;

      if (stat (name, &st) < 0
	  || S_ISDIR (st.st_mode))
	return -1;
    }

  return access (name, mode);
}

/* Prepare a target image for offload TARGET, using mkoffload tool from
   COMPILER_PATH.  Return the name of the resultant object file.  */

static char *
compile_offload_image (const char *target, const char *compiler_path,
		       unsigned in_argc, char *in_argv[],
		       struct cl_decoded_option *compiler_opts,
		       unsigned int compiler_opt_count,
		       struct cl_decoded_option *linker_opts,
		       unsigned int linker_opt_count)
{
  char *filename = NULL;
  char **argv;
  char *suffix
    = XALLOCAVEC (char, sizeof ("/accel//mkoffload") + strlen (target));
  strcpy (suffix, "/accel/");
  strcat (suffix, target);
  strcat (suffix, "/mkoffload");

  char **paths = NULL;
  unsigned n_paths = parse_env_var (compiler_path, &paths, suffix);

  const char *compiler = NULL;
  for (unsigned i = 0; i < n_paths; i++)
    if (access_check (paths[i], X_OK) == 0)
      {
	compiler = paths[i];
	break;
      }

  if (compiler)
    {
      /* Generate temporary output file name.  */
      filename = make_temp_file (".target.o");

      struct obstack argv_obstack;
      obstack_init (&argv_obstack);
      obstack_ptr_grow (&argv_obstack, compiler);
      if (save_temps)
	obstack_ptr_grow (&argv_obstack, "-save-temps");
      if (verbose)
	obstack_ptr_grow (&argv_obstack, "-v");
      obstack_ptr_grow (&argv_obstack, "-o");
      obstack_ptr_grow (&argv_obstack, filename);

      /* Append names of input object files.  */
      for (unsigned i = 0; i < in_argc; i++)
	obstack_ptr_grow (&argv_obstack, in_argv[i]);

      /* Append options from offload_lto sections.  */
      append_compiler_options (&argv_obstack, compiler_opts,
			       compiler_opt_count);

      /* Append options specified by -foffload last.  In case of conflicting
	 options we expect offload compiler to choose the latest.  */
      append_offload_options (&argv_obstack, target, compiler_opts,
			      compiler_opt_count);
      append_offload_options (&argv_obstack, target, linker_opts,
			      linker_opt_count);

      obstack_ptr_grow (&argv_obstack, NULL);
      argv = XOBFINISH (&argv_obstack, char **);
      fork_execute (argv[0], argv, true);
      obstack_free (&argv_obstack, NULL);
    }

  free_array_of_ptrs ((void **) paths, n_paths);
  return filename;
}


/* The main routine dealing with offloading.
   The routine builds a target image for each offload target.  IN_ARGC and
   IN_ARGV specify options and input object files.  As all of them could contain
   target sections, we pass them all to target compilers.  */

static void
compile_images_for_offload_targets (unsigned in_argc, char *in_argv[],
				    struct cl_decoded_option *compiler_opts,
				    unsigned int compiler_opt_count,
				    struct cl_decoded_option *linker_opts,
				    unsigned int linker_opt_count)
{
  char **names = NULL;
  const char *target_names = getenv (OFFLOAD_TARGET_NAMES_ENV);
  if (!target_names)
    return;
  unsigned num_targets = parse_env_var (target_names, &names, NULL);

  const char *compiler_path = getenv ("COMPILER_PATH");
  if (!compiler_path)
    goto out;

  /* Prepare an image for each target and save the name of the resultant object
     file to the OFFLOAD_NAMES array.  It is terminated by a NULL entry.  */
  offload_names = XCNEWVEC (char *, num_targets + 1);
  for (unsigned i = 0; i < num_targets; i++)
    {
      offload_names[i]
	= compile_offload_image (names[i], compiler_path, in_argc, in_argv,
				 compiler_opts, compiler_opt_count,
				 linker_opts, linker_opt_count);
      if (!offload_names[i])
	fatal_error (input_location,
		     "problem with building target image for %s\n", names[i]);
    }

 out:
  free_array_of_ptrs ((void **) names, num_targets);
}

/* Copy a file from SRC to DEST.  */

static void
copy_file (const char *dest, const char *src)
{
  FILE *d = fopen (dest, "wb");
  FILE *s = fopen (src, "rb");
  char buffer[512];
  while (!feof (s))
    {
      size_t len = fread (buffer, 1, 512, s);
      if (ferror (s) != 0)
	fatal_error (input_location, "reading input file");
      if (len > 0)
	{
	  fwrite (buffer, 1, len, d);
	  if (ferror (d) != 0)
	    fatal_error (input_location, "writing output file");
	}
    }
}

/* Find the crtoffloadbegin.o and crtoffloadend.o files in LIBRARY_PATH, make
   copies and store the names of the copies in offloadbegin and offloadend.  */

static void
find_offloadbeginend (void)
{
  char **paths = NULL;
  const char *library_path = getenv ("LIBRARY_PATH");
  if (!library_path)
    return;
  unsigned n_paths = parse_env_var (library_path, &paths, "/crtoffloadbegin.o");

  unsigned i;
  for (i = 0; i < n_paths; i++)
    if (access_check (paths[i], R_OK) == 0)
      {
	size_t len = strlen (paths[i]);
	char *tmp = xstrdup (paths[i]);
	strcpy (paths[i] + len - strlen ("begin.o"), "end.o");
	if (access_check (paths[i], R_OK) != 0)
	  fatal_error (input_location,
		       "installation error, can't find crtoffloadend.o");
	/* The linker will delete the filenames we give it, so make
	   copies.  */
	offloadbegin = make_temp_file (".o");
	offloadend = make_temp_file (".o");
	copy_file (offloadbegin, tmp);
	copy_file (offloadend, paths[i]);
	free (tmp);
	break;
      }
  if (i == n_paths)
    fatal_error (input_location,
		 "installation error, can't find crtoffloadbegin.o");

  free_array_of_ptrs ((void **) paths, n_paths);
}

/* A subroutine of run_gcc.  Examine the open file FD for lto sections with
   name prefix PREFIX, at FILE_OFFSET, and store any options we find in OPTS
   and OPT_COUNT.  Return true if we found a matchingn section, false
   otherwise.  COLLECT_GCC holds the value of the environment variable with
   the same name.  */

static bool
find_and_merge_options (int fd, off_t file_offset, const char *prefix,
			struct cl_decoded_option **opts,
			unsigned int *opt_count, const char *collect_gcc)
{
  off_t offset, length;
  char *data;
  char *fopts;
  const char *errmsg;
  int err;
  struct cl_decoded_option *fdecoded_options = *opts;
  unsigned int fdecoded_options_count = *opt_count;

  simple_object_read *sobj;
  sobj = simple_object_start_read (fd, file_offset, "__GNU_LTO",
				   &errmsg, &err);
  if (!sobj)
    return false;

  char *secname = XALLOCAVEC (char, strlen (prefix) + sizeof (".opts"));
  strcpy (secname, prefix);
  strcat (secname, ".opts");
  if (!simple_object_find_section (sobj, secname, &offset, &length,
				   &errmsg, &err))
    {
      simple_object_release_read (sobj);
      return false;
    }

  lseek (fd, file_offset + offset, SEEK_SET);
  data = (char *)xmalloc (length);
  read (fd, data, length);
  fopts = data;
  do
    {
      struct cl_decoded_option *f2decoded_options;
      unsigned int f2decoded_options_count;
      get_options_from_collect_gcc_options (collect_gcc,
					    fopts, CL_LANG_ALL,
					    &f2decoded_options,
					    &f2decoded_options_count);
      if (!fdecoded_options)
       {
	 fdecoded_options = f2decoded_options;
	 fdecoded_options_count = f2decoded_options_count;
       }
      else
	merge_and_complain (&fdecoded_options,
			    &fdecoded_options_count,
			    f2decoded_options, f2decoded_options_count);

      fopts += strlen (fopts) + 1;
    }
  while (fopts - data < length);

  free (data);
  simple_object_release_read (sobj);
  *opts = fdecoded_options;
  *opt_count = fdecoded_options_count;
  return true;
}

/* Execute gcc. ARGC is the number of arguments. ARGV contains the arguments. */

static void
run_gcc (unsigned argc, char *argv[])
{
  unsigned i, j;
  const char **new_argv;
  const char **argv_ptr;
  char *list_option_full = NULL;
  const char *linker_output = NULL;
  const char *collect_gcc, *collect_gcc_options;
  int parallel = 0;
  int jobserver = 0;
  bool no_partition = false;
  struct cl_decoded_option *fdecoded_options = NULL;
  struct cl_decoded_option *offload_fdecoded_options = NULL;
  unsigned int fdecoded_options_count = 0;
  unsigned int offload_fdecoded_options_count = 0;
  struct cl_decoded_option *decoded_options;
  unsigned int decoded_options_count;
  struct obstack argv_obstack;
  int new_head_argc;
  bool have_lto = false;
  bool have_offload = false;
  unsigned lto_argc = 0, offload_argc = 0;
  char **lto_argv, **offload_argv;

  /* Get the driver and options.  */
  collect_gcc = getenv ("COLLECT_GCC");
  if (!collect_gcc)
    fatal_error (input_location,
		 "environment variable COLLECT_GCC must be set");
  collect_gcc_options = getenv ("COLLECT_GCC_OPTIONS");
  if (!collect_gcc_options)
    fatal_error (input_location,
		 "environment variable COLLECT_GCC_OPTIONS must be set");
  get_options_from_collect_gcc_options (collect_gcc, collect_gcc_options,
					CL_LANG_ALL,
					&decoded_options,
					&decoded_options_count);

  /* Allocate arrays for input object files with LTO or offload IL,
     and for possible preceding arguments.  */
  lto_argv = XNEWVEC (char *, argc);
  offload_argv = XNEWVEC (char *, argc);

  /* Look at saved options in the IL files.  */
  for (i = 1; i < argc; ++i)
    {
      char *p;
      int fd;
      off_t file_offset = 0;
      long loffset;
      int consumed;
      char *filename = argv[i];

      if ((p = strrchr (argv[i], '@'))
	  && p != argv[i] 
	  && sscanf (p, "@%li%n", &loffset, &consumed) >= 1
	  && strlen (p) == (unsigned int) consumed)
	{
	  filename = XNEWVEC (char, p - argv[i] + 1);
	  memcpy (filename, argv[i], p - argv[i]);
	  filename[p - argv[i]] = '\0';
	  file_offset = (off_t) loffset;
	}
      fd = open (filename, O_RDONLY | O_BINARY);
      if (fd == -1)
	{
	  lto_argv[lto_argc++] = argv[i];
	  continue;
	}

      if (find_and_merge_options (fd, file_offset, LTO_SECTION_NAME_PREFIX,
				  &fdecoded_options, &fdecoded_options_count,
				  collect_gcc))
	{
	  have_lto = true;
	  lto_argv[lto_argc++] = argv[i];
	}

      if (find_and_merge_options (fd, file_offset, OFFLOAD_SECTION_NAME_PREFIX,
				  &offload_fdecoded_options,
				  &offload_fdecoded_options_count, collect_gcc))
	{
	  have_offload = true;
	  offload_argv[offload_argc++] = argv[i];
	}

      close (fd);
    }

  /* Initalize the common arguments for the driver.  */
  obstack_init (&argv_obstack);
  obstack_ptr_grow (&argv_obstack, collect_gcc);
  obstack_ptr_grow (&argv_obstack, "-xlto");
  obstack_ptr_grow (&argv_obstack, "-c");

  append_compiler_options (&argv_obstack, fdecoded_options,
			   fdecoded_options_count);
  append_linker_options (&argv_obstack, decoded_options, decoded_options_count);

  /* Scan linker driver arguments for things that are of relevance to us.  */
  for (j = 1; j < decoded_options_count; ++j)
    {
      struct cl_decoded_option *option = &decoded_options[j];
      switch (option->opt_index)
	{
	case OPT_o:
	  linker_output = option->arg;
	  break;

	case OPT_save_temps:
	  save_temps = 1;
	  break;

	case OPT_v:
	  verbose = 1;
	  break;

	case OPT_flto_partition_:
	  if (strcmp (option->arg, "none") == 0)
	    no_partition = true;
	  break;

	case OPT_flto_:
	  if (strcmp (option->arg, "jobserver") == 0)
	    {
	      jobserver = 1;
	      parallel = 1;
	    }
	  else
	    {
	      parallel = atoi (option->arg);
	      if (parallel <= 1)
		parallel = 0;
	    }
	  /* Fallthru.  */

	case OPT_flto:
	  lto_mode = LTO_MODE_WHOPR;
	  break;

	default:
	  break;
	}
    }

  if (no_partition)
    {
      lto_mode = LTO_MODE_LTO;
      jobserver = 0;
      parallel = 0;
    }

  if (linker_output)
    {
      char *output_dir, *base, *name;
      bool bit_bucket = strcmp (linker_output, HOST_BIT_BUCKET) == 0;

      output_dir = xstrdup (linker_output);
      base = output_dir;
      for (name = base; *name; name++)
	if (IS_DIR_SEPARATOR (*name))
	  base = name + 1;
      *base = '\0';

      linker_output = &linker_output[base - output_dir];
      if (*output_dir == '\0')
	{
	  static char current_dir[] = { '.', DIR_SEPARATOR, '\0' };
	  output_dir = current_dir;
	}
      if (!bit_bucket)
	{
	  obstack_ptr_grow (&argv_obstack, "-dumpdir");
	  obstack_ptr_grow (&argv_obstack, output_dir);
	}

      obstack_ptr_grow (&argv_obstack, "-dumpbase");
    }

  /* Remember at which point we can scrub args to re-use the commons.  */
  new_head_argc = obstack_object_size (&argv_obstack) / sizeof (void *);

  if (have_offload)
    {
      compile_images_for_offload_targets (offload_argc, offload_argv,
					  offload_fdecoded_options,
					  offload_fdecoded_options_count,
					  decoded_options,
					  decoded_options_count);
      if (offload_names)
	{
	  find_offloadbeginend ();
	  for (i = 0; offload_names[i]; i++)
	    printf ("%s\n", offload_names[i]);
	  free_array_of_ptrs ((void **) offload_names, i);
	}
    }

  if (offloadbegin)
    printf ("%s\n", offloadbegin);

  /* If object files contain offload sections, but do not contain LTO sections,
     then there is no need to perform a link-time recompilation, i.e.
     lto-wrapper is used only for a compilation of offload images.  */
  if (have_offload && !have_lto)
    {
      for (i = 1; i < argc; ++i)
	if (strncmp (argv[i], "-fresolution=", sizeof ("-fresolution=") - 1))
	  {
	    char *out_file;
	    /* Can be ".o" or ".so".  */
	    char *ext = strrchr (argv[i], '.');
	    if (ext == NULL)
	      out_file = make_temp_file ("");
	    else
	      out_file = make_temp_file (ext);
	    /* The linker will delete the files we give it, so make copies.  */
	    copy_file (out_file, argv[i]);
	    printf ("%s\n", out_file);
	  }
      goto finish;
    }

  if (lto_mode == LTO_MODE_LTO)
    {
      flto_out = make_temp_file (".lto.o");
      if (linker_output)
	obstack_ptr_grow (&argv_obstack, linker_output);
      obstack_ptr_grow (&argv_obstack, "-o");
      obstack_ptr_grow (&argv_obstack, flto_out);
    }
  else 
    {
      const char *list_option = "-fltrans-output-list=";
      size_t list_option_len = strlen (list_option);
      char *tmp;

      if (linker_output)
	{
	  char *dumpbase = (char *) xmalloc (strlen (linker_output)
					     + sizeof (".wpa") + 1);
	  strcpy (dumpbase, linker_output);
	  strcat (dumpbase, ".wpa");
	  obstack_ptr_grow (&argv_obstack, dumpbase);
	}

      if (linker_output && save_temps)
	{
	  ltrans_output_file = (char *) xmalloc (strlen (linker_output)
						 + sizeof (".ltrans.out") + 1);
	  strcpy (ltrans_output_file, linker_output);
	  strcat (ltrans_output_file, ".ltrans.out");
	}
      else
	ltrans_output_file = make_temp_file (".ltrans.out");
      list_option_full = (char *) xmalloc (sizeof (char) *
		         (strlen (ltrans_output_file) + list_option_len + 1));
      tmp = list_option_full;

      obstack_ptr_grow (&argv_obstack, tmp);
      strcpy (tmp, list_option);
      tmp += list_option_len;
      strcpy (tmp, ltrans_output_file);

      if (jobserver)
	obstack_ptr_grow (&argv_obstack, xstrdup ("-fwpa=jobserver"));
      else if (parallel > 1)
	{
	  char buf[256];
	  sprintf (buf, "-fwpa=%i", parallel);
	  obstack_ptr_grow (&argv_obstack, xstrdup (buf));
	}
      else
        obstack_ptr_grow (&argv_obstack, "-fwpa");
    }

  /* Append the input objects and possible preceding arguments.  */
  for (i = 0; i < lto_argc; ++i)
    obstack_ptr_grow (&argv_obstack, lto_argv[i]);
  obstack_ptr_grow (&argv_obstack, NULL);

  new_argv = XOBFINISH (&argv_obstack, const char **);
  argv_ptr = &new_argv[new_head_argc];
  fork_execute (new_argv[0], CONST_CAST (char **, new_argv), true);

  if (lto_mode == LTO_MODE_LTO)
    {
      printf ("%s\n", flto_out);
      free (flto_out);
      flto_out = NULL;
    }
  else
    {
      FILE *stream = fopen (ltrans_output_file, "r");
      FILE *mstream = NULL;
      struct obstack env_obstack;

      if (!stream)
	fatal_error (input_location, "fopen: %s: %m", ltrans_output_file);

      /* Parse the list of LTRANS inputs from the WPA stage.  */
      obstack_init (&env_obstack);
      nr = 0;
      for (;;)
	{
	  const unsigned piece = 32;
	  char *output_name = NULL;
	  char *buf, *input_name = (char *)xmalloc (piece);
	  size_t len;

	  buf = input_name;
cont:
	  if (!fgets (buf, piece, stream))
	    break;
	  len = strlen (input_name);
	  if (input_name[len - 1] != '\n')
	    {
	      input_name = (char *)xrealloc (input_name, len + piece);
	      buf = input_name + len;
	      goto cont;
	    }
	  input_name[len - 1] = '\0';

	  if (input_name[0] == '*')
	    output_name = &input_name[1];

	  nr++;
	  input_names = (char **)xrealloc (input_names, nr * sizeof (char *));
	  output_names = (char **)xrealloc (output_names, nr * sizeof (char *));
	  input_names[nr-1] = input_name;
	  output_names[nr-1] = output_name;
	}
      fclose (stream);
      maybe_unlink (ltrans_output_file);
      ltrans_output_file = NULL;

      if (parallel)
	{
	  makefile = make_temp_file (".mk");
	  mstream = fopen (makefile, "w");
	}

      /* Execute the LTRANS stage for each input file (or prepare a
	 makefile to invoke this in parallel).  */
      for (i = 0; i < nr; ++i)
	{
	  char *output_name;
	  char *input_name = input_names[i];
	  /* If it's a pass-through file do nothing.  */
	  if (output_names[i])
	    continue;

	  /* Replace the .o suffix with a .ltrans.o suffix and write
	     the resulting name to the LTRANS output list.  */
	  obstack_grow (&env_obstack, input_name, strlen (input_name) - 2);
	  obstack_grow (&env_obstack, ".ltrans.o", sizeof (".ltrans.o"));
	  output_name = XOBFINISH (&env_obstack, char *);

	  /* Adjust the dumpbase if the linker output file was seen.  */
	  if (linker_output)
	    {
	      char *dumpbase
		  = (char *) xmalloc (strlen (linker_output)
				      + sizeof (DUMPBASE_SUFFIX) + 1);
	      snprintf (dumpbase,
			strlen (linker_output) + sizeof (DUMPBASE_SUFFIX),
			"%s.ltrans%u", linker_output, i);
	      argv_ptr[0] = dumpbase;
	    }

	  argv_ptr[1] = "-fltrans";
	  argv_ptr[2] = "-o";
	  argv_ptr[3] = output_name;
	  argv_ptr[4] = input_name;
	  argv_ptr[5] = NULL;
	  if (parallel)
	    {
	      fprintf (mstream, "%s:\n\t@%s ", output_name, new_argv[0]);
	      for (j = 1; new_argv[j] != NULL; ++j)
		fprintf (mstream, " '%s'", new_argv[j]);
	      fprintf (mstream, "\n");
	      /* If we are not preserving the ltrans input files then
	         truncate them as soon as we have processed it.  This
		 reduces temporary disk-space usage.  */
	      if (! save_temps)
		fprintf (mstream, "\t@-touch -r %s %s.tem > /dev/null 2>&1 "
			 "&& mv %s.tem %s\n",
			 input_name, input_name, input_name, input_name); 
	    }
	  else
	    {
	      fork_execute (new_argv[0], CONST_CAST (char **, new_argv),
			    true);
	      maybe_unlink (input_name);
	    }

	  output_names[i] = output_name;
	}
      if (parallel)
	{
	  struct pex_obj *pex;
	  char jobs[32];

	  fprintf (mstream, "all:");
	  for (i = 0; i < nr; ++i)
	    fprintf (mstream, " \\\n\t%s", output_names[i]);
	  fprintf (mstream, "\n");
	  fclose (mstream);
	  if (!jobserver)
	    {
	      /* Avoid passing --jobserver-fd= and similar flags 
		 unless jobserver mode is explicitly enabled.  */
	      putenv (xstrdup ("MAKEFLAGS="));
	      putenv (xstrdup ("MFLAGS="));
	    }
	  new_argv[0] = getenv ("MAKE");
	  if (!new_argv[0])
	    new_argv[0] = "make";
	  new_argv[1] = "-f";
	  new_argv[2] = makefile;
	  i = 3;
	  if (!jobserver)
	    {
	      snprintf (jobs, 31, "-j%d", parallel);
	      new_argv[i++] = jobs;
	    }
	  new_argv[i++] = "all";
	  new_argv[i++] = NULL;
	  pex = collect_execute (new_argv[0], CONST_CAST (char **, new_argv),
				 NULL, NULL, PEX_SEARCH, false);
	  do_wait (new_argv[0], pex);
	  maybe_unlink (makefile);
	  makefile = NULL;
	  for (i = 0; i < nr; ++i)
	    maybe_unlink (input_names[i]);
	}
      for (i = 0; i < nr; ++i)
	{
	  fputs (output_names[i], stdout);
	  putc ('\n', stdout);
	  free (input_names[i]);
	}
      nr = 0;
      free (output_names);
      free (input_names);
      free (list_option_full);
      obstack_free (&env_obstack, NULL);
    }

 finish:
  if (offloadend)
    printf ("%s\n", offloadend);

  XDELETE (lto_argv);
  XDELETE (offload_argv);
  obstack_free (&argv_obstack, NULL);
}


/* Entry point.  */

int
main (int argc, char *argv[])
{
  const char *p;

  gcc_obstack_init (&opts_obstack);

  p = argv[0] + strlen (argv[0]);
  while (p != argv[0] && !IS_DIR_SEPARATOR (p[-1]))
    --p;
  progname = p;

  xmalloc_set_program_name (progname);

  gcc_init_libintl ();

  diagnostic_initialize (global_dc, 0);

  if (atexit (lto_wrapper_cleanup) != 0)
    fatal_error (input_location, "atexit failed");

  if (signal (SIGINT, SIG_IGN) != SIG_IGN)
    signal (SIGINT, fatal_signal);
#ifdef SIGHUP
  if (signal (SIGHUP, SIG_IGN) != SIG_IGN)
    signal (SIGHUP, fatal_signal);
#endif
  if (signal (SIGTERM, SIG_IGN) != SIG_IGN)
    signal (SIGTERM, fatal_signal);
#ifdef SIGPIPE
  if (signal (SIGPIPE, SIG_IGN) != SIG_IGN)
    signal (SIGPIPE, fatal_signal);
#endif
#ifdef SIGCHLD
  /* We *MUST* set SIGCHLD to SIG_DFL so that the wait4() call will
     receive the signal.  A different setting is inheritable */
  signal (SIGCHLD, SIG_DFL);
#endif

  /* We may be called with all the arguments stored in some file and
     passed with @file.  Expand them into argv before processing.  */
  expandargv (&argc, &argv);

  run_gcc (argc, argv);

  return 0;
}
