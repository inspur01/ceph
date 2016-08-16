/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB
*/

#include "ceph_fuse_opt.h"
//#include <fuse/fuse_misc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stddef.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <sys/param.h>

/* Define to the version of this package. */
#define FUSE_PACKAGE_VERSION "2.9.3"

enum  {
	CEPH_KEY_HELP,
	CEPH_KEY_HELP_NOHEADER,
	CEPH_KEY_VERSION,
};

struct ceph_helper_opts {
	int singlethread;
	int foreground;
	int nodefault_subtype;
	char *mountpoint;
};

#define CEPH_FUSE_HELPER_OPT(t, p) { t, offsetof(struct ceph_helper_opts, p), 1 }

static const struct ceph_fuse_opt ceph_fuse_helper_opts[] = {
	CEPH_FUSE_HELPER_OPT("-d",		foreground),
	CEPH_FUSE_HELPER_OPT("debug",	foreground),
	CEPH_FUSE_HELPER_OPT("-f",		foreground),
	CEPH_FUSE_HELPER_OPT("-s",		singlethread),
	CEPH_FUSE_HELPER_OPT("fsname=",	nodefault_subtype),
	CEPH_FUSE_HELPER_OPT("subtype=",	nodefault_subtype),

	CEPH_FUSE_OPT_KEY("-h",		CEPH_KEY_HELP),
	CEPH_FUSE_OPT_KEY("--help",		CEPH_KEY_HELP),
	CEPH_FUSE_OPT_KEY("-ho",		CEPH_KEY_HELP_NOHEADER),
	CEPH_FUSE_OPT_KEY("-V",		CEPH_KEY_VERSION),
	CEPH_FUSE_OPT_KEY("--version",	CEPH_KEY_VERSION),
	CEPH_FUSE_OPT_KEY("-d",		FUSE_OPT_KEY_KEEP),
	CEPH_FUSE_OPT_KEY("debug",		FUSE_OPT_KEY_KEEP),
	CEPH_FUSE_OPT_KEY("fsname=",		FUSE_OPT_KEY_KEEP),
	CEPH_FUSE_OPT_KEY("subtype=",	FUSE_OPT_KEY_KEEP),
	FUSE_OPT_END
};

struct ceph_fuse_opt_context {
	void *data;
	const struct ceph_fuse_opt *opt;
	ceph_fuse_opt_proc_t proc;
	int argctr;
	int argc;
	char **argv;
	struct fuse_args outargs;
	char *opts;
	int nonopt;
};

void ceph_fuse_opt_free_args(struct fuse_args *args)
{
	if (args) {
		if (args->argv && args->allocated) {
			int i;
			for (i = 0; i < args->argc; i++)
				free(args->argv[i]);
			free(args->argv);
		}
		args->argc = 0;
		args->argv = NULL;
		args->allocated = 0;
	}
}

static int alloc_failed(void)
{
	fprintf(stderr, "fuse: memory allocation failed\n");
	return -1;
}

int ceph_fuse_opt_add_arg(struct fuse_args *args, const char *arg)
{
	char **newargv;
	char *newarg;

	assert(!args->argv || args->allocated);

	newarg = strdup(arg);
	if (!newarg)
		return alloc_failed();

	newargv = realloc(args->argv, (args->argc + 2) * sizeof(char *));
	if (!newargv) {
		free(newarg);
		return alloc_failed();
	}

	args->argv = newargv;
	args->allocated = 1;
	args->argv[args->argc++] = newarg;
	args->argv[args->argc] = NULL;
	return 0;
}

static int ceph_fuse_opt_insert_arg_common(struct fuse_args *args, int pos,
				      const char *arg)
{
	assert(pos <= args->argc);
	if (ceph_fuse_opt_add_arg(args, arg) == -1)
		return -1;

	if (pos != args->argc - 1) {
		char *newarg = args->argv[args->argc - 1];
		memmove(&args->argv[pos + 1], &args->argv[pos],
			sizeof(char *) * (args->argc - pos - 1));
		args->argv[pos] = newarg;
	}
	return 0;
}

