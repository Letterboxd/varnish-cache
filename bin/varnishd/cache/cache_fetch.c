/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2014 Varnish Software AS
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
 */

#include "config.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cache.h"

#include "hash/hash_slinger.h"

#include "vcl.h"
#include "vtim.h"


/*--------------------------------------------------------------------
 */

static void
vbf_release_req(struct busyobj *bo)
{
	assert(bo->state == BOS_INVALID);
	AN(bo->req);
	bo->req = NULL;
	http_CopyHome(bo->bereq);
	VBO_setstate(bo, BOS_REQ_DONE);
}

/*--------------------------------------------------------------------
 * Turn the beresp into a obj
 */

static int
vbf_beresp2obj(struct busyobj *bo)
{
	unsigned l;
	char *b;
	struct vsb *vary = NULL;
	int varyl = 0;
	uint16_t nhttp;
	struct object *obj;
	struct http *hp, *hp2;

	l = 0;

	/* Create Vary instructions */
	if (!(bo->fetch_objcore->flags & OC_F_PRIVATE)) {
		varyl = VRY_Create(bo, &vary);
		if (varyl > 0) {
			AN(vary);
			assert(varyl == VSB_len(vary));
			l += varyl;
		} else if (varyl < 0) {
			/*
			 * Vary parse error
			 * Complain about it, and make this a pass.
			 */
			VSLb(bo->vsl, SLT_Error,
			    "Illegal 'Vary' header from backend, "
			    "making this a pass.");
			bo->uncacheable = 1;
			AZ(vary);
		} else
			/* No vary */
			AZ(vary);
	}

	l += http_EstimateWS(bo->beresp,
	    bo->uncacheable ? HTTPH_R_PASS : HTTPH_A_INS, &nhttp);

	if (bo->uncacheable)
		bo->fetch_objcore->flags |= OC_F_PASS;

	if (bo->uncacheable ||
	    bo->exp.ttl+bo->exp.grace+bo->exp.keep < cache_param->shortlived)
		bo->storage_hint = TRANSIENT_STORAGE;

	AN(bo->fetch_objcore);
	obj = STV_NewObject(bo, bo->storage_hint, l, nhttp);
	if (obj == NULL &&
	    (bo->storage_hint == NULL ||
	    strcmp(bo->storage_hint, TRANSIENT_STORAGE))) {
		/*
		 * Try to salvage the transaction by allocating a
		 * shortlived object on Transient storage.
		 */
		if (bo->exp.ttl > cache_param->shortlived)
			bo->exp.ttl = cache_param->shortlived;
		bo->exp.grace = 0.0;
		bo->exp.keep = 0.0;
		obj = STV_NewObject(bo, TRANSIENT_STORAGE, l, nhttp);
	}
	if (obj == NULL)
		return (-1);

	CHECK_OBJ_NOTNULL(obj, OBJECT_MAGIC);

	bo->storage_hint = NULL;

	AZ(bo->fetch_obj);
	bo->fetch_obj = obj;

	if (bo->do_gzip || (bo->is_gzip && !bo->do_gunzip))
		obj->gziped = 1;

	if (vary != NULL) {
		obj->vary = (void *)WS_Copy(obj->http->ws,
		    VSB_data(vary), varyl);
		AN(obj->vary);
		(void)VRY_Validate(obj->vary);
		VSB_delete(vary);
	}

	obj->vxid = bo->vsl->wid;
	WS_Assert(bo->ws_o);

	/* Filter into object */
	hp = bo->beresp;
	hp2 = obj->http;

	hp2->logtag = SLT_ObjMethod;
	http_FilterResp(hp, hp2, bo->uncacheable ? HTTPH_R_PASS : HTTPH_A_INS);
	http_CopyHome(hp2);

	if (http_GetHdr(hp, H_Last_Modified, &b))
		obj->last_modified = VTIM_parse(b);
	else
		obj->last_modified = floor(bo->exp.t_origin);

	return (0);
}

/*--------------------------------------------------------------------
 * Copy req->bereq
 */

