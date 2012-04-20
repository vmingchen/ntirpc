/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Sun Microsystems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * clnt_tcp.c, Implements a TCP/IP based, client side RPC.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 * TCP based RPC supports 'batched calls'.
 * A sequence of calls may be batched-up in a send buffer.  The rpc call
 * return immediately to the client even though the call was not necessarily
 * sent.  The batching occurs if the results' xdr routine is NULL (0) AND
 * the rpc timeout value is zero (see clnt.h, rpc).
 *
 * Clients should NOT casually batch calls that in fact return results; that is,
 * the server side should be aware that a call is batched and not produce any
 * return message.  Batched calls that produce many result messages can
 * deadlock (netlock) the client and the server....
 *
 * Now go hang yourself.
 */
#include <pthread.h>

#include <reentrant.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/syslog.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <rpc/rpc.h>
#include "rpc_com.h"

#include "clnt_internal.h"
#include "vc_lock.h"
#include <rpc/svc_rqst.h>

#define CMGROUP_MAX    16
#define SCM_CREDS      0x03            /* process creds (struct cmsgcred) */

/*
 * Credentials structure, used to verify the identity of a peer
 * process that has sent us a message. This is allocated by the
 * peer process but filled in by the kernel. This prevents the
 * peer from lying about its identity. (Note that cmcred_groups[0]
 * is the effective GID.)
 */
struct cmsgcred {
        pid_t   cmcred_pid;             /* PID of sending process */
        uid_t   cmcred_uid;             /* real UID of sending process */
        uid_t   cmcred_euid;            /* effective UID of sending process */
        gid_t   cmcred_gid;             /* real GID of sending process */
        short   cmcred_ngroups;         /* number or groups */
        gid_t   cmcred_groups[CMGROUP_MAX];     /* groups */
};

struct cmessage {
        struct cmsghdr cmsg;
        struct cmsgcred cmcred;
};

static enum clnt_stat clnt_vc_call(CLIENT *, rpcproc_t, xdrproc_t, void *,
    xdrproc_t, void *, struct timeval);
static void clnt_vc_geterr(CLIENT *, struct rpc_err *);
static bool_t clnt_vc_freeres(CLIENT *, xdrproc_t, void *);
static void clnt_vc_abort(CLIENT *);
static bool_t clnt_vc_control(CLIENT *, u_int, void *);
void clnt_vc_destroy(CLIENT *);
static struct clnt_ops *clnt_vc_ops(void);
static bool_t time_not_ok(struct timeval *);
static int read_vc(void *, void *, int);
static int write_vc(void *, void *, int);

#include "clnt_internal.h"

/*
 *      This machinery implements per-fd locks for MT-safety.  It is not
 *      sufficient to do per-CLIENT handle locks for MT-safety because a
 *      user may create more than one CLIENT handle with the same fd behind
 *      it.  Therfore, we allocate an array of flags (vc_fd_locks), protected
 *      by the clnt_fd_lock mutex, and an array (vc_cv) of condition variables
 *      similarly protected.  Vc_fd_lock[fd] == 1 => a call is active on some
 *      CLIENT handle created for that fd.
 *
 *      The current implementation holds locks across the entire RPC and reply.
 *      Yes, this is silly, and as soon as this code is proven to work, this
 *      should be the first thing fixed.  One step at a time.
 *
 *      The ONC RPC record marking (RM) standard (RFC 5531, s. 11) does not
 *      provide for mixed call and reply fragment reassembly, so writes to the 
 *      bytestream with MUST be record-wise atomic.  It appears that an
 *      implementation may interleave call and reply messages on distinct
 *      conversations.  This is not incompatible with holding a transport
 *      exclusive locked across a full call and reply, bud does require new
 *      control tranfers and delayed decoding support in the transport.  For
 *      duplex channels, full coordination is required between client and
 *      server tranpsorts sharing an underlying bytestream (Matt).
 */
extern mutex_t  clnt_fd_lock; /* XXX still protects creation */

static const char clnt_vc_errstr[] = "%s : %s";
static const char clnt_vc_str[] = "clnt_vc_create";
static const char clnt_read_vc_str[] = "read_vc";
static const char __no_mem_str[] = "out of memory";

