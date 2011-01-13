/*-
 * Copyright (c) 2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
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
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include "config.h"

#include "svnid.h"
SVNID("$Id")

#include <stdio.h>
#include <stdlib.h>

#include "cache.h"
#include "cache_esi.h"
#include "vend.h"
#include "vct.h"
#include "stevedore.h"

#ifndef OLD_ESI

struct vep_match {
	const char	*match;
	const char	**state;
};

struct vep_state {
	unsigned		magic;
#define VEP_MAGIC		0x55cb9b82
	vfp_bytes_f		*bytes;
	struct vsb		*vsb;

	/* parser state */
	const char		*state;

	unsigned		endtag;
	unsigned		emptytag;
	unsigned		canattr;

	unsigned		remove;
	unsigned		skip;

	unsigned		o_verbatim;
	unsigned		o_skip;

	const char		*ver_p;

	const char		*until;
	const char		*until_p;
	const char		*until_s;

	const char		*esicmt;
	const char		*esicmt_p;

	struct vep_match	*attr;
	int			attr_l;
	struct vsb		*attr_vsb;
	int			attr_delim;

	struct vep_match	*match;
	int			match_l;
	struct vep_match	*match_hit;

	char			tag[10];
	int			tag_i;
};

/*---------------------------------------------------------------------*/

static const char *VEP_START =		"[Start]";
static const char *VEP_NOTXML =	 	"[NotXml]";

static const char *VEP_NEXTTAG = 	"[NxtTag]";
static const char *VEP_NOTMYTAG =	"[NotMyTag]";

static const char *VEP_STARTTAG = 	"[StartTag]";
static const char *VEP_COMMENT =	"[Comment]";
static const char *VEP_CDATA =		"[CDATA]";
static const char *VEP_ESITAG =		"[ESITag]";
static const char *VEP_ESIETAG =	"[ESIEndTag]";

static const char *VEP_ESIREMOVE =	"[ESI:Remove]";
static const char *VEP_ESIINCLUDE =	"[ESI:Include]";
static const char *VEP_ESICOMMENT =	"[ESI:Comment]";

static const char *VEP_INTAG =		"[InTag]";

static const char *VEP_ATTR =		"[Attribute]";
static const char *VEP_SKIPATTR =	"[SkipAttribute]";
static const char *VEP_SKIPATTR2 =	"[SkipAttribute2]";
static const char *VEP_ATTRGETVAL =	"[AttrGetValue]";
static const char *VEP_ATTRVAL =	"[AttrValue]";

static const char *VEP_UNTIL =		"[Until]";
static const char *VEP_MATCHBUF = 	"[MatchBuf]";
static const char *VEP_MATCH =		"[Match]";
static const char *VEP_XXX =		"[XXX]";

/*---------------------------------------------------------------------*/

static struct vep_match vep_match_starttag[] = {
	{ "<!--",	&VEP_COMMENT },
	{ "</esi:",	&VEP_ESIETAG },
	{ "<esi:",	&VEP_ESITAG },
	{ "<![CDATA[",	&VEP_CDATA },
	{ NULL,		&VEP_NOTMYTAG }
};

static const int vep_match_starttag_len =
    sizeof vep_match_starttag / sizeof vep_match_starttag[0];

/*---------------------------------------------------------------------*/

static struct vep_match vep_match_esi[] = {
	{ "include",	&VEP_ESIINCLUDE },
	{ "remove",	&VEP_ESIREMOVE },
	{ "comment",	&VEP_ESICOMMENT },
	{ NULL,		&VEP_XXX }
};

static const int vep_match_esi_len =
    sizeof vep_match_esi / sizeof vep_match_esi[0];

/*---------------------------------------------------------------------*/

static struct vep_match vep_match_esie[] = {
	{ "remove",	&VEP_ESIREMOVE },
	{ NULL,		&VEP_XXX }
};

static const int vep_match_esie_len =
    sizeof vep_match_esie / sizeof vep_match_esie[0];

/*---------------------------------------------------------------------*/

static struct vep_match vep_match_attr_include[] = {
	{ "src=",	&VEP_ATTRGETVAL },
	{ NULL,		&VEP_SKIPATTR }
};

static const int vep_match_attr_include_len =
    sizeof vep_match_attr_include / sizeof vep_match_attr_include[0];

/*---------------------------------------------------------------------
 * return match or NULL if more input needed.
 */
