/*	$OpenBSD: util.c,v 1.39 2010/07/02 22:18:03 tedu Exp $	*/

/*-
 * Copyright (c) 1999 James Howard and Dag-Erling Co�dan Sm�rgrav
 * Copyright (C) 2008-2010 Gabor Kovesdan <gabor@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/stat.h>
#include <sys/types.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fnmatch.h>
#include <fts.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include "grep.h"

static int	 linesqueued;
static int	 procline(struct str *l, int);

/*
 * Processes a directory when a recursive search is performed with
 * the -R option.  Each appropriate file is passed to procfile().
 */
int
grep_tree(char **argv)
{
	FTS *fts;
	FTSENT *p;
	char *d, *dir;
	unsigned int i;
	int c, fts_flags;
	bool ok;

	c = fts_flags = 0;

	switch(linkbehave) {
	case LINK_EXPLICIT:
		fts_flags = FTS_COMFOLLOW;
		break;
	case LINK_SKIP:
		fts_flags = FTS_PHYSICAL;
		break;
	default:
		fts_flags = FTS_LOGICAL;
			
	}

	fts_flags |= FTS_NOSTAT | FTS_NOCHDIR;

	if (!(fts = fts_open(argv, fts_flags, NULL)))
		err(2, NULL);
	while ((p = fts_read(fts)) != NULL) {
		switch (p->fts_info) {
		case FTS_DNR:
			/* FALLTHROUGH */
		case FTS_ERR:
			errx(2, "%s: %s", p->fts_path, strerror(p->fts_errno));
			break;
		case FTS_D:
			/* FALLTHROUGH */
		case FTS_DP:
			break;
		case FTS_DC:
			/* Print a warning for recursive directory loop */
			warnx("warning: %s: recursive directory loop",
				p->fts_path);
			break;
		default:
			/* Check for file exclusion/inclusion */
			ok = true;
			if (exclflag) {
				d = strrchr(p->fts_path, '/');
				dir = grep_malloc(sizeof(char) *
				    (d - p->fts_path + 2));
				strlcpy(dir, p->fts_path,
				    (d - p->fts_path + 1));
				for (i = 0; i < epatterns; ++i) {
					switch(epattern[i].type) {
					case FILE_PAT:
						if (fnmatch(epattern[i].pat,
						    basename(p->fts_path), 0) == 0)
							ok = epattern[i].mode != EXCL_PAT;
						break;
					case DIR_PAT:
						if (strstr(dir,
						    epattern[i].pat) != NULL)
							ok = epattern[i].mode != EXCL_PAT;
						break;
					}
				}
			free(dir);
			}

			if (ok)
				c += procfile(p->fts_path);
			break;
		}
	}

	return (c);
}

/*
 * Opens a file and processes it.  Each file is processed line-by-line
 * passing the lines to procline().
 */
int
procfile(const char *fn)
{
	struct file *f;
	struct stat sb;
	struct str ln;
	mode_t s;
	int c, t;

	if (mflag && (mcount <= 0))
		return (0);

	if (strcmp(fn, "-") == 0) {
		fn = label != NULL ? label : getstr(1);
		f = grep_stdin_open();
	} else {
		if (!stat(fn, &sb)) {
			/* Check if we need to process the file */
			s = sb.st_mode & S_IFMT;
			if (s == S_IFDIR && dirbehave == DIR_SKIP)
				return (0);
			if ((s == S_IFIFO || s == S_IFCHR || s == S_IFBLK
				|| s == S_IFSOCK) && devbehave == DEV_SKIP)
					return (0);
		}
		f = grep_open(fn);
	}
	if (f == NULL) {
		if (!sflag)
			warn("%s", fn);
		if (errno == ENOENT)
			notfound = true;
		return (0);
	}

	ln.file = grep_malloc(strlen(fn) + 1);
	strcpy(ln.file, fn);
	ln.line_no = 0;
	ln.len = 0;
	linesqueued = 0;
	tail = 0;
	ln.off = -1;

	for (c = 0;  c == 0 || !(lflag || qflag); ) {
		ln.off += ln.len + 1;
		if ((ln.dat = grep_fgetln(f, &ln.len)) == NULL) {
			if (ln.line_no == 0 && matchall)
				exit(0);
			else
				break;
		}
		if (ln.len > 0 && ln.dat[ln.len - 1] == '\n')
			--ln.len;
		ln.line_no++;

		/* Return if we need to skip a binary file */
		if (f->binary && binbehave == BINFILE_SKIP) {
			grep_close(f);
			free(f);
			return (0);
		}
		/* Process the file line-by-line */
		if ((t = procline(&ln, f->binary)) == 0 && Bflag > 0) {
			enqueue(&ln);
			linesqueued++;
		}
		c += t;

		/* Count the matches if we have a match limit */
		if (mflag) {
			mcount -= t;
			if (mcount <= 0)
				break;
		}
	}
	if (Bflag > 0)
		clearqueue();
	grep_close(f);

	if (cflag) {
		if (!hflag)
			printf("%s:", ln.file);
		printf("%u\n", c);
	}
	if (lflag && c != 0)
		printf("%s\n", fn);
	if (Lflag && c == 0)
		printf("%s\n", fn);
	if (c && !cflag && !lflag && !Lflag &&
	    binbehave == BINFILE_BIN && f->binary && !qflag)
		printf(getstr(9), fn);

	free(f);
	return (c);
}

#define iswword(x)	(iswalnum((x)) || (x) == L'_')

/*
 * Processes a line comparing it with the specified patterns.  Each pattern
 * is looped to be compared along with the full string, saving each and every
 * match, which is necessary to colorize the output and to count the
 * matches.  The matching lines are passed to printline() to display the
 * appropriate output.
 */