static enum fetch_step
vbf_stp_mkbereq(const struct worker *wrk, struct busyobj *bo)
{
	char *p;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	CHECK_OBJ_NOTNULL(bo->req, REQ_MAGIC);

	AN(bo->director);
	AZ(bo->vbc);
	AZ(bo->should_close);
	AZ(bo->storage_hint);

	HTTP_Setup(bo->bereq0, bo->ws, bo->vsl, SLT_BereqMethod);
	http_FilterReq(bo->bereq0, bo->req->http,
	    bo->do_pass ? HTTPH_R_PASS : HTTPH_R_FETCH);
	if (!bo->do_pass) {
		// XXX: Forcing GET should happen in vcl_miss{} ?
		http_ForceGet(bo->bereq0);
		if (cache_param->http_gzip_support) {
			/*
			 * We always ask the backend for gzip, even if the
			 * client doesn't grok it.  We will uncompress for
			 * the minority of clients which don't.
			 */
			http_ForceHeader(bo->bereq0, H_Accept_Encoding, "gzip");
		}
	}
	if (bo->ims_obj != NULL) {
		if (http_GetHdr(bo->ims_obj->http, H_Last_Modified, &p)) {
			http_PrintfHeader(bo->bereq0,
			    "If-Modified-Since: %s", p);
		} else if (http_GetHdr(bo->ims_obj->http, H_ETag, &p)) {
			http_PrintfHeader(bo->bereq0,
			    "If-None-Match: %s", p);
		} else {
			WRONG("Shouldn't have bo->ims_obj");
		}
	}

	return (F_STP_STARTFETCH);
}

/*--------------------------------------------------------------------
 * Start a new VSL transaction and try again
 */

static enum fetch_step
vbf_stp_retry(struct worker *wrk, struct busyobj *bo)
{
	unsigned owid, wid;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);

	// XXX: BereqEnd + BereqAcct ?
	wid = VXID_Get(&wrk->vxid_pool);
	VSLb(bo->vsl, SLT_Link, "bereq %u retry", wid);
	VSLb(bo->vsl, SLT_End, "%s", "");
	VSL_Flush(bo->vsl, 0);
	owid = bo->vsl->wid & VSL_IDENTMASK;
	bo->vsl->wid = wid | VSL_BACKENDMARKER;
	VSLb(bo->vsl, SLT_Begin, "bereq %u retry", owid);

	return (F_STP_STARTFETCH);
}

/*--------------------------------------------------------------------
 * Setup bereq from bereq0, run vcl_backend_fetch
 */

static enum fetch_step
vbf_stp_startfetch(struct worker *wrk, struct busyobj *bo)
{

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);

	AN(bo->director);
	AZ(bo->vbc);
	AZ(bo->should_close);
	AZ(bo->storage_hint);

	HTTP_Setup(bo->bereq, bo->ws, bo->vsl, SLT_BereqMethod);
	HTTP_Copy(bo->bereq, bo->bereq0);

	VCL_backend_fetch_method(bo->vcl, wrk, NULL, bo, bo->bereq->ws);

	bo->uncacheable = bo->do_pass;

	http_PrintfHeader(bo->bereq,
	    "X-Varnish: %u", bo->vsl->wid & VSL_IDENTMASK);
	if (wrk->handling == VCL_RET_ABANDON) {
		if (bo->req != NULL)
			vbf_release_req(bo);
		HSH_Fail(bo->fetch_objcore);
		VBO_setstate(bo, BOS_FAILED);
		return (F_STP_DONE);
	}
	assert (wrk->handling == VCL_RET_FETCH);
	return (F_STP_FETCHHDR);
}


/*--------------------------------------------------------------------
 */