static struct vep_match *
vep_match(struct vep_state *vep, const char *b, const char *e)
{
	struct vep_match *vm;
	const char *q, *r;

	for (vm = vep->match; vm->match; vm++) {
		r = b;
		for (q = vm->match; *q && r < e; q++, r++)
			if (*q != *r)
				break;
		if (*q != '\0' && r == e) {
			if (b != vep->tag) {
				assert(e - b < sizeof vep->tag);
				memcpy(vep->tag, b, e - b);
				vep->tag_i = e - b;
			}
			return (NULL);
		}
		if (*q == '\0')
			return (vm);
	}
	return (vm);
}

/*---------------------------------------------------------------------
 *
 */

static void
vep_emit_len(struct vep_state *vep, ssize_t l, int m8, int m16, int m32)
{
	uint8_t buf[5];

	assert(l > 0);
	if (l < 256) {
		buf[0] = m8;
		buf[1] = (uint8_t)l;
		vsb_bcat(vep->vsb, buf, 2);
	} else if (l < 65536) {
		buf[0] = m16;
		vbe16enc(buf + 1, (uint16_t)l);
		vsb_bcat(vep->vsb, buf, 3);
	} else {
		/* XXX assert < 2^32 */
		buf[0] = m32;
		vbe32enc(buf + 1, (uint32_t)l);
		vsb_bcat(vep->vsb, buf, 5);
	}
} 

static void
vep_emit_skip(struct vep_state *vep)
{
	ssize_t l;

	l = vep->o_skip;
	vep->o_skip = 0;
	assert(l > 0);
	vep_emit_len(vep, l, VEC_S1, VEC_S2, VEC_S4);
} 

static void
vep_emit_verbatim(struct vep_state *vep)
{
	ssize_t l;

	l = vep->o_verbatim;
	vep->o_verbatim = 0;
	assert(l > 0);
	vep_emit_len(vep, l, VEC_V1, VEC_V2, VEC_V4);
	vsb_printf(vep->vsb, "%lx\r\n%c", l, 0);
} 

static void
vep_emit_literal(struct vep_state *vep, const char *p, const char *e)
{
	ssize_t l;

	if (e == NULL)
		e = strchr(p, '\0');
	if (vep->o_verbatim > 0) 
		vep_emit_verbatim(vep);
	if (vep->o_skip > 0) 
		vep_emit_skip(vep);
	l = e - p;
	printf("---->L(%d) [%.*s]\n", (int)l, (int)l, p);
	vep_emit_len(vep, l, VEC_L1, VEC_L2, VEC_L4);
	vsb_printf(vep->vsb, "%lx\r\n%c", l, 0);
	vsb_bcat(vep->vsb, p, l);
}


static void
vep_mark_verbatim(struct vep_state *vep, const char *p)
{
	ssize_t l;

	AN(vep->ver_p);
	l = p - vep->ver_p;
	if (l == 0)
		return;
	if (vep->o_skip > 0) 
		vep_emit_skip(vep);
	AZ(vep->o_skip);
	printf("-->V(%d) [%.*s]\n", (int)l, (int)l, vep->ver_p);
	vep->o_verbatim += l;
	vep->ver_p = p;
} 

static void
vep_mark_skip(struct vep_state *vep, const char *p)
{
	ssize_t l;

	AN(vep->ver_p);
	l = p - vep->ver_p;
	if (l == 0)
		return;
	if (vep->o_verbatim > 0) 
		vep_emit_verbatim(vep);
	AZ(vep->o_verbatim);
	printf("-->S(%d) [%.*s]\n", (int)l, (int)l, vep->ver_p);
	vep->o_skip += l;
	vep->ver_p = p;
} 

/*---------------------------------------------------------------------
 * Lex/Parse object for ESI instructions
 *
 * This function is called with the input object piecemal so do not
 * assume that we have more than one char available at at time, but
 * optimize for getting huge chunks. 
 */