int ceph_fuse_opt_insert_arg(struct fuse_args *args, int pos, const char *arg)
{
	return ceph_fuse_opt_insert_arg_common(args, pos, arg);
}

int ceph_fuse_opt_insert_arg_compat(struct fuse_args *args, int pos,
			       const char *arg);
int ceph_fuse_opt_insert_arg_compat(struct fuse_args *args, int pos, const char *arg)
{
	return ceph_fuse_opt_insert_arg_common(args, pos, arg);
}

static int ceph_next_arg(struct ceph_fuse_opt_context *ctx, const char *opt)
{
	if (ctx->argctr + 1 >= ctx->argc) {
		fprintf(stderr, "fuse: missing argument after `%s'\n", opt);
		return -1;
	}
	ctx->argctr++;
	return 0;
}

static int ceph_add_arg(struct ceph_fuse_opt_context *ctx, const char *arg)
{
	return ceph_fuse_opt_add_arg(&ctx->outargs, arg);
}

static int ceph_add_opt_common(char **opts, const char *opt, int esc)
{
	unsigned oldlen = *opts ? strlen(*opts) : 0;
	char *d = realloc(*opts, oldlen + 1 + strlen(opt) * 2 + 1);

	if (!d)
		return alloc_failed();

	*opts = d;
	if (oldlen) {
		d += oldlen;
		*d++ = ',';
	}

	for (; *opt; opt++) {
		if (esc && (*opt == ',' || *opt == '\\'))
			*d++ = '\\';
		*d++ = *opt;
	}
	*d = '\0';

	return 0;
}

int ceph_fuse_opt_add_opt(char **opts, const char *opt)
{
	return ceph_add_opt_common(opts, opt, 0);
}

int ceph_fuse_opt_add_opt_escaped(char **opts, const char *opt)
{
	return ceph_add_opt_common(opts, opt, 1);
}

static int ceph_add_opt(struct ceph_fuse_opt_context *ctx, const char *opt)
{
	return ceph_add_opt_common(&ctx->opts, opt, 1);
}

static int ceph_call_proc(struct ceph_fuse_opt_context *ctx, const char *arg, int key,
		     int iso)
{
	if (key == CEPH_FUSE_OPT_KEY_DISCARD)
		return 0;

	if (key != CEPH_FUSE_OPT_KEY_KEEP && ctx->proc) {
		int res = ctx->proc(ctx->data, arg, key, &ctx->outargs);
		if (res == -1 || !res)
			return res;
	}
	if (iso)
		return ceph_add_opt(ctx, arg);
	else
		return ceph_add_arg(ctx, arg);
}

static int match_template(const char *t, const char *arg, unsigned *sepp)
{
	int arglen = strlen(arg);
	const char *sep = strchr(t, '=');
	sep = sep ? sep : strchr(t, ' ');
	if (sep && (!sep[1] || sep[1] == '%')) {
		int tlen = sep - t;
		if (sep[0] == '=')
			tlen ++;
		if (arglen >= tlen && strncmp(arg, t, tlen) == 0) {
			*sepp = sep - t;
			return 1;
		}
	}
	if (strcmp(t, arg) == 0) {
		*sepp = 0;
		return 1;
	}
	return 0;
}

static const struct ceph_fuse_opt *find_opt(const struct ceph_fuse_opt *opt,
				       const char *arg, unsigned *sepp)
{
	for (; opt && opt->templ; opt++)
		if (match_template(opt->templ, arg, sepp))
			return opt;
	return NULL;
}

int ceph_fuse_opt_match(const struct ceph_fuse_opt *opts, const char *opt)
{
	unsigned dummy;
	return find_opt(opts, opt, &dummy) ? 1 : 0;
}

static int process_opt_param(void *var, const char *format, const char *param,
			     const char *arg)
{
	assert(format[0] == '%');
	if (format[1] == 's') {
		char *copy = strdup(param);
		if (!copy)
			return alloc_failed();

		*(char **) var = copy;
	} else {
		if (sscanf(param, format, var) != 1) {
			fprintf(stderr, "fuse: invalid parameter in option `%s'\n", arg);
			return -1;
		}
	}
	return 0;
}

