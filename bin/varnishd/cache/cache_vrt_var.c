/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2011 Varnish Software AS
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
 * Runtime support for compiled VCL programs
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>

#include "cache.h"
#include "common/heritage.h"

#include "cache_backend.h"
#include "vrt_obj.h"
#include "vtcp.h"
#include "vtim.h"

static char vrt_hostname[255] = "";

/*--------------------------------------------------------------------*/

static void
vrt_do_string(struct worker *w, int fd, const struct http *hp, int fld,
    const char *err, const char *p, va_list ap)
{
	char *b;

	// AN(p);
	AN(hp);
	b = VRT_String(hp->ws, NULL, p, ap);
	if (b == NULL || *b == '\0') {
		WSL(w, SLT_LostHeader, fd, "%s", err);
	} else {
		http_SetH(hp, fld, b);
	}
	va_end(ap);
}

#define VRT_DO_HDR(obj, hdr, http, fld)				\
void								\
VRT_l_##obj##_##hdr(const struct sess *sp, const char *p, ...)	\
{								\
	va_list ap;						\
								\
	va_start(ap, p);					\
	vrt_do_string(sp->wrk, sp->fd,				\
	    http, fld, #obj "." #hdr, p, ap);			\
	va_end(ap);						\
}								\
								\
const char *							\
VRT_r_##obj##_##hdr(const struct sess *sp)			\
{								\
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);			\
	CHECK_OBJ_NOTNULL(http, HTTP_MAGIC);			\
	return (http->hd[fld].b);				\
}

VRT_DO_HDR(req,   request,	sp->req->http,		HTTP_HDR_REQ)
VRT_DO_HDR(req,   url,		sp->req->http,		HTTP_HDR_URL)
VRT_DO_HDR(req,   proto,	sp->req->http,		HTTP_HDR_PROTO)
VRT_DO_HDR(bereq, request,	sp->wrk->busyobj->bereq,	HTTP_HDR_REQ)
VRT_DO_HDR(bereq, url,		sp->wrk->busyobj->bereq,	HTTP_HDR_URL)
VRT_DO_HDR(bereq, proto,	sp->wrk->busyobj->bereq,	HTTP_HDR_PROTO)
VRT_DO_HDR(obj,   proto,	sp->req->obj->http,	HTTP_HDR_PROTO)
VRT_DO_HDR(obj,   response,	sp->req->obj->http,	HTTP_HDR_RESPONSE)
VRT_DO_HDR(resp,  proto,	sp->req->resp,		HTTP_HDR_PROTO)
VRT_DO_HDR(resp,  response,	sp->req->resp,		HTTP_HDR_RESPONSE)
VRT_DO_HDR(beresp,  proto,	sp->wrk->busyobj->beresp,	HTTP_HDR_PROTO)
VRT_DO_HDR(beresp,  response,	sp->wrk->busyobj->beresp, HTTP_HDR_RESPONSE)

/*--------------------------------------------------------------------*/

#define VRT_DO_STATUS(obj, http)				\
void								\
VRT_l_##obj##_status(const struct sess *sp, int num)		\
{								\
								\
	assert(num >= 100 && num <= 999);			\
	http->status = (uint16_t)num;				\
}								\
								\
int								\
VRT_r_##obj##_status(const struct sess *sp)			\
{								\
								\
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);			\
	return(http->status);					\
}

VRT_DO_STATUS(obj, sp->req->obj->http)
VRT_DO_STATUS(beresp, sp->wrk->busyobj->beresp)
VRT_DO_STATUS(resp, sp->req->resp)

/*--------------------------------------------------------------------*/

/* XXX: review this */
/* Add an objecthead to the saintmode list for the (hopefully) relevant
 * backend. Some double-up asserting here to avoid assert-errors when there
 * is no object.
 */