/*
 * If event processing on the xprt associated with cl is not
 * currently blocked, do so.
 *
 * Returns TRUE if blocking state was changed, FALSE otherwise.
 *
 * Locked on entry.
 */
bool_t
cond_block_events_client(CLIENT *cl)
{
    struct ct_data *ct = (struct ct_data *) cl->cl_private;
    if ((ct->ct_duplex.ct_flags & CT_FLAG_DUPLEX) &&
        (! (ct->ct_duplex.ct_flags & CT_FLAG_EVENTS_BLOCKED))) {
        SVCXPRT *xprt = ct->ct_duplex.ct_xprt;
        assert(xprt);
        ct->ct_duplex.ct_flags |= CT_FLAG_EVENTS_BLOCKED;
        (void) svc_rqst_block_events(xprt, SVC_RQST_FLAG_NONE);
        return (TRUE);
    }
    return (FALSE);
}

/* Restore event processing on the xprt associated with cl.
 * Locked. */
void
cond_unblock_events_client(CLIENT *cl)
{
    struct ct_data *ct = (struct ct_data *) cl->cl_private;
    if (ct->ct_duplex.ct_flags & CT_FLAG_EVENTS_BLOCKED) {
        SVCXPRT *xprt = ct->ct_duplex.ct_xprt;
        assert(xprt);
        ct->ct_duplex.ct_flags &= ~CT_FLAG_EVENTS_BLOCKED;
        (void) svc_rqst_unblock_events(xprt, SVC_RQST_FLAG_NONE);
    }
}

/*
 * Create a client handle for a connection.
 * Default options are set, which the user can change using clnt_control()'s.
 * The rpc/vc package does buffering similar to stdio, so the client
 * must pick send and receive buffer sizes, 0 => use the default.
 * NB: fd is copied into a private area.
 * NB: The rpch->cl_auth is set null authentication. Caller may wish to
 * set this something more useful.
 *
 * fd should be an open socket
 */
CLIENT *
clnt_vc_create(fd, raddr, prog, vers, sendsz, recvsz)
	int fd;				/* open file descriptor */
	const struct netbuf *raddr;	/* servers address */
	const rpcprog_t prog;			/* program number */
	const rpcvers_t vers;			/* version number */
	u_int sendsz;			/* buffer recv size */
	u_int recvsz;			/* buffer send size */
{
    return (clnt_vc_create2(fd, raddr, prog, vers, sendsz, recvsz,
			    CLNT_CREATE_FLAG_CONNECT));
}