static int process_opt(struct ceph_fuse_opt_context *ctx,
		       const struct ceph_fuse_opt *opt, unsigned sep,
		       const char *arg, int iso)
{
	if (opt->offset == -1U) {
		if (ceph_call_proc(ctx, arg, opt->value, iso) == -1)
			return -1;
	} else {
		void *var = ctx->data + opt->offset;
		if (sep && opt->templ[sep + 1]) {
			const char *param = arg + sep;
			if (opt->templ[sep] == '=')
				param ++;
			if (process_opt_param(var, opt->templ + sep + 1,
					      param, arg) == -1)
				return -1;
		} else
			*(int *)var = opt->value;
	}
	return 0;
}

static int process_opt_sep_arg(struct ceph_fuse_opt_context *ctx,
			       const struct ceph_fuse_opt *opt, unsigned sep,
			       const char *arg, int iso)
{
	int res;
	char *newarg;
	char *param;

	if (ceph_next_arg(ctx, arg) == -1)
		return -1;

	param = ctx->argv[ctx->argctr];
	newarg = malloc(sep + strlen(param) + 1);
	if (!newarg)
		return alloc_failed();

	memcpy(newarg, arg, sep);
	strcpy(newarg + sep, param);
	res = process_opt(ctx, opt, sep, newarg, iso);
	free(newarg);

	return res;
}

static int process_gopt(struct ceph_fuse_opt_context *ctx, const char *arg, int iso)
{
	unsigned sep;
	const struct ceph_fuse_opt *opt = find_opt(ctx->opt, arg, &sep);
	if (opt) {
		for (; opt; opt = find_opt(opt + 1, arg, &sep)) {
			int res;
			if (sep && opt->templ[sep] == ' ' && !arg[sep])
				res = process_opt_sep_arg(ctx, opt, sep, arg,
							  iso);
			else
				res = process_opt(ctx, opt, sep, arg, iso);
			if (res == -1)
				return -1;
		}
		return 0;
	} else
		return ceph_call_proc(ctx, arg, CEPH_FUSE_OPT_KEY_OPT, iso);
}

static int ceph_process_real_option_group(struct ceph_fuse_opt_context *ctx, char *opts)
{
	char *s = opts;
	char *d = s;
	int end = 0;

	while (!end) {
		if (*s == '\0')
			end = 1;
		if (*s == ',' || end) {
			int res;

			*d = '\0';
			res = process_gopt(ctx, opts, 1);
			if (res == -1)
				return -1;
			d = opts;
		} else {
			if (s[0] == '\\' && s[1] != '\0') {
				s++;
				if (s[0] >= '0' && s[0] <= '3' &&
				    s[1] >= '0' && s[1] <= '7' &&
				    s[2] >= '0' && s[2] <= '7') {
					*d++ = (s[0] - '0') * 0100 +
						(s[1] - '0') * 0010 +
						(s[2] - '0');
					s += 2;
				} else {
					*d++ = *s;
				}
			} else {
				*d++ = *s;
			}
		}
		s++;
	}

	return 0;
}

static int ceph_process_option_group(struct ceph_fuse_opt_context *ctx, const char *opts)
{
	int res;
	char *copy = strdup(opts);

	if (!copy) {
		fprintf(stderr, "fuse: memory allocation failed\n");
		return -1;
	}
	res = ceph_process_real_option_group(ctx, copy);
	free(copy);
	return res;
}

static int ceph_process_one(struct ceph_fuse_opt_context *ctx, const char *arg)
{
	if (ctx->nonopt || arg[0] != '-')
		return ceph_call_proc(ctx, arg, CEPH_FUSE_OPT_KEY_NONOPT, 0);
	else if (arg[1] == 'o') {
		if (arg[2])
			return ceph_process_option_group(ctx, arg + 2);
		else {
			if (ceph_next_arg(ctx, arg) == -1)
				return -1;

			return ceph_process_option_group(ctx,
						    ctx->argv[ctx->argctr]);
		}
	} else if (arg[1] == '-' && !arg[2]) {
		if (ceph_add_arg(ctx, arg) == -1)
			return -1;
		ctx->nonopt = ctx->outargs.argc;
		return 0;
	} else
		return process_gopt(ctx, arg, 0);
}