void
VRT_l_beresp_saintmode(const struct sess *sp, double a)
{
	struct trouble *new;
	struct trouble *tr;
	struct trouble *tr2;
	struct worker *wrk;
	struct vbc *vbc;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(sp->wrk, WORKER_MAGIC);
	wrk = sp->wrk;
	CHECK_OBJ_NOTNULL(wrk->busyobj, BUSYOBJ_MAGIC);
	vbc = wrk->busyobj->vbc;
	if (!vbc)
		return;
	CHECK_OBJ_NOTNULL(vbc, VBC_MAGIC);
	if (!vbc->backend)
		return;
	CHECK_OBJ_NOTNULL(vbc->backend, BACKEND_MAGIC);
	if (!sp->req->objcore)
		return;
	CHECK_OBJ_NOTNULL(sp->req->objcore, OBJCORE_MAGIC);

	/* Setting a negative holdoff period is a mistake. Detecting this
	 * when compiling the VCL would be better.
	 */
	assert(a > 0);

	ALLOC_OBJ(new, TROUBLE_MAGIC);
	AN(new);
	new->target = (uintptr_t)(sp->req->objcore->objhead);
	new->timeout = sp->t_req + a;

	/* Insert the new item on the list before the first item with a
	 * timeout at a later date (ie: sort by which entry will time out
	 * from the list
	 */
	Lck_Lock(&vbc->backend->mtx);
	VTAILQ_FOREACH_SAFE(tr, &vbc->backend->troublelist, list, tr2) {
		if (tr->timeout < new->timeout) {
			VTAILQ_INSERT_BEFORE(tr, new, list);
			new = NULL;
			break;
		}
	}

	/* Insert the item at the end if the list is empty or all other
	 * items have a longer timeout.
	 */
	if (new)
		VTAILQ_INSERT_TAIL(&vbc->backend->troublelist, new, list);

	Lck_Unlock(&vbc->backend->mtx);
}

/*--------------------------------------------------------------------*/

#define VBERESP(dir, type, onm, field)					\
void									\
VRT_l_##dir##_##onm(const struct sess *sp, type a)			\
{									\
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);				\
	sp->wrk->field = a;						\
}									\
									\
type									\
VRT_r_##dir##_##onm(const struct sess *sp)				\
{									\
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);				\
	return (sp->wrk->field);					\
}

VBERESP(beresp, unsigned, do_esi, busyobj->do_esi)
VBERESP(beresp, unsigned, do_gzip, busyobj->do_gzip)
VBERESP(beresp, unsigned, do_gunzip, busyobj->do_gunzip)
VBERESP(beresp, unsigned, do_stream, busyobj->do_stream)

/*--------------------------------------------------------------------*/

const char *
VRT_r_client_identity(const struct sess *sp)
{
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	if (sp->req->client_identity != NULL)
		return (sp->req->client_identity);
	else
		return (sp->addr);
}

void
VRT_l_client_identity(const struct sess *sp, const char *str, ...)
{
	va_list ap;
	char *b;

	va_start(ap, str);
	b = VRT_String(sp->req->http->ws, NULL, str, ap);
	va_end(ap);
	sp->req->client_identity = b;
}

/*--------------------------------------------------------------------*/

#define BEREQ_TIMEOUT(which)					\
void __match_proto__()						\
VRT_l_bereq_##which(struct sess *sp, double num)		\
{								\
								\
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);			\
	sp->wrk->which = (num > 0.0 ? num : 0.0);		\
}								\
								\
double __match_proto__()					\
VRT_r_bereq_##which(struct sess *sp)				\
{								\
								\
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);			\
	return(sp->wrk->which);					\
}

BEREQ_TIMEOUT(connect_timeout)
BEREQ_TIMEOUT(first_byte_timeout)
BEREQ_TIMEOUT(between_bytes_timeout)

/*--------------------------------------------------------------------*/

const char *
VRT_r_beresp_backend_name(const struct sess *sp)
{

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(sp->wrk->busyobj->vbc, VBC_MAGIC);
	return(sp->wrk->busyobj->vbc->backend->vcl_name);
}

struct sockaddr_storage *
VRT_r_beresp_backend_ip(const struct sess *sp)
{

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(sp->wrk->busyobj->vbc, VBC_MAGIC);
	return(sp->wrk->busyobj->vbc->addr);
}

int
VRT_r_beresp_backend_port(const struct sess *sp)
{

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(sp->wrk->busyobj->vbc, VBC_MAGIC);
	return (VTCP_port(sp->wrk->busyobj->vbc->addr));
}