static void
vep_parse(struct vep_state *vep, const char *b, size_t l)
{
	const char *e, *p;
	struct vep_match *vm;
	int i;

	CHECK_OBJ_NOTNULL(vep, VEP_MAGIC);
	assert(l > 0);

	e = b + l;

	if (0)
		vep_emit_literal(vep, "A", "B");

	p = b;
	while (p < e) {
		AN(vep->state);
		printf("EP %s %d %d (%.*s) [%.*s]\n",
		    vep->state,
		    vep->skip,
		    vep->remove,
		    vep->tag_i, vep->tag,
		    (int)(e - p), p);
		fflush(stdout);
		usleep(10);

		/******************************************************
		 * SECTION A
		 */

		if (vep->state == VEP_START) {
			/*
			 * If the first non-whitespace char is different
			 * from '<' we assume this is not XML.
			 */
			while (p < e && vct_islws(*p)) {
				p++;
				vep_mark_verbatim(vep, p);
			}
			if (p < e) {
				if (*p == '<') {
					vep->state = VEP_STARTTAG;
				} else
					vep->state = VEP_NOTXML;
			}
		} else if (vep->state == VEP_NOTXML) {
			/*
			 * This is not recognized as XML, just skip thru
			 * vfp_esi_end() will handle the rest
			 */
			p = e;

		/******************************************************
		 * SECTION D
		 */

		} else if (vep->state == VEP_NOTMYTAG) {
			vep->tag_i = 0;
			while (p < e) {
				if (!vep->remove)
					vep_mark_verbatim(vep, p + 1);
				if (*p++ == '>') {
					vep->state = VEP_NEXTTAG;
					break;
				}
			}
		} else if (vep->state == VEP_NEXTTAG) {
			/*
			 * Hunt for start of next tag and keep an eye
			 * out for end of EsiCmt if armed.
			 */
			while (p < e && *p != '<') {
				if (vep->esicmt_p != NULL &&
				    *p == *vep->esicmt_p++) {
					p++;
					if (*vep->esicmt_p == '\0') {
						vep->esicmt = NULL;
						vep->esicmt_p = NULL;
						/*
						 * The end of the esicmt
						 * should not be emitted.
						 * But the stuff before should
						 */
						if (!vep->remove)
							vep_mark_verbatim(vep,
							    p - 3);
						vep_mark_skip(vep, p);
					}
				} else {
					p++;
					vep->esicmt_p = vep->esicmt;
					if (vep->esicmt_p == NULL &&
					    !vep->remove)
						vep_mark_verbatim(vep, p);
				}
			}
			if (p < e)
				vep->state = VEP_STARTTAG;

		/******************************************************
		 * SECTION B
		 */

		} else if (vep->state == VEP_STARTTAG) {
			/*
			 * Start of tag, set up match table
			 */
			assert(*p == '<');
			if (!vep->remove)
				vep_mark_verbatim(vep, p);
			vep->match = vep_match_starttag;
			vep->match_l = vep_match_starttag_len;
			vep->state = VEP_MATCH;
		} else if (vep->state == VEP_COMMENT) {
			/*
			 * We are in a comment, find out if it is an
			 * ESI comment or a regular comment
			 */
			if (vep->esicmt == NULL)
				vep->esicmt_p = vep->esicmt = "esi";
			while (p < e) {
				if (*p == *vep->esicmt_p) {
					p++;
					if (*++vep->esicmt_p == '\0') {
						vep->esicmt_p =
						    vep->esicmt = "-->";
						vep->state = VEP_NEXTTAG;
						vep_mark_skip(vep, p);
						break;
					}
				} else {
					vep->esicmt_p = vep->esicmt = NULL; 
					vep->until_p = vep->until = "-->";
					vep->until_s = VEP_NEXTTAG;
					vep->state = VEP_UNTIL;
					break;
				}
			}
		} else if (vep->state == VEP_CDATA) {
			/*
			 * Easy: just look for the end of CDATA
			 */
			vep->until_p = vep->until = "]]>";
			vep->until_s = VEP_NEXTTAG;
			vep->state = VEP_UNTIL;
		} else if (vep->state == VEP_ESITAG) {
			vep->tag_i = 0;
			vep->endtag = 0;
			if (vep->remove) {
				VSC_main->esi_errors++;
				vep->state = VEP_NOTMYTAG;
				break;
			}
			vep->skip = 1;
			vep_mark_skip(vep, p);
			vep->match = vep_match_esi;
			vep->match_l = vep_match_esi_len;
			vep->state = VEP_MATCH;
		} else if (vep->state == VEP_ESIETAG) {
			vep->tag_i = 0;
			vep->endtag = 1;
			vep->match = vep_match_esie;
			vep->match_l = vep_match_esie_len;
			vep->state = VEP_MATCH;
		} else if (vep->state == VEP_ESIINCLUDE) {
			vep->state = VEP_INTAG;
			vep->attr = vep_match_attr_include;
			vep->attr_l = vep_match_attr_include_len;
		} else if (vep->state == VEP_ESIREMOVE) {
			vep->remove = !vep->endtag;

		/******************************************************
		 * SECTION F
		 */

		} else if (vep->state == VEP_INTAG) {
			vep->tag_i = 0;
			while (p < e && vct_islws(*p)) {
				p++;	
				vep->canattr = 1;
			}
			if (p < e && *p == '/' && !vep->emptytag) {
				p++;
				vep->emptytag = 1;
				vep->canattr = 0;
			}
			if (p < e && *p == '>') {
				p++;
				/* XXX: processing */
				vep->state = VEP_NEXTTAG;
			} else if (p < e && vep->emptytag) {
				INCOMPL();	/* ESI-SYNTAX ERROR */
			} else if (p < e && vct_isxmlnamestart(*p)) {
				vep->state = VEP_ATTR;
			} else if (p < e) {
				INCOMPL();	/* ESI-SYNTAX ERROR */
			}

		/******************************************************
		 * SECTION G
		 */

		} else if (vep->state == VEP_ATTR) {
			AZ(vep->attr_delim);
			if (vep->attr == NULL) {
				p++;
				AZ(vep->attr_vsb);
				vep->state = VEP_SKIPATTR;
				break;
			}
			vep->match = vep->attr;
			vep->match_l = vep->attr_l;
			vep->state = VEP_MATCH;
		} else if (vep->state == VEP_SKIPATTR) {
			vep->state = VEP_SKIPATTR2;
			for (i = 0; i < vep->tag_i; i++) {
				if (vct_isxmlname(vep->tag[i]))
					continue;
				if (vep->tag[i] == '=') {
					assert(i + 1 == vep->tag_i);
					vep->state = VEP_ATTRVAL;
				}
			}
			xxxassert(i == vep->tag_i);
			vep->tag_i = 0;
		} else if (vep->state == VEP_SKIPATTR2) {
			while (p < e && vct_isxmlname(*p))
				p++;
			if (p < e && *p == '=') {
				p++;
				vep->state = VEP_ATTRVAL;
				break;
			}
			if (p < e) {
				INCOMPL();	/* ESI-SYNTAX ERROR */
			}
		} else if (vep->state == VEP_ATTRGETVAL) {
			vep->attr_vsb = vsb_newauto();
			vep->state = VEP_ATTRVAL;
		} else if (vep->state == VEP_ATTRVAL) {
			if (vep->attr_delim == 0)  {
				if (*p != '"' && *p != '\'')
					INCOMPL();	/* ESI-SYNTAX */
				vep->attr_delim = *p++;
			}
			while (p < e && *p != vep->attr_delim) {
				if (vep->attr_vsb != NULL)
					vsb_bcat(vep->attr_vsb, p, 1);
				p++;
			}
			if (p < e) {
				if (vep->attr_vsb != NULL) {
					vsb_finish(vep->attr_vsb);
					printf("ATTR (%s) (%s)\n",
						vep->match_hit->match,
						vsb_data(vep->attr_vsb));
					vsb_delete(vep->attr_vsb);
					vep->attr_vsb = NULL;
				}
				p++;
				vep->attr_delim = 0;
				vep->state = VEP_INTAG;
			}
	

		/******************************************************
		 * Utility Section
		 */

		} else if (vep->state == VEP_MATCH) {
			/*
			 * Match against a table
			 */
			vm = vep_match(vep, p, e);
			vep->match_hit = vm;
			if (vm != NULL) {
				if (vm->match != NULL)
					p += strlen(vm->match);
				b = p;
				vep->state = *vm->state;
				vep->match = NULL;
				vep->tag_i = 0;
			} else {
				memcpy(vep->tag, p, e - p);
				vep->tag_i = e - p;
				vep->state = VEP_MATCHBUF;
				p = e;
				break;
			}
		} else if (vep->state == VEP_MATCHBUF) {
			/*
			 * Match against a table while split over input
			 * sections.
			 */
			do {
				if (*p == '>') {
					vm = NULL;
				} else {
					vep->tag[vep->tag_i++] = *p++;
					vm = vep_match(vep,
					    vep->tag, vep->tag + vep->tag_i);
				}
			} while (vm == 0 && p < e);
			vep->match_hit = vm;
			if (vm == 0) {
				b = e;
				break;
			}
			b = p;
			vep->state = *vm->state;
			vep->match = NULL;
		} else if (vep->state == VEP_UNTIL) {
			/*
			 * Skip until we see magic string
			 */
			while (p < e) {
				if (*p++ != *vep->until_p++) {
					vep->until_p = vep->until;
				} else if (*vep->until_p == '\0') {
					vep->state = vep->until_s;
					break;
				}
			}
		} else {
			printf("*** Unknown state %s\n", vep->state);
			INCOMPL();
		}
	}
}