CLIENT *
clnt_vc_create2(fd, raddr, prog, vers, sendsz, recvsz, flags)
	int fd;				/* open file descriptor */
	const struct netbuf *raddr;	/* servers address */
	const rpcprog_t prog;			/* program number */
	const rpcvers_t vers;			/* version number */
	u_int sendsz;			/* buffer recv size */
	u_int recvsz;			/* buffer send size */
	u_int flags;
{
	CLIENT *cl;			/* client handle */
	struct ct_data *ct = NULL;
	struct timeval now;
	struct rpc_msg call_msg;
	static u_int32_t disrupt;
	sigset_t mask;
	sigset_t newmask;
	struct sockaddr_storage ss;
	socklen_t slen;
	struct __rpc_sockinfo si;

	if (disrupt == 0)
		disrupt = (u_int32_t)(long)raddr;

	cl = (CLIENT *)mem_alloc(sizeof (*cl));
	ct = (struct ct_data *)mem_alloc(sizeof (*ct));
	if ((cl == (CLIENT *)NULL) || (ct == (struct ct_data *)NULL)) {
		(void) syslog(LOG_ERR, clnt_vc_errstr,
		    clnt_vc_str, __no_mem_str);
		rpc_createerr.cf_stat = RPC_SYSTEMERROR;
		rpc_createerr.cf_error.re_errno = errno;
		goto err;
	}
	ct->ct_addr.buf = NULL;
	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);
	mutex_lock(&clnt_fd_lock);

	/*
	 * XXX - fvdl connecting while holding a mutex?
	 */
	if (flags & CLNT_CREATE_FLAG_CONNECT) {
	    slen = sizeof ss;
	    if (getpeername(fd, (struct sockaddr *)&ss, &slen) < 0) {
		if (errno != ENOTCONN) {
		    rpc_createerr.cf_stat = RPC_SYSTEMERROR;
		    rpc_createerr.cf_error.re_errno = errno;
		    mutex_unlock(&clnt_fd_lock);
		    thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
		    goto err;
		}
		if (connect(fd, (struct sockaddr *)raddr->buf, raddr->len) < 0){
		    rpc_createerr.cf_stat = RPC_SYSTEMERROR;
		    rpc_createerr.cf_error.re_errno = errno;
		    mutex_unlock(&clnt_fd_lock);
		    thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
		    goto err;
		}
	    }
	} /* FLAG_CONNECT */

	mutex_unlock(&clnt_fd_lock);
	if (!__rpc_fd2sockinfo(fd, &si))
		goto err;
	thr_sigsetmask(SIG_SETMASK, &(mask), NULL);

	ct->ct_closeit = FALSE;

	/*
	 * Set up private data struct
	 */
	ct->ct_fd = fd;
	ct->ct_wait.tv_usec = 0;
	ct->ct_waitset = FALSE;
	ct->ct_addr.buf = mem_alloc(raddr->maxlen);
	if (ct->ct_addr.buf == NULL)
		goto err;
	memcpy(ct->ct_addr.buf, raddr->buf, raddr->len);
	ct->ct_addr.len = raddr->len;
	ct->ct_addr.maxlen = raddr->maxlen;

	/*
	 * Initialize call message
	 */
	(void)gettimeofday(&now, NULL);
	call_msg.rm_xid = ((u_int32_t)++disrupt) ^ __RPC_GETXID(&now);
	call_msg.rm_direction = CALL;
	call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
	call_msg.rm_call.cb_prog = (u_int32_t)prog;
	call_msg.rm_call.cb_vers = (u_int32_t)vers;

	/*
	 * pre-serialize the static part of the call msg and stash it away
	 */
	xdrmem_create(&(ct->ct_xdrs), ct->ct_u.ct_mcallc, MCALL_MSG_SIZE,
	    XDR_ENCODE);
	if (! xdr_callhdr(&(ct->ct_xdrs), &call_msg)) {
		if (ct->ct_closeit) {
			(void)close(fd);
		}
		goto err;
	}
	ct->ct_mpos = XDR_GETPOS(&(ct->ct_xdrs));
	XDR_DESTROY(&(ct->ct_xdrs));

	/*
	 * Create a client handle which uses xdrrec for serialization
	 * and authnone for authentication.
	 */
	cl->cl_ops = clnt_vc_ops();
	cl->cl_private = ct;

        /*
         * Register lock channel (sync)
         */
        vc_lock_init_cl(cl);

        /*
         * Setup auth
         */
	cl->cl_auth = authnone_create();

	sendsz = __rpc_get_t_size(si.si_af, si.si_proto, (int)sendsz);
	recvsz = __rpc_get_t_size(si.si_af, si.si_proto, (int)recvsz);
	xdrrec_create(&(ct->ct_xdrs), sendsz, recvsz,
	    cl->cl_private, read_vc, write_vc);
	return (cl);

err:
	if (cl) {
		if (ct) {
			if (ct->ct_addr.len)
				mem_free(ct->ct_addr.buf, ct->ct_addr.len);
			mem_free(ct, sizeof (struct ct_data));
		}
		if (cl)
			mem_free(cl, sizeof (CLIENT));
	}
	return ((CLIENT *)NULL);
}