const char * __match_proto__()
VRT_r_beresp_storage(struct sess *sp)
{
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	if (sp->req->storage_hint != NULL)
		return (sp->req->storage_hint);
	else
		return (NULL);
}

void __match_proto__()
VRT_l_beresp_storage(struct sess *sp, const char *str, ...)
{
	va_list ap;
	char *b;

	va_start(ap, str);
	b = VRT_String(sp->wrk->ws, NULL, str, ap);
	va_end(ap);
	sp->req->storage_hint = b;
}

double
VRT_r_beresp_stream_pass_bufsize(const struct sess *sp)
{
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(sp->wrk->busyobj, BUSYOBJ_MAGIC);
	return (sp->wrk->busyobj->stream_pass_bufsize);
}

void
VRT_l_beresp_stream_pass_bufsize(const struct sess *sp, double val)
{
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(sp->wrk->busyobj, BUSYOBJ_MAGIC);
	if (val >= 0.)
		sp->wrk->busyobj->stream_pass_bufsize = val;
	else
		sp->wrk->busyobj->stream_pass_bufsize = 0;
}

int
VRT_r_beresp_stream_tokens(const struct sess *sp)
{
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(sp->wrk->busyobj, BUSYOBJ_MAGIC);
	return (sp->wrk->busyobj->stream_tokens);
}

void
VRT_l_beresp_stream_tokens(const struct sess *sp, int val)
{
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(sp->wrk->busyobj, BUSYOBJ_MAGIC);
	if (val >= 1)
		sp->wrk->busyobj->stream_tokens = val;
	else
		sp->wrk->busyobj->stream_tokens = 1;
}


/*--------------------------------------------------------------------*/

void
VRT_l_req_backend(const struct sess *sp, struct director *be)
{
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	sp->req->director = be;
}

struct director *
VRT_r_req_backend(const struct sess *sp)
{
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	return (sp->req->director);
}

/*--------------------------------------------------------------------*/

void
VRT_l_req_esi(const struct sess *sp, unsigned process_esi)
{
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	/*
	 * Only allow you to turn of esi in the main request
	 * else everything gets confused
	 */
	if(sp->req->esi_level == 0)
		sp->req->disable_esi = !process_esi;
}

unsigned
VRT_r_req_esi(const struct sess *sp)
{
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	return (!sp->req->disable_esi);
}

int
VRT_r_req_esi_level(const struct sess *sp)
{

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	return(sp->req->esi_level);
}

/*--------------------------------------------------------------------*/

unsigned __match_proto__()
VRT_r_req_can_gzip(struct sess *sp)
{

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	return (RFC2616_Req_Gzip(sp));
}


/*--------------------------------------------------------------------*/

int
VRT_r_req_restarts(const struct sess *sp)
{

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	return (sp->req->restarts);
}

/*--------------------------------------------------------------------
 * NB: TTL is relative to when object was created, whereas grace and
 * keep are relative to ttl.
 */

#define VRT_DO_EXP(which, exp, fld, offset, extra)		\
								\
void __match_proto__()						\
VRT_l_##which##_##fld(struct sess *sp, double a)		\
{								\
								\
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);			\
	if (a > 0.)						\
		a += offset;					\
	EXP_Set_##fld(&exp, a);					\
	extra;							\
}								\
								\
double __match_proto__()					\
VRT_r_##which##_##fld(struct sess *sp)				\
{								\
								\
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);			\
	return(EXP_Get_##fld(&exp) - offset);			\
}

static void
vrt_wsp_exp(const struct sess *sp, unsigned xid, const struct exp *e)
{
	WSP(sp, SLT_TTL, "%u VCL %.0f %.0f %.0f %.0f %.0f",
	    xid, e->ttl - (sp->t_req - e->entered), e->grace, e->keep,
	    sp->t_req, e->age + (sp->t_req - e->entered));
}

VRT_DO_EXP(req, sp->req->exp, ttl, 0, )
VRT_DO_EXP(req, sp->req->exp, grace, 0, )
VRT_DO_EXP(req, sp->req->exp, keep, 0, )

VRT_DO_EXP(obj, sp->req->obj->exp, grace, 0,
   EXP_Rearm(sp->req->obj);
   vrt_wsp_exp(sp, sp->req->obj->xid, &sp->req->obj->exp);)