static int
procline(struct str *l, int nottext)
{
	regmatch_t matches[MAX_LINE_MATCHES];
	regmatch_t pmatch;
	size_t st = 0;
	unsigned int i;
	int c = 0, m = 0, r = 0;

	if (!matchall) {
		/* Loop to process the whole line */
		while (st <= l->len) {
			pmatch.rm_so = st;
			pmatch.rm_eo = l->len;

			/* Loop to compare with all the patterns */
			for (i = 0; i < patterns; i++) {
/*
 * XXX: grep_search() is a workaround for speed up and should be
 * removed in the future.  See fastgrep.c.
 */
				if (fg_pattern[i].pattern) {
					r = grep_search(&fg_pattern[i],
					    (unsigned char *)l->dat,
					    l->len, &pmatch);
					r = (r == 0) ? 0 : REG_NOMATCH;
					st = pmatch.rm_eo;
				} else {
					r = regexec(&r_pattern[i], l->dat, 1,
					    &pmatch, eflags);
					r = (r == 0) ? 0 : REG_NOMATCH;
					st = pmatch.rm_eo;
				}
				if (r == REG_NOMATCH)
					continue;
				/* Check for full match */
				if (r == 0 && xflag)
					if (pmatch.rm_so != 0 ||
					    (size_t)pmatch.rm_eo != l->len)
						r = REG_NOMATCH;
				/* Check for whole word match */
				if (r == 0 && wflag && pmatch.rm_so != 0 &&
				    (size_t)pmatch.rm_eo != l->len) {
					wchar_t *wbegin;
					wint_t wend;
					size_t size;

					size = mbstowcs(NULL, l->dat,
					    pmatch.rm_so);

					if (size == ((size_t) - 1))
						r = REG_NOMATCH;
					else {
						wbegin = grep_malloc(size);
						if (mbstowcs(wbegin, l->dat,
						    pmatch.rm_so) == ((size_t) - 1))
							r = REG_NOMATCH;
						else if (sscanf(&l->dat[pmatch.rm_eo],
						    "%lc", &wend) != 1)
							r = REG_NOMATCH;
						else if (iswword(wbegin[wcslen(wbegin)]) ||
						    iswword(wend))
							r = REG_NOMATCH;
						free(wbegin);
					}
				}
				if (r == 0) {
					if (m == 0)
						c++;
					if (m < MAX_LINE_MATCHES)
						matches[m++] = pmatch;
					/* matches - skip further patterns */
					break;
				}
			}

			if (vflag) {
				c = !c;
				break;
			}
			/* One pass if we are not recording matches */
			if (!oflag && !color)
				break;

			if (st == (size_t)pmatch.rm_so)
				break; 	/* No matches */
		}
	} else
		c = !vflag;

	if (c && binbehave == BINFILE_BIN && nottext)
		return (c); /* Binary file */

	/* Dealing with the context */
	if ((tail || c) && !cflag && !qflag) {
		if (c) {
			if (!first && !prev && !tail && Aflag)
				printf("--\n");
			tail = Aflag;
			if (Bflag > 0) {
				if (!first && !prev)
					printf("--\n");
				printqueue();
			}
			linesqueued = 0;
			printline(l, ':', matches, m);
		} else {
			printline(l, '-', matches, m);
			tail--;
		}
	}

	if (c) {
		prev = true;
		first = false;
	} else
		prev = false;

	return (c);
}

/*
 * Safe malloc() for internal use.
 */
void *
grep_malloc(size_t size)
{
	void *ptr;

	if ((ptr = malloc(size)) == NULL)
		err(2, "malloc");
	return (ptr);
}

/*
 * Safe calloc() for internal use.
 */
void *
grep_calloc(size_t nmemb, size_t size)
{
	void *ptr;

	if ((ptr = calloc(nmemb, size)) == NULL)
		err(2, "calloc");
	return (ptr);
}

/*
 * Safe realloc() for internal use.
 */
void *
grep_realloc(void *ptr, size_t size)
{

	if ((ptr = realloc(ptr, size)) == NULL)
		err(2, "realloc");
	return (ptr);
}

/*
 * Prints a matching line according to the command line options.
 */
void
printline(struct str *line, int sep, regmatch_t *matches, int m)
{
	size_t a = 0;
	int i, n = 0;

	if (!hflag) {
		if (nullflag == 0)
			fputs(line->file, stdout);
		else {
			printf("%s", line->file);
			putchar(0);
		}
		++n;
	}
	if (nflag) {
		if (n > 0)
			putchar(sep);
		printf("%d", line->line_no);
		++n;
	}
	if (bflag) {
		if (n > 0)
			putchar(sep);
		printf("%lld", (long long)line->off);
		++n;
	}
	if (n)
		putchar(sep);
	/* --color and -o */
	if ((oflag || color) && m > 0) {
		for (i = 0; i < m; i++) {
			if (!oflag)
				fwrite(line->dat + a, matches[i].rm_so - a, 1,
				    stdout);
			if (color) 
				fprintf(stdout, "\33[%sm\33[K", color);

				fwrite(line->dat + matches[i].rm_so, 
				    matches[i].rm_eo - matches[i].rm_so, 1,
				    stdout);
			if (color) 
				fprintf(stdout, "\33[m\33[K");
			a = matches[i].rm_eo;
			if (oflag)
				putchar('\n');
		}
		if (!oflag) {
			if (line->len - a > 0)
				fwrite(line->dat + a, line->len - a, 1, stdout);
			putchar('\n');
		}
	} else {
		fwrite(line->dat, line->len, 1, stdout);
		putchar('\n');
	}
}