/*---------------------------------------------------------------------
 * We receive a ungzip'ed object, and want to store it ungzip'ed.
 */

static int __match_proto__()
vfp_esi_bytes_uu(struct sess *sp, struct http_conn *htc, size_t bytes)
{
	struct vep_state *vep;
	ssize_t l, w;
	struct storage *st;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	vep = sp->wrk->vep;
	CHECK_OBJ_NOTNULL(vep, VEP_MAGIC);

	while (bytes > 0) {
		if (sp->wrk->storage == NULL) {
			l = params->fetch_chunksize * 1024LL;
			sp->wrk->storage = STV_alloc(sp, l);
		}
		if (sp->wrk->storage == NULL) {
			errno = ENOMEM;
			return (-1);
		}
		st = sp->wrk->storage;
		l = st->space - st->len;
		if (l > bytes)
			l = bytes;
		w = HTC_Read(htc, st->ptr + st->len, l);
		if (w <= 0)
			return (w);
		vep->ver_p = (const char *)st->ptr + st->len;
#if 1
		{
		for (l = 0; l < w; l++) 
			vep_parse(vep, (const char *)st->ptr + st->len + l, 1);
		}
#else
		vep_parse(vep, (const char *)st->ptr + st->len, w);
#endif
		st->len += w;
		sp->obj->len += w;
		if (st->len == st->space) {
			VTAILQ_INSERT_TAIL(&sp->obj->store,
			    sp->wrk->storage, list);
			sp->wrk->storage = NULL;
			st = NULL;
		}
		bytes -= w;
	}
	return (1);
}

