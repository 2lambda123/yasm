/*
 * Incbin bytecode
 *
 *  Copyright (C) 2001-2006  Peter Johnson
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND OTHER CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR OTHER CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#define YASM_LIB_INTERNAL
#include "util.h"
/*@unused@*/ RCSID("$Id$");

#include "coretype.h"

#include "errwarn.h"
#include "intnum.h"
#include "expr.h"
#include "value.h"

#include "bytecode.h"

#include "bc-int.h"


typedef struct bytecode_incbin {
    /*@only@*/ char *filename;		/* file to include data from */

    /* starting offset to read from (NULL=0) */
    /*@only@*/ /*@null@*/ yasm_expr *start;

    /* maximum number of bytes to read (NULL=no limit) */
    /*@only@*/ /*@null@*/ yasm_expr *maxlen;
} bytecode_incbin;

static void bc_incbin_destroy(void *contents);
static void bc_incbin_print(const void *contents, FILE *f, int indent_level);
static void bc_incbin_finalize(yasm_bytecode *bc, yasm_bytecode *prev_bc);
static yasm_bc_resolve_flags bc_incbin_resolve
    (yasm_bytecode *bc, int save, yasm_calc_bc_dist_func calc_bc_dist);
static int bc_incbin_tobytes(yasm_bytecode *bc, unsigned char **bufp, void *d,
			     yasm_output_value_func output_value,
			     /*@null@*/ yasm_output_reloc_func output_reloc);

static const yasm_bytecode_callback bc_incbin_callback = {
    bc_incbin_destroy,
    bc_incbin_print,
    bc_incbin_finalize,
    bc_incbin_resolve,
    bc_incbin_tobytes,
    0
};


static void
bc_incbin_destroy(void *contents)
{
    bytecode_incbin *incbin = (bytecode_incbin *)contents;
    yasm_xfree(incbin->filename);
    yasm_expr_destroy(incbin->start);
    yasm_expr_destroy(incbin->maxlen);
    yasm_xfree(contents);
}

static void
bc_incbin_print(const void *contents, FILE *f, int indent_level)
{
    const bytecode_incbin *incbin = (const bytecode_incbin *)contents;
    fprintf(f, "%*s_IncBin_\n", indent_level, "");
    fprintf(f, "%*sFilename=`%s'\n", indent_level, "",
	    incbin->filename);
    fprintf(f, "%*sStart=", indent_level, "");
    if (!incbin->start)
	fprintf(f, "nil (0)");
    else
	yasm_expr_print(incbin->start, f);
    fprintf(f, "%*sMax Len=", indent_level, "");
    if (!incbin->maxlen)
	fprintf(f, "nil (unlimited)");
    else
	yasm_expr_print(incbin->maxlen, f);
    fprintf(f, "\n");
}

static void
bc_incbin_finalize(yasm_bytecode *bc, yasm_bytecode *prev_bc)
{
    bytecode_incbin *incbin = (bytecode_incbin *)bc->contents;
    yasm_value val;

    if (yasm_value_finalize_expr(&val, incbin->start, 0))
	yasm_error_set(YASM_ERROR_TOO_COMPLEX,
		       N_("start expression too complex"));
    else if (val.rel)
	yasm_error_set(YASM_ERROR_NOT_ABSOLUTE,
		       N_("start expression not absolute"));
    incbin->start = val.abs;

    if (yasm_value_finalize_expr(&val, incbin->maxlen, 0))
	yasm_error_set(YASM_ERROR_TOO_COMPLEX,
		       N_("maximum length expression too complex"));
    else if (val.rel)
	yasm_error_set(YASM_ERROR_NOT_ABSOLUTE,
		       N_("maximum length expression not absolute"));
    incbin->maxlen = val.abs;
}