static enum fetch_step
vbf_stp_fetchhdr(struct worker *wrk, struct busyobj *bo)
{
	int i, do_ims;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);

	xxxassert (wrk->handling == VCL_RET_FETCH);

	HTTP_Setup(bo->beresp, bo->ws, bo->vsl, SLT_BerespMethod);

	if (!bo->do_pass && bo->req != NULL)
		vbf_release_req(bo); /* XXX: retry ?? */

	assert(bo->state <= BOS_REQ_DONE);

	i = V1F_fetch_hdr(wrk, bo, bo->req);
	/*
	 * If we recycle a backend connection, there is a finite chance
	 * that the backend closed it before we get a request to it.
	 * Do a single retry in that case.
	 */
	if (i == 1) {
		VSC_C_main->backend_retry++;
		i = V1F_fetch_hdr(wrk, bo, bo->req);
	}

	if (i) {
		AZ(bo->vbc);
		return (F_STP_ERROR);
	}

	if (bo->do_pass && bo->req != NULL)
		vbf_release_req(bo); /* XXX : retry ?? */

	AZ(bo->req);

	AN(bo->vbc);
	http_VSL_log(bo->beresp);

	/*
	 * These two headers can be spread over multiple actual headers
	 * and we rely on their content outside of VCL, so collect them
	 * into one line here.
	 */
	http_CollectHdr(bo->beresp, H_Cache_Control);
	http_CollectHdr(bo->beresp, H_Vary);

	/*
	 * Figure out how the fetch is supposed to happen, before the
	 * headers are adultered by VCL
	 * NB: Also sets other wrk variables
	 */
	bo->htc.body_status = RFC2616_Body(bo, &wrk->stats);

	if (bo->htc.body_status == BS_ERROR) {
		AN (bo->vbc);
		VDI_CloseFd(&bo->vbc);
		VSLb(bo->vsl, SLT_VCL_Error, "Body cannot be fetched");
		return (F_STP_ERROR);
	}

	/*
	 * What does RFC2616 think about TTL ?
	 */
	EXP_Clr(&bo->exp);
	RFC2616_Ttl(bo);

	/* private objects have negative TTL */
	if (bo->fetch_objcore->flags & OC_F_PRIVATE)
		bo->exp.ttl = -1.;

	AZ(bo->do_esi);

	if (bo->ims_obj != NULL && bo->beresp->status == 304) {
		bo->beresp->status = 200;
		http_PrintfHeader(bo->beresp, "Content-Length: %jd",
		    (intmax_t)bo->ims_obj->len);
		do_ims = 1;
	} else
		do_ims = 0;

	VCL_backend_response_method(bo->vcl, wrk, NULL, bo, bo->beresp->ws);

	if (wrk->handling == VCL_RET_RETRY) {
		AN (bo->vbc);
		VDI_CloseFd(&bo->vbc);
		bo->retries++;
		if (bo->retries <= cache_param->max_retries)
			return (F_STP_RETRY);
		VSLb(bo->vsl, SLT_VCL_Error,
		    "Too many retries, delivering 503");
		return (F_STP_ERROR);
	}

	assert(bo->state == BOS_REQ_DONE);
	VBO_setstate(bo, BOS_COMMITTED);

	if (bo->do_esi)
		bo->do_stream = 0;
	if (bo->do_pass || bo->uncacheable)
		bo->fetch_objcore->flags |= OC_F_PASS;

	assert(wrk->handling == VCL_RET_DELIVER);
	return (do_ims ? F_STP_CONDFETCH : F_STP_FETCH);
}

/*--------------------------------------------------------------------
 */