/*---------------------------------------------------------------------*/

static void __match_proto__()
vfp_esi_begin(struct sess *sp, size_t estimate)
{
	struct vep_state *vep;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	AZ(sp->wrk->vep);
	vep = (void*)WS_Alloc(sp->wrk->ws, sizeof *vep);
	AN(vep);


	memset(vep, 0, sizeof *vep);
	vep->magic = VEP_MAGIC;
	vep->bytes = vfp_esi_bytes_uu;
	vep->vsb = vsb_newauto();
	vep->state = VEP_START;
	AN(vep->vsb);

	sp->wrk->vep = vep;
	(void)estimate;
}

static int __match_proto__()
vfp_esi_bytes(struct sess *sp, struct http_conn *htc, size_t bytes)
{
	struct vep_state *vep;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	vep = sp->wrk->vep;
	CHECK_OBJ_NOTNULL(vep, VEP_MAGIC);
	AN(vep->bytes);
	return (vep->bytes(sp, htc, bytes));
}

static int __match_proto__()
vfp_esi_end(struct sess *sp)
{
	struct storage *st;
	struct vep_state *vep;
	size_t l;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	vep = sp->wrk->vep;
	CHECK_OBJ_NOTNULL(vep, VEP_MAGIC);

	if (vep->o_verbatim)
		vep_emit_verbatim(vep);
	if (vep->o_skip)
		vep_emit_skip(vep);
	vsb_finish(vep->vsb);
	l = vsb_len(vep->vsb);
	if (vep->state != VEP_NOTXML && l != 0) {
		printf("ESI %d <%s>\n", (int)l, vsb_data(vep->vsb));

		/* XXX: This is a huge waste of storage... */
		sp->obj->esidata = STV_alloc(sp, vsb_len(vep->vsb));
		AN(sp->obj->esidata);
		memcpy(sp->obj->esidata->ptr,
		    vsb_data(vep->vsb), vsb_len(vep->vsb));
		sp->obj->esidata->len = vsb_len(vep->vsb);
	}
	vsb_delete(vep->vsb);

	st = sp->wrk->storage;
	sp->wrk->storage = NULL;
	if (st == NULL)
		return (0);

	if (st->len == 0) {
		STV_free(st);
		return (0);
	}
	if (st->len < st->space)
		STV_trim(st, st->len);
	VTAILQ_INSERT_TAIL(&sp->obj->store, st, list);
	return (0);
}

struct vfp vfp_esi = {
        .begin  =       vfp_esi_begin,
        .bytes  =       vfp_esi_bytes,
        .end    =       vfp_esi_end,
};

#endif /* OLD_ESI */