VRT_DO_EXP(obj, sp->req->obj->exp, ttl, (sp->t_req - sp->req->obj->exp.entered),
   EXP_Rearm(sp->req->obj);
   vrt_wsp_exp(sp, sp->req->obj->xid, &sp->req->obj->exp);)
VRT_DO_EXP(obj, sp->req->obj->exp, keep, 0,
   EXP_Rearm(sp->req->obj);
   vrt_wsp_exp(sp, sp->req->obj->xid, &sp->req->obj->exp);)

VRT_DO_EXP(beresp, sp->wrk->busyobj->exp, grace, 0,
   vrt_wsp_exp(sp, sp->req->xid, &sp->wrk->busyobj->exp);)
VRT_DO_EXP(beresp, sp->wrk->busyobj->exp, ttl, 0,
   vrt_wsp_exp(sp, sp->req->xid, &sp->wrk->busyobj->exp);)
VRT_DO_EXP(beresp, sp->wrk->busyobj->exp, keep, 0,
   vrt_wsp_exp(sp, sp->req->xid, &sp->wrk->busyobj->exp);)

/*--------------------------------------------------------------------
 * req.xid
 */

const char * __match_proto__()
VRT_r_req_xid(struct sess *sp)
{
	char *p;
	int size;
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);

	size = snprintf(NULL, 0, "%u", sp->req->xid) + 1;
	AN(p = WS_Alloc(sp->req->http->ws, size));
	assert(snprintf(p, size, "%u", sp->req->xid) < size);
	return (p);
}

/*--------------------------------------------------------------------*/

#define REQ_BOOL(hash_var)					\
void __match_proto__()						\
VRT_l_req_##hash_var(struct sess *sp, unsigned val)		\
{								\
								\
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);			\
	sp->req->hash_var = val ? 1 : 0;				\
}								\
								\
unsigned __match_proto__()					\
VRT_r_req_##hash_var(struct sess *sp)				\
{								\
								\
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);			\
	return(sp->req->hash_var);					\
}

REQ_BOOL(hash_ignore_busy)
REQ_BOOL(hash_always_miss)

/*--------------------------------------------------------------------*/

struct sockaddr_storage *
VRT_r_client_ip(struct sess *sp)
{

	return (&sp->sockaddr);
}

struct sockaddr_storage *
VRT_r_server_ip(struct sess *sp)
{
	int i;

	if (sp->mysockaddr.ss_family == AF_UNSPEC) {
		i = getsockname(sp->fd,
		    (void*)&sp->mysockaddr, &sp->mysockaddrlen);
		assert(VTCP_Check(i));
	}

	return (&sp->mysockaddr);
}

const char*
VRT_r_server_identity(struct sess *sp)
{
	(void)sp;

	if (heritage.identity[0] != '\0')
		return (heritage.identity);
	else
		return (heritage.name);
}


const char*
VRT_r_server_hostname(struct sess *sp)
{
	(void)sp;

	if (vrt_hostname[0] == '\0')
		AZ(gethostname(vrt_hostname, sizeof(vrt_hostname)));

	return (vrt_hostname);
}

/*--------------------------------------------------------------------
 * XXX: This is pessimistically silly
 */

int
VRT_r_server_port(struct sess *sp)
{
	int i;

	if (sp->mysockaddr.ss_family == AF_UNSPEC) {
		i = getsockname(sp->fd,
		    (void*)&sp->mysockaddr, &sp->mysockaddrlen);
		assert(VTCP_Check(i));
	}
	return (VTCP_port(&sp->mysockaddr));
}

/*--------------------------------------------------------------------*/

int
VRT_r_obj_hits(const struct sess *sp)
{

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(sp->req->obj, OBJECT_MAGIC);	/* XXX */
	return (sp->req->obj->hits);
}

double
VRT_r_obj_lastuse(const struct sess *sp)
{

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(sp->req->obj, OBJECT_MAGIC);	/* XXX */
	return (VTIM_real() - sp->req->obj->last_use);
}

unsigned
VRT_r_req_backend_healthy(const struct sess *sp)
{
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(sp->req->director, DIRECTOR_MAGIC);
	return (VDI_Healthy(sp->req->director, sp));
}