static enum fetch_step
vbf_stp_fetch(struct worker *wrk, struct busyobj *bo)
{
	struct object *obj;
	ssize_t est;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);

	assert(wrk->handling == VCL_RET_DELIVER);

	/*
	 * The VCL variables beresp.do_g[un]zip tells us how we want the
	 * object processed before it is stored.
	 *
	 * The backend Content-Encoding header tells us what we are going
	 * to receive, which we classify in the following three classes:
	 *
	 *	"Content-Encoding: gzip"	--> object is gzip'ed.
	 *	no Content-Encoding		--> object is not gzip'ed.
	 *	anything else			--> do nothing wrt gzip
	 *
	 * XXX: BS_NONE/cl==0 should avoid gzip/gunzip
	 */

	/* We do nothing unless the param is set */
	if (!cache_param->http_gzip_support)
		bo->do_gzip = bo->do_gunzip = 0;

	bo->is_gzip = http_HdrIs(bo->beresp, H_Content_Encoding, "gzip");

	bo->is_gunzip = !http_GetHdr(bo->beresp, H_Content_Encoding, NULL);

	/* It can't be both */
	assert(bo->is_gzip == 0 || bo->is_gunzip == 0);

	/* We won't gunzip unless it is gzip'ed */
	if (bo->do_gunzip && !bo->is_gzip)
		bo->do_gunzip = 0;

	/* If we do gunzip, remove the C-E header */
	if (bo->do_gunzip)
		http_Unset(bo->beresp, H_Content_Encoding);

	/* We wont gzip unless it is ungziped */
	if (bo->do_gzip && !bo->is_gunzip)
		bo->do_gzip = 0;

	/* If we do gzip, add the C-E header */
	if (bo->do_gzip)
		http_SetHeader(bo->beresp, "Content-Encoding: gzip");

	/* But we can't do both at the same time */
	assert(bo->do_gzip == 0 || bo->do_gunzip == 0);

	AN(bo->vbc);
	est = V1F_Setup_Fetch(bo);

	if (bo->do_gunzip || (bo->is_gzip && bo->do_esi)) {
		RFC2616_Weaken_Etag(bo->beresp);
		VFP_Push(bo, vfp_gunzip_pull, 0);
	}

	if (bo->do_esi && bo->do_gzip) {
		VFP_Push(bo, vfp_esi_gzip_pull, 0);
		RFC2616_Weaken_Etag(bo->beresp);
	} else if (bo->do_esi && bo->is_gzip && !bo->do_gunzip) {
		VFP_Push(bo, vfp_esi_gzip_pull, 0);
		RFC2616_Weaken_Etag(bo->beresp);
	} else if (bo->do_esi) {
		VFP_Push(bo, vfp_esi_pull, 0);
	} else if (bo->do_gzip) {
		VFP_Push(bo, vfp_gzip_pull, 0);
		RFC2616_Weaken_Etag(bo->beresp);
	} else if (bo->is_gzip && !bo->do_gunzip) {
		VFP_Push(bo, vfp_testgunzip_pull, 0);
	}

	if (bo->fetch_objcore->flags & OC_F_PRIVATE)
		AN(bo->uncacheable);

	/* No reason to try streaming a non-existing body */
	if (bo->htc.body_status == BS_NONE)
		bo->do_stream = 0;

	if (vbf_beresp2obj(bo)) {
		(void)VFP_Error(bo, "Could not get storage");
		VDI_CloseFd(&bo->vbc);
		return (F_STP_DONE);
	}

	assert(WRW_IsReleased(wrk));

	obj = bo->fetch_obj;

	/*
	 * Ready to fetch the body
	 */

	assert(bo->refcount >= 1);

	AZ(WS_Overflowed(bo->ws_o));
	if (bo->do_stream)
		HSH_Unbusy(&wrk->stats, obj->objcore);

	if (bo->state == BOS_COMMITTED)
		VBO_setstate(bo, BOS_FETCHING);
	else if (bo->state != BOS_FAILED)
		WRONG("Wrong bo->state");

	if (bo->htc.body_status != BS_NONE) {
		assert(bo->htc.body_status  != BS_ERROR);
		VFP_Fetch_Body(bo, est);
	}

	bo->t_body = VTIM_mono();

	VSLb(bo->vsl, SLT_Fetch_Body, "%u(%s)",
	    bo->htc.body_status, body_status_2str(bo->htc.body_status));

	if (bo->state == BOS_FAILED) {
		wrk->stats.fetch_failed++;
	} else {
		assert(bo->state == BOS_FETCHING);

		VSLb(bo->vsl, SLT_Length, "%zd", obj->len);

		{
		/* Sanity check fetch methods accounting */
			ssize_t uu;
			struct storage *st;

			uu = 0;
			VTAILQ_FOREACH(st, &obj->store, list)
				uu += st->len;
			if (bo->do_stream)
				/* Streaming might have started freeing stuff */
				assert(uu <= obj->len);

			else
				assert(uu == obj->len);
		}
	}

	if (!bo->do_stream && bo->state != BOS_FAILED)
		HSH_Unbusy(&wrk->stats, obj->objcore);

	HSH_Complete(obj->objcore);

	assert(bo->refcount >= 1);

	if (bo->state != BOS_FAILED)
		VBO_setstate(bo, BOS_FINISHED);

	return (F_STP_DONE);
}

/*--------------------------------------------------------------------
 */

static enum fetch_step
vbf_stp_condfetch(struct worker *wrk, struct busyobj *bo)
{
	unsigned l;
	uint16_t nhttp;
	struct object *obj;
	struct objiter *oi;
	void *sp;
	ssize_t sl, al, tl, vl;
	struct storage *st;
	enum objiter_status ois;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);

	l = 0;
	if (bo->ims_obj->vary != NULL) {
		vl = VRY_Validate(bo->ims_obj->vary);
		l += vl;
	} else
		vl = 0;
	l += http_EstimateWS(bo->ims_obj->http, 0, &nhttp);

	obj = STV_NewObject(bo, bo->storage_hint, l, nhttp);
	if (obj == NULL) {
		(void)VFP_Error(bo, "Could not get storage");
		VDI_CloseFd(&bo->vbc);
		return (F_STP_DONE);
	}

	AZ(bo->fetch_obj);
	bo->fetch_obj = obj;

	obj->gziped = bo->ims_obj->gziped;
	obj->gzip_start = bo->ims_obj->gzip_start;
	obj->gzip_last = bo->ims_obj->gzip_last;
	obj->gzip_stop = bo->ims_obj->gzip_stop;

	XXXAZ(bo->ims_obj->esidata);

	if (bo->ims_obj->vary != NULL) {
		obj->vary = (void *)WS_Copy(obj->http->ws,
		    bo->ims_obj->vary, vl);
		assert(vl == VRY_Validate(obj->vary));
	}

	obj->vxid = bo->vsl->wid;

	obj->http->logtag = SLT_ObjMethod;
	/* XXX: we should have our own HTTP_A_CONDFETCH */
	http_FilterResp(bo->ims_obj->http, obj->http, HTTPH_A_INS);
	http_CopyHome(obj->http);


	AZ(WS_Overflowed(bo->ws_o));
	VBO_setstate(bo, BOS_FETCHING);
	HSH_Unbusy(&wrk->stats, obj->objcore);


	st = NULL;
	al = 0;

	oi = ObjIterBegin(wrk, bo->ims_obj);
	do {
		ois = ObjIter(oi, &sp, &sl);
		while (sl > 0) {
			if (st == NULL)
				st = VFP_GetStorage(bo, bo->ims_obj->len - al);
			if (st == NULL)
				break;
			tl = sl;
			if (tl > st->space - st->len)
				tl = st->space - st->len;
			memcpy(st->ptr + st->len, sp, tl);
			al += tl;
			sp = (char *)sp + tl;
			sl -= tl;
			VBO_extend(bo, tl);
			if (st->len == st->space)
				st = NULL;
		}
	} while (bo->state < BOS_FAILED &&
	    (ois == OIS_DATA || ois == OIS_STREAM));
	ObjIterEnd(&oi);
	if (bo->state != BOS_FAILED) {
		assert(al == bo->ims_obj->len);
		assert(obj->len == al);
		VBO_setstate(bo, BOS_FINISHED);
		EXP_Rearm(bo->ims_obj, bo->ims_obj->exp.t_origin, 0, 0, 0);
	}
	HSH_Complete(obj->objcore);
	return (F_STP_DONE);
}

/*--------------------------------------------------------------------
 * Create synth object
 */

static enum fetch_step
vbf_stp_error(struct worker *wrk, struct busyobj *bo)
{
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);

	AN(bo->fetch_objcore->flags &= OC_F_BUSY);

	// XXX: reset all beresp flags ?

	HTTP_Setup(bo->beresp, bo->ws, bo->vsl, SLT_BerespMethod);
	http_SetResp(bo->beresp, "HTTP/1.1", 503, "Backend fetch failed");
	http_SetHeader(bo->beresp, "Content-Length: 0");
	http_SetHeader(bo->beresp, "Connection: close");

	bo->exp.ttl = 0;
	bo->exp.grace = 0;
	bo->exp.keep = 0;

	VCL_backend_error_method(bo->vcl, wrk, NULL, bo, bo->bereq->ws);

	xxxassert(wrk->handling == VCL_RET_DELIVER);

	if (bo->req != NULL)
		vbf_release_req(bo);

	http_PrintfHeader(bo->beresp, "Content-Length: %jd", (intmax_t)0);
	http_PrintfHeader(bo->beresp, "X-XXXPHK: yes");

	if (vbf_beresp2obj(bo)) {
		INCOMPL();
	}

	HSH_Unbusy(&wrk->stats, bo->fetch_obj->objcore);
	VBO_setstate(bo, BOS_FINISHED);
	HSH_Complete(bo->fetch_obj->objcore);
	return (F_STP_DONE);
}

/*--------------------------------------------------------------------
 */

static enum fetch_step
vbf_stp_done(void)
{
	WRONG("Just plain wrong");
	return (F_STP_DONE);
}