#define vc_call_return(r) do { result=(r); goto out; } while (0);
static enum clnt_stat
clnt_vc_call(cl, proc, xdr_args, args_ptr, xdr_results, results_ptr, timeout)
	CLIENT *cl;
	rpcproc_t proc;
	xdrproc_t xdr_args;
	void *args_ptr;
	xdrproc_t xdr_results;
	void *results_ptr;
	struct timeval timeout;
{
    struct ct_data *ct = (struct ct_data *) cl->cl_private;
    XDR *xdrs = &(ct->ct_xdrs);
    struct rpc_msg *msg = alloc_rpc_msg();
    enum clnt_stat result;
    u_int32_t x_id;
    u_int32_t *msg_x_id = &ct->ct_u.ct_mcalli;    /* yuk */
    bool_t shipnow, ev_blocked, duplex;
    SVCXPRT *duplex_xprt = NULL;
    int refreshes = 2;
    sigset_t mask;

    assert(cl != NULL);
    vc_fd_lock_c(cl, &mask);

    /* two basic strategies are possible here--this stage
     * assumes the cost of updating the epoll (or select)
     * registration of the connected transport is preferable
     * to muxing reply events with call events through the/a
     * request dispatcher (in the common case).
     *
     * the CT_FLAG_EPOLL_ACTIVE is intended to indicate the
     * inverse strategy, which would place the current thread
     * on a waitq here (in the common case). */

    duplex = ct->ct_duplex.ct_flags & CT_FLAG_DUPLEX;
    if (duplex)
        duplex_xprt = ct->ct_duplex.ct_xprt;

    ev_blocked = cond_block_events_client(cl);

    if (!ct->ct_waitset) {
        /* If time is not within limits, we ignore it. */
        if (time_not_ok(&timeout) == FALSE)
            ct->ct_wait = timeout;
    }

    shipnow =
        (xdr_results == NULL && timeout.tv_sec == 0
         && timeout.tv_usec == 0) ? FALSE : TRUE;

call_again:
    xdrs->x_op = XDR_ENCODE;
    ct->ct_error.re_status = RPC_SUCCESS;
    x_id = ntohl(--(*msg_x_id));

    if ((! XDR_PUTBYTES(xdrs, ct->ct_u.ct_mcallc, ct->ct_mpos)) ||
        (! XDR_PUTINT32(xdrs, (int32_t *)&proc)) ||
        (! AUTH_MARSHALL(cl->cl_auth, xdrs)) ||
        (! AUTH_WRAP(cl->cl_auth, xdrs, xdr_args, args_ptr))) {
        if (ct->ct_error.re_status == RPC_SUCCESS)
            ct->ct_error.re_status = RPC_CANTENCODEARGS;
        (void)xdrrec_endofrecord(xdrs, TRUE);
        vc_call_return (ct->ct_error.re_status);
    }
    if (! xdrrec_endofrecord(xdrs, shipnow))
        vc_call_return (ct->ct_error.re_status = RPC_CANTSEND);
    if (! shipnow)
        vc_call_return (RPC_SUCCESS);
    /*
     * Hack to provide rpc-based message passing
     */
    if (timeout.tv_sec == 0 && timeout.tv_usec == 0)
        vc_call_return (ct->ct_error.re_status = RPC_TIMEDOUT);
    /*
     * Keep receiving until we get a valid transaction id
     */
    /*
     * I think we -can- do this, but we need to queue any message
     * that isn't the desired reply (Matt)
     */
    xdrs->x_op = XDR_DECODE;
    while (TRUE) {
        msg->acpted_rply.ar_verf = _null_auth;
        msg->acpted_rply.ar_results.where = NULL;
        msg->acpted_rply.ar_results.proc = (xdrproc_t)xdr_void;
        if (! xdrrec_skiprecord(xdrs)) {
            __warnx("%s: error at skiprecord",
                    __func__);
            vc_call_return (ct->ct_error.re_status);
        }
        /* now decode and validate the response header */
        if (! xdr_dplx_msg(xdrs, msg)) {
            __warnx("%s: error at xdr_dplx_msg",
                __func__);
            if (ct->ct_error.re_status == RPC_SUCCESS) {
                __warnx("%s: error at ct_error (direction == %d, status == %d)",
                        __func__,
                        msg->rm_direction,
                        ct->ct_error.re_status);
                continue;
            }
            vc_call_return (ct->ct_error.re_status);
        }
        __warnx("%s: successful xdr_dplx_msg (direction==%d)\n",
                __func__, msg->rm_direction);
        /* switch on direction */
        switch (msg->rm_direction) {
        case REPLY:
            if (msg->rm_xid == x_id)
                goto replied;
            break;
        case CALL:
            /* XXX queue or dispatch.  on return from xp_dispatch,
             * duplex_msg points to a (potentially new, junk) rpc_msg
             * object owned by this call path */
            if (duplex) {
                struct cf_conn *cd;
                assert(duplex_xprt);
                cd = (struct cf_conn *) duplex_xprt->xp_p1;
                cd->x_id = msg->rm_xid;
                __warnx("%s: call intercepted, dispatching (x_id == %d)\n",
                        __func__, cd->x_id);
                duplex_xprt->xp_ops2->xp_dispatch(duplex_xprt, &msg);
            }
            break;
        default:
            break;
        }
    } /* while (TRUE) */

    /*
     * process header
     */
replied:
    _seterr_reply(msg, &(ct->ct_error));
    if (ct->ct_error.re_status == RPC_SUCCESS) {
        if (! AUTH_VALIDATE(cl->cl_auth, &msg->acpted_rply.ar_verf)) {
            ct->ct_error.re_status = RPC_AUTHERROR;
            ct->ct_error.re_why = AUTH_INVALIDRESP;
        } else if (! AUTH_UNWRAP(cl->cl_auth, xdrs,
                                 xdr_results, results_ptr)) {
            if (ct->ct_error.re_status == RPC_SUCCESS)
                ct->ct_error.re_status = RPC_CANTDECODERES;
        }
        /* free verifier ... */
        if (msg->acpted_rply.ar_verf.oa_base != NULL) {
            xdrs->x_op = XDR_FREE;
            (void)xdr_opaque_auth(xdrs, &(msg->acpted_rply.ar_verf));
        }
    }  /* end successful completion */
    else {
        /* maybe our credentials need to be refreshed ... */
        if (refreshes-- && AUTH_REFRESH(cl->cl_auth, &msg))
            goto call_again;
    }  /* end of unsuccessful completion */
    vc_call_return (ct->ct_error.re_status);

out:
    if (ev_blocked)
        cond_unblock_events_client(cl);

    vc_fd_unlock_c(cl, &mask);
    free_rpc_msg(msg);

    return (result);
}

static void
clnt_vc_geterr(cl, errp)
	CLIENT *cl;
	struct rpc_err *errp;
{
	struct ct_data *ct;

	assert(cl != NULL);
	assert(errp != NULL);

	ct = (struct ct_data *) cl->cl_private;
	*errp = ct->ct_error;
}

static bool_t
clnt_vc_freeres(cl, xdr_res, res_ptr)
	CLIENT *cl;
	xdrproc_t xdr_res;
	void *res_ptr;
{
	struct ct_data *ct;
	XDR *xdrs;
	bool_t dummy;
	sigset_t mask, newmask;

	assert(cl != NULL);

	ct = (struct ct_data *)cl->cl_private;
	xdrs = &(ct->ct_xdrs);

        /* Handle our own signal mask here, the signal section is
         * larger than the wait (not 100% clear why) */
	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);

        vc_fd_wait_c(cl, rpc_flag_clear);        

	xdrs->x_op = XDR_FREE;
	dummy = (*xdr_res)(xdrs, res_ptr);

	thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
        vc_fd_signal_c(cl, VC_LOCK_FLAG_NONE);

	return dummy;
}

/*ARGSUSED*/
static void
clnt_vc_abort(cl)
	CLIENT *cl;
{
}

static bool_t
clnt_vc_control(cl, request, info)
	CLIENT *cl;
	u_int request;
	void *info;
{
	struct ct_data *ct;
	void *infop = info;
	sigset_t mask;

	assert(cl);

	ct = (struct ct_data *)cl->cl_private;
        vc_fd_lock_c(cl, &mask);

	switch (request) {
	case CLSET_FD_CLOSE:
            ct->ct_closeit = TRUE;
            vc_fd_unlock_c(cl, &mask);
            return (TRUE);
	case CLSET_FD_NCLOSE:
            ct->ct_closeit = FALSE;
            vc_fd_unlock_c(cl, &mask);
            return (TRUE);
	default:
            break;
	}

	/* for other requests which use info */
	if (info == NULL) {
            vc_fd_unlock_c(cl, &mask);
            return (FALSE);
	}
	switch (request) {
	case CLSET_TIMEOUT:
            if (time_not_ok((struct timeval *)info)) {
                vc_fd_unlock_c(cl, &mask);
                return (FALSE);
            }
            ct->ct_wait = *(struct timeval *)infop;
            ct->ct_waitset = TRUE;
            break;
	case CLGET_TIMEOUT:
            *(struct timeval *)infop = ct->ct_wait;
            break;
	case CLGET_SERVER_ADDR:
            (void) memcpy(info, ct->ct_addr.buf, (size_t)ct->ct_addr.len);
            break;
	case CLGET_FD:
            *(int *)info = ct->ct_fd;
            break;
	case CLGET_SVC_ADDR:
            /* The caller should not free this memory area */
            *(struct netbuf *)info = ct->ct_addr;
            break;
	case CLSET_SVC_ADDR:		/* set to new address */
            vc_fd_unlock_c(cl, &mask);
            return (FALSE);
	case CLGET_XID:
            /*
             * use the knowledge that xid is the
             * first element in the call structure
             * This will get the xid of the PREVIOUS call
             */
            *(u_int32_t *)info =
                ntohl(*(u_int32_t *)(void *)&ct->ct_u.ct_mcalli);
            break;
	case CLSET_XID:
            /* This will set the xid of the NEXT call */
            *(u_int32_t *)(void *)&ct->ct_u.ct_mcalli =
                htonl(*((u_int32_t *)info) + 1);
            /* increment by 1 as clnt_vc_call() decrements once */
            break;
	case CLGET_VERS:
            /*
             * This RELIES on the information that, in the call body,
             * the version number field is the fifth field from the
             * begining of the RPC header. MUST be changed if the
             * call_struct is changed
             */
            *(u_int32_t *)info =
                ntohl(*(u_int32_t *)(void *)
                      (ct->ct_u.ct_mcallc + 4 * BYTES_PER_XDR_UNIT));
            break;

	case CLSET_VERS:
            *(u_int32_t *)(void *)
                (ct->ct_u.ct_mcallc + 4 * BYTES_PER_XDR_UNIT) =
                htonl(*(u_int32_t *)info);
		break;

	case CLGET_PROG:
            /*
             * This RELIES on the information that, in the call body,
             * the program number field is the fourth field from the
             * begining of the RPC header. MUST be changed if the
             * call_struct is changed
             */
            *(u_int32_t *)info =
                ntohl(*(u_int32_t *)(void *)
                      (ct->ct_u.ct_mcallc + 3 * BYTES_PER_XDR_UNIT));
            break;

	case CLSET_PROG:
            *(u_int32_t *)(void *)
                (ct->ct_u.ct_mcallc + 3 * BYTES_PER_XDR_UNIT) =
                htonl(*(u_int32_t *)info);
            break;

	default:
            vc_fd_unlock_c(cl, &mask);
            return (FALSE);
	}

        vc_fd_unlock_c(cl, &mask);
	return (TRUE);
}


void
clnt_vc_destroy(cl)
	CLIENT *cl;
{
	struct ct_data *ct = (struct ct_data *) cl->cl_private;
	sigset_t mask, newmask;

	ct = (struct ct_data *) cl->cl_private;

        /* Handle our own signal mask here, the signal section is
         * larger than the wait (not 100% clear why) */
	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);
        vc_fd_wait_c(cl, rpc_flag_clear);

	if (ct->ct_closeit && ct->ct_fd != -1)
            (void)close(ct->ct_fd);
	XDR_DESTROY(&(ct->ct_xdrs));
	if (ct->ct_addr.buf)
            __free(ct->ct_addr.buf);

        vc_fd_signal_c(cl, VC_LOCK_FLAG_NONE); /* XXX moved before free */
        vc_lock_unref_clnt(cl);

	mem_free(ct, sizeof(struct ct_data));

	if (cl->cl_netid && cl->cl_netid[0])
            mem_free(cl->cl_netid, strlen(cl->cl_netid) +1);
	if (cl->cl_tp && cl->cl_tp[0])
            mem_free(cl->cl_tp, strlen(cl->cl_tp) +1);
	mem_free(cl, sizeof(CLIENT));
	mutex_unlock(&clnt_fd_lock);
	thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
}

/*
 * Interface between xdr serializer and tcp connection.
 * Behaves like the system calls, read & write, but keeps some error state
 * around for the rpc level.
 */
static int
read_vc(ctp, buf, len)
	void *ctp;
	void *buf;
	int len;
{
	struct ct_data *ct = (struct ct_data *)ctp;
	struct pollfd fd;
	int milliseconds = (int)((ct->ct_wait.tv_sec * 1000) +
	    (ct->ct_wait.tv_usec / 1000));

	if (len == 0)
		return (0);

        /* if ct->ct_duplex.ct_flags & CT_FLAG_DUPLEX, in the current
         * strategy (cf. clnt_vc_call and the duplex-aware getreq
         * implementation), we assert that the current thread may safely
         * block in poll (though we are not constrained to blocking
         * semantics) */

        fd.fd = ct->ct_fd;
        fd.events = POLLIN;
        for (;;) {
            switch (poll(&fd, 1, milliseconds)) {
            case 0:
                ct->ct_error.re_status = RPC_TIMEDOUT;
                return (-1);

            case -1:
                if (errno == EINTR)
                    continue;
                ct->ct_error.re_status = RPC_CANTRECV;
                ct->ct_error.re_errno = errno;
                return (-1);
            }
            break;
        }

	len = read(ct->ct_fd, buf, (size_t)len);

	switch (len) {
	case 0:
		/* premature eof */
		ct->ct_error.re_errno = ECONNRESET;
		ct->ct_error.re_status = RPC_CANTRECV;
		len = -1;  /* it's really an error */
		break;

	case -1:
		ct->ct_error.re_errno = errno;
		ct->ct_error.re_status = RPC_CANTRECV;
		break;
	}
	return (len);
}

static int
write_vc(ctp, buf, len)
	void *ctp;
	void *buf;
	int len;
{
	struct ct_data *ct = (struct ct_data *)ctp;
	int i = 0, cnt;

	for (cnt = len; cnt > 0; cnt -= i, buf += i) {
	    if ((i = write(ct->ct_fd, buf, (size_t)cnt)) == -1) {
		ct->ct_error.re_errno = errno;
		ct->ct_error.re_status = RPC_CANTSEND;
		return (-1);
	    }
	}
	return (len);
}

static void *
clnt_vc_xdrs(cl)
	CLIENT *cl;
{
	struct ct_data *ct = (struct ct_data *) cl->cl_private;
	return ((void *) & ct->ct_xdrs);
}

static struct clnt_ops *
clnt_vc_ops()
{
	static struct clnt_ops ops;
	extern mutex_t  ops_lock;
	sigset_t mask, newmask;

	/* VARIABLES PROTECTED BY ops_lock: ops */

	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);
	mutex_lock(&ops_lock);
	if (ops.cl_call == NULL) {
		ops.cl_call = clnt_vc_call;
		ops.cl_xdrs = clnt_vc_xdrs;
		ops.cl_abort = clnt_vc_abort;
		ops.cl_geterr = clnt_vc_geterr;
		ops.cl_freeres = clnt_vc_freeres;
		ops.cl_destroy = clnt_vc_destroy;
		ops.cl_control = clnt_vc_control;
	}
	mutex_unlock(&ops_lock);
	thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
	return (&ops);
}

/*
 * Make sure that the time is not garbage.   -1 value is disallowed.
 * Note this is different from time_not_ok in clnt_dg.c
 */
static bool_t
time_not_ok(t)
	struct timeval *t;
{
	return (t->tv_sec <= -1 || t->tv_sec > 100000000 ||
		t->tv_usec <= -1 || t->tv_usec > 1000000);
}