static int ceph_opt_parse(struct ceph_fuse_opt_context *ctx)
{
	if (ctx->argc) {
		if (ceph_add_arg(ctx, ctx->argv[0]) == -1)
			return -1;
	}

	for (ctx->argctr = 1; ctx->argctr < ctx->argc; ctx->argctr++)
		if (ceph_process_one(ctx, ctx->argv[ctx->argctr]) == -1)
			return -1;

	if (ctx->opts) {
		if (ceph_fuse_opt_insert_arg(&ctx->outargs, 1, "-o") == -1 ||
		    ceph_fuse_opt_insert_arg(&ctx->outargs, 2, ctx->opts) == -1)
			return -1;
	}

	/* If option separator ("--") is the last argument, remove it */
	if (ctx->nonopt && ctx->nonopt == ctx->outargs.argc &&
	    strcmp(ctx->outargs.argv[ctx->outargs.argc - 1], "--") == 0) {
		free(ctx->outargs.argv[ctx->outargs.argc - 1]);
		ctx->outargs.argv[--ctx->outargs.argc] = NULL;
	}

	return 0;
}

int ceph_fuse_opt_parse(struct fuse_args *args, void *data,
		   const struct ceph_fuse_opt opts[], ceph_fuse_opt_proc_t proc)
{
	int res;
	struct ceph_fuse_opt_context ctx = {
		.data = data,
		.opt = opts,
		.proc = proc,
	};

	if (!args || !args->argv || !args->argc)
		return 0;

	ctx.argc = args->argc;
	ctx.argv = args->argv;

	res = ceph_opt_parse(&ctx);
	if (res != -1) {
		struct fuse_args tmp = *args;
		*args = ctx.outargs;
		ctx.outargs = tmp;
	}
	free(ctx.opts);
	ceph_fuse_opt_free_args(&ctx.outargs);
	return res;
}

static void ceph_usage(const char *progname)
{
	fprintf(stderr,
		"ceph_usage: %s mountpoint [options]\n\n", progname);
	fprintf(stderr,
		"general options:\n"
		"    -o opt,[opt...]        mount options\n"
		"    -h   --help            print help\n"
		"    -V   --version         print version\n"
		"\n");
}

static void ceph_helper_help(void)
{
	fprintf(stderr,
		"FUSE options:\n"
		"    -d   -o debug          enable debug output (implies -f)\n"
		"    -f                     foreground operation\n"
		"    -s                     disable multi-threaded operation\n"
		"\n"
		);
}

static void ceph_helper_version(void)
{
	fprintf(stderr, "FUSE library version: %s\n", FUSE_PACKAGE_VERSION);
}

static int ceph_fuse_helper_opt_proc(void *data, const char *arg, int key,
				struct fuse_args *outargs)
{
	struct ceph_helper_opts *hopts = data;

	switch (key) {
	case CEPH_KEY_HELP:
		ceph_usage(outargs->argv[0]);
		/* fall through */

	case CEPH_KEY_HELP_NOHEADER:
		ceph_helper_help();
		return ceph_fuse_opt_add_arg(outargs, "-h");

	case CEPH_KEY_VERSION:
		ceph_helper_version();
		return 1;

	case FUSE_OPT_KEY_NONOPT:
		if (!hopts->mountpoint) {
			char mountpoint[PATH_MAX];
			if (realpath(arg, mountpoint) == NULL) {
				fprintf(stderr,
					"fuse: bad mount point `%s': %s\n",
					arg, strerror(errno));
				return -1;
			}
			return ceph_fuse_opt_add_opt(&hopts->mountpoint, mountpoint);
		} else {
			fprintf(stderr, "fuse: invalid argument `%s'\n", arg);
			return -1;
		}

	default:
		return 1;
	}
}


/* This symbol version was mistakenly added to the version script */
//CEPH_USR_SYMVER(".symver ceph_fuse_opt_insert_arg_compat,ceph_fuse_opt_insert_arg@ceph_fuse_2.5");