static void
vbf_fetch_thread(struct worker *wrk, void *priv)
{
	struct busyobj *bo;
	enum fetch_step stp;
	double t_hdr, t_body;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CAST_OBJ_NOTNULL(bo, priv, BUSYOBJ_MAGIC);
	CHECK_OBJ_NOTNULL(bo->req, REQ_MAGIC);

	THR_SetBusyobj(bo);
	stp = F_STP_MKBEREQ;
	bo->t_start = VTIM_real();
	bo->t_send = NAN;
	bo->t_sent = NAN;
	bo->t_hdr = NAN;
	bo->t_body = NAN;

	bo->stats = &wrk->stats;

	while (stp != F_STP_DONE) {
		CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
		bo->step = stp;
		switch(stp) {
#define FETCH_STEP(l, U, arg)						\
		case F_STP_##U:						\
			stp = vbf_stp_##l arg;				\
			break;
#include "tbl/steps.h"
#undef FETCH_STEP
		default:
			WRONG("Illegal fetch_step");
		}
	}
	assert(WRW_IsReleased(wrk));

	bo->stats = NULL;

	if (bo->vbc != NULL) {
		if (bo->should_close)
			VDI_CloseFd(&bo->vbc);
		else
			VDI_RecycleFd(&bo->vbc);
		AZ(bo->vbc);
	}

	http_Teardown(bo->bereq);
	http_Teardown(bo->beresp);

	if (bo->state == BOS_FAILED)
		assert(bo->fetch_objcore->flags & OC_F_FAILED);

	if (bo->ims_obj != NULL)
		(void)HSH_DerefObj(&wrk->stats, &bo->ims_obj);

	t_hdr = bo->t_hdr - bo->t_sent;
	t_body = bo->t_body - bo->t_hdr;
	VSLb(bo->vsl, SLT_BereqEnd, "%.9f %.9f %.9f %.9f %.9f %.9f",
	     bo->t_start,
	     VTIM_real(),
	     bo->t_sent - bo->t_send,
	     t_hdr, t_body, t_hdr + t_body);

	VBO_DerefBusyObj(wrk, &bo);
	THR_SetBusyobj(NULL);
}

/*--------------------------------------------------------------------
 */

void
VBF_Fetch(struct worker *wrk, struct req *req, struct objcore *oc,
    struct object *oldobj, enum vbf_fetch_mode_e mode)
{
	struct busyobj *bo;
	const char *how;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
	CHECK_OBJ_ORNULL(oldobj, OBJECT_MAGIC);

	bo = VBO_GetBusyObj(wrk, req);
	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);

	switch(mode) {
	case VBF_PASS:		how = "pass"; break;
	case VBF_NORMAL:	how = "fetch"; break;
	case VBF_BACKGROUND:	how = "bgfetch"; break;
	default:		WRONG("Wrong fetch mode");
	}

	VSLb(bo->vsl, SLT_Begin, "bereq %u %s ",
	    req->vsl->wid & VSL_IDENTMASK, how);
	VSLb(req->vsl, SLT_Link, "bereq %u %s ",
	    bo->vsl->wid & VSL_IDENTMASK, how);

	bo->refcount = 2;

	oc->busyobj = bo;

	CHECK_OBJ_NOTNULL(bo->vcl, VCL_CONF_MAGIC);

	if (mode == VBF_PASS)
		bo->do_pass = 1;

	bo->vary = req->vary_b;
	req->vary_b = NULL;

	if (mode != VBF_BACKGROUND)
		HSH_Ref(oc);
	bo->fetch_objcore = oc;

	AZ(bo->ims_obj);
	if (oldobj != NULL) {
		if (http_GetHdr(oldobj->http, H_Last_Modified, NULL) ||
		   http_GetHdr(oldobj->http, H_ETag, NULL)) {
			assert(oldobj->objcore->refcnt > 0);
			HSH_Ref(oldobj->objcore);
			bo->ims_obj = oldobj;
		}
	}

	AZ(bo->req);
	bo->req = req;

	bo->fetch_task.priv = bo;
	bo->fetch_task.func = vbf_fetch_thread;

	if (Pool_Task(wrk->pool, &bo->fetch_task, POOL_QUEUE_FRONT))
		vbf_fetch_thread(wrk, bo);
	if (mode == VBF_BACKGROUND) {
		VBO_waitstate(bo, BOS_REQ_DONE);
	} else {
		VBO_waitstate(bo, BOS_FETCHING);
		if (!bo->do_stream)
			VBO_waitstate(bo, BOS_FINISHED);
		assert(bo->state != BOS_FAILED || (oc->flags & OC_F_FAILED));
	}
	VBO_DerefBusyObj(wrk, &bo);
}