static yasm_bc_resolve_flags
bc_incbin_resolve(yasm_bytecode *bc, int save,
		  yasm_calc_bc_dist_func calc_bc_dist)
{
    bytecode_incbin *incbin = (bytecode_incbin *)bc->contents;
    FILE *f;
    /*@null@*/ yasm_expr *temp;
    yasm_expr **tempp;
    /*@dependent@*/ /*@null@*/ const yasm_intnum *num;
    unsigned long start = 0, maxlen = 0xFFFFFFFFUL, flen;

    /* Try to convert start to integer value */
    if (incbin->start) {
	if (save) {
	    temp = NULL;
	    tempp = &incbin->start;
	} else {
	    temp = yasm_expr_copy(incbin->start);
	    assert(temp != NULL);
	    tempp = &temp;
	}
	num = yasm_expr_get_intnum(tempp, calc_bc_dist);
	if (num)
	    start = yasm_intnum_get_uint(num);
	yasm_expr_destroy(temp);
	if (!num)
	    return YASM_BC_RESOLVE_UNKNOWN_LEN;
    }

    /* Try to convert maxlen to integer value */
    if (incbin->maxlen) {
	if (save) {
	    temp = NULL;
	    tempp = &incbin->maxlen;
	} else {
	    temp = yasm_expr_copy(incbin->maxlen);
	    assert(temp != NULL);
	    tempp = &temp;
	}
	num = yasm_expr_get_intnum(tempp, calc_bc_dist);
	if (num)
	    maxlen = yasm_intnum_get_uint(num);
	yasm_expr_destroy(temp);
	if (!num)
	    return YASM_BC_RESOLVE_UNKNOWN_LEN;
    }

    /* FIXME: Search include path for filename.  Save full path back into
     * filename if save is true.
     */

    /* Open file and determine its length */
    f = fopen(incbin->filename, "rb");
    if (!f) {
	yasm_error_set(YASM_ERROR_IO,
		       N_("`incbin': unable to open file `%s'"),
		       incbin->filename);
	return YASM_BC_RESOLVE_ERROR | YASM_BC_RESOLVE_UNKNOWN_LEN;
    }
    if (fseek(f, 0L, SEEK_END) < 0) {
	yasm_error_set(YASM_ERROR_IO,
		       N_("`incbin': unable to seek on file `%s'"),
		       incbin->filename);
	return YASM_BC_RESOLVE_ERROR | YASM_BC_RESOLVE_UNKNOWN_LEN;
    }
    flen = (unsigned long)ftell(f);
    fclose(f);

    /* Compute length of incbin from start, maxlen, and len */
    if (start > flen) {
	yasm_warn_set(YASM_WARN_GENERAL,
		      N_("`incbin': start past end of file `%s'"),
		      incbin->filename);
	start = flen;
    }
    flen -= start;
    if (incbin->maxlen)
	if (maxlen < flen)
	    flen = maxlen;
    bc->len += flen;
    return YASM_BC_RESOLVE_MIN_LEN;
}

static int
bc_incbin_tobytes(yasm_bytecode *bc, unsigned char **bufp, void *d,
		  yasm_output_value_func output_value,
		  /*@unused@*/ yasm_output_reloc_func output_reloc)
{
    bytecode_incbin *incbin = (bytecode_incbin *)bc->contents;
    FILE *f;
    /*@dependent@*/ /*@null@*/ const yasm_intnum *num;
    unsigned long start = 0;

    /* Convert start to integer value */
    if (incbin->start) {
	num = yasm_expr_get_intnum(&incbin->start, NULL);
	if (!num)
	    yasm_internal_error(
		N_("could not determine start in bc_tobytes_incbin"));
	start = yasm_intnum_get_uint(num);
    }

    /* Open file */
    f = fopen(incbin->filename, "rb");
    if (!f) {
	yasm_error_set(YASM_ERROR_IO, N_("`incbin': unable to open file `%s'"),
		       incbin->filename);
	return 1;
    }

    /* Seek to start of data */
    if (fseek(f, (long)start, SEEK_SET) < 0) {
	yasm_error_set(YASM_ERROR_IO,
		       N_("`incbin': unable to seek on file `%s'"),
		       incbin->filename);
	fclose(f);
	return 1;
    }

    /* Read len bytes */
    if (fread(*bufp, 1, (size_t)bc->len, f) < (size_t)bc->len) {
	yasm_error_set(YASM_ERROR_IO,
		       N_("`incbin': unable to read %lu bytes from file `%s'"),
		       bc->len, incbin->filename);
	fclose(f);
	return 1;
    }

    *bufp += bc->len;
    fclose(f);
    return 0;
}

yasm_bytecode *
yasm_bc_create_incbin(char *filename, yasm_expr *start, yasm_expr *maxlen,
		      unsigned long line)
{
    bytecode_incbin *incbin = yasm_xmalloc(sizeof(bytecode_incbin));

    /*@-mustfree@*/
    incbin->filename = filename;
    incbin->start = start;
    incbin->maxlen = maxlen;
    /*@=mustfree@*/

    return yasm_bc_create_common(&bc_incbin_callback, incbin, line);
}