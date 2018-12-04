/*
 * Copyright (C) 1994-2018 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * For a copy of the commercial license terms and conditions,
 * go to: (http://www.pbspro.com/UserArea/agreement.html)
 * or contact the Altair Legal Department.
 *
 * Altair’s dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of PBS Pro and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair’s trademarks, including but not limited to "PBS™",
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
 * trademark licensing policies.
 *
 */
/**
 * @file    req_rerun.c
 *
 * @brief
 * 		req_rerun.c - functions dealing with a Rerun Job Request
 *
 * Included functions are:
 * 	post_rerun()
 * 	force_reque()
 * 	req_rerunjob()
 * 	timeout_rerun_request()
 * 	req_rerunjob2()
 *
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <sys/types.h>
#include "libpbs.h"
#include <signal.h>
#include "server_limits.h"
#include "list_link.h"
#include "work_task.h"
#include "attribute.h"
#include "server.h"
#include "credential.h"
#include "batch_request.h"
#include "job.h"
#include "pbs_error.h"
#include "log.h"
#include "acct.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include "net_connect.h"


/* Private Function local to this file */
static void req_rerunjob2(struct batch_request *preq, job *pjob);

/* Global Data Items: */

extern char *msg_manager;
extern char *msg_jobrerun;
extern time_t time_now;
extern job  *chk_job_request(char *, struct batch_request *, int *);



/**
 * @brief
 * 		post_rerun - handler for reply from mom on signal_job sent in req_rerunjob
 *		If mom acknowledged the signal, then all is ok.
 *		If mom rejected the signal for unknown jobid, and force is set by the
 *		original client for a non manager as indicated by the preq->rq_extra being zero,
 *		then do local requeue.
 *
 * @param[in]	pwt	-	work task structure which contains the reply from mom
 */

static void
post_rerun(struct work_task *pwt)
{
	job	*pjob;
	struct batch_request *preq;

	preq = (struct batch_request *)pwt->wt_parm1;

	if (preq->rq_reply.brp_code != 0) {
		if ((pjob = find_job(preq->rq_ind.rq_signal.rq_jid)) != NULL) {
			(void)sprintf(log_buffer, "rerun signal reject by mom: %d",
				preq->rq_reply.brp_code);
			log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO,
				preq->rq_ind.rq_signal.rq_jid, log_buffer);

			if ((preq->rq_reply.brp_code == PBSE_UNKJOBID) &&
				(preq->rq_extra == 0)) {
				pjob->ji_qs.ji_substate = JOB_SUBSTATE_RERUN3;
				discard_job(pjob, "Force rerun", 1);
				force_reque(pjob);
			}
		}
	}
	release_req(pwt);
	return;
}

/**
 * @brief
 * 		force_reque - requeue (rerun) a job
 *
 * @param[in,out]	pwt	-	job which needs to be rerun
 */
void
force_reque(job *pjob)
{
	int  newstate;
	int  newsubstate;

	pjob->ji_modified = 1;
	pjob->ji_momhandle = -1;
	pjob->ji_mom_prot = PROT_INVALID;

	if ((pjob->ji_wattr[(int) JOB_ATR_resc_released].at_flags & ATR_VFLAG_SET)) {
		/* If JOB_ATR_resc_released attribute is set and we are trying to rerun a job
		 * then we need to reassign resources first because
		 * when we suspend a job we don't decrement all the resources.
		 * So we need to set partially released resources
		 * back again to release all other resources
		 */
		set_resc_assigned(pjob, 0, INCR);
		job_attr_def[(int) JOB_ATR_resc_released].at_free(
			&pjob->ji_wattr[(int) JOB_ATR_resc_released]);
		pjob->ji_wattr[(int) JOB_ATR_resc_released].at_flags &= ~ATR_VFLAG_SET;
		if (pjob->ji_wattr[(int) JOB_ATR_resc_released_list].at_flags & ATR_VFLAG_SET) {
			job_attr_def[(int) JOB_ATR_resc_released_list].at_free(
				&pjob->ji_wattr[(int) JOB_ATR_resc_released_list]);
			pjob->ji_wattr[(int) JOB_ATR_resc_released_list].at_flags &= ~ATR_VFLAG_SET;
		}

	}

	/* simulate rerun: free nodes, clear checkpoint flag, and */
	/* clear exec_vnode string				  */

	rel_resc(pjob);

	/* note in accounting file */
	account_jobend(pjob, pjob->ji_acctrec, PBS_ACCT_RERUN);

	/* if a subjob,  we set substate to RERUN3 to cause trktbl entry */
	/* to be reset to Qeued, and then blow away the job struct       */

	if (pjob->ji_qs.ji_svrflags & JOB_SVFLG_SubJob) {
		pjob->ji_qs.ji_substate = JOB_SUBSTATE_RERUN3;
		job_purge(pjob);
		return;
	}

	/*
	 * Clear any JOB_SVFLG_Actsuspd flag too, as the job is no longer
	 * suspended (User busy).  A suspended job is rerun in case of a
	 * MOM failure after the workstation becomes active(busy).
	 */
	pjob->ji_qs.ji_svrflags &= ~(JOB_SVFLG_Actsuspd | JOB_SVFLG_StagedIn | JOB_SVFLG_CHKPT);
	job_attr_def[(int)JOB_ATR_exec_host].at_free(
		&pjob->ji_wattr[(int)JOB_ATR_exec_host]);
	job_attr_def[(int)JOB_ATR_exec_host2].at_free(
		&pjob->ji_wattr[(int)JOB_ATR_exec_host2]);
	job_attr_def[(int)JOB_ATR_exec_vnode].at_free(
		&pjob->ji_wattr[(int)JOB_ATR_exec_vnode]);
	job_attr_def[(int)JOB_ATR_pset].at_free(
		&pjob->ji_wattr[(int)JOB_ATR_pset]);
	/* job dir has no meaning for re-queued jobs, so unset it */
	job_attr_def[(int)JOB_ATR_jobdir].at_free(&pjob->
		ji_wattr[(int)JOB_ATR_jobdir]);
	svr_evaljobstate(pjob, &newstate, &newsubstate, 1);
	(void)svr_setjobstate(pjob, newstate, newsubstate);
}

/**
 * @brief
 * 		req_rerunjob - service the Rerun Job Request
 *
 *		This request Reruns a job by:
 *		sending to MOM a signal job request with SIGKILL
 *		marking the job as being rerun by setting the substate.
 *
 *  @param[in,out]	preq	-	Job Request
 */

void
req_rerunjob(struct batch_request *preq)
{
	int		  anygood = 0;
	int		  i;
	int		  j;
	char		 *jid;
	int		  jt;		/* job type */
	int		  offset;
	char		 *pc;
	job		 *pjob;
	job		 *parent;
	char		 *range;
	char		 *vrange;
	int		  x, y, z;

	jid = preq->rq_ind.rq_signal.rq_jid;
	parent = chk_job_request(jid, preq, &jt);
	if (parent == NULL)
		return;		/* note, req_reject already called */

	if ((preq->rq_perm & (ATR_DFLAG_MGWR | ATR_DFLAG_OPWR)) == 0) {
		req_reject(PBSE_PERM, 0, preq);
		return;
	}

	if (jt == IS_ARRAY_NO) {

		/* just a regular job, pass it on down the line and be done */

		req_rerunjob2(preq, parent);
		return;

	} else if (jt == IS_ARRAY_Single) {

		/* single subjob, if running can signal */

		offset = subjob_index_to_offset(parent, get_index_from_jid(jid));
		if (offset == -1) {
			req_reject(PBSE_UNKJOBID, 0, preq);
			return;
		}
		i = get_subjob_state(parent, offset);
		if (i == -1) {
			req_reject(PBSE_IVALREQ, 0, preq);
			return;
		} else if (i == JOB_STATE_RUNNING) {
			if ((pjob = parent->ji_ajtrk->tkm_tbl[offset].trk_psubjob)) {
				req_rerunjob2(preq, pjob);
			} else {
				req_reject(PBSE_BADSTATE, 0, preq);
				return;
			}
		} else {
			req_reject(PBSE_BADSTATE, 0, preq);
			return;
		}
		return;

	} else if (jt == IS_ARRAY_ArrayJob) {

		/* The Array Job itself ... */

		if (parent->ji_qs.ji_state != JOB_STATE_BEGUN) {
			req_reject(PBSE_BADSTATE, 0, preq);
			return;
		}

		/* for each subjob that is running, call req_rerunjob2 */

		++preq->rq_refct;	/* protect the request/reply struct */

		/* Setting deleted subjobs count to 0,
		 * since all the deleted subjobs will be moved to Q state
		 */
		parent->ji_ajtrk->tkm_dsubjsct = 0;

		for (i=0; i<parent->ji_ajtrk->tkm_ct; i++) {
			if ((pjob = parent->ji_ajtrk->tkm_tbl[i].trk_psubjob)) {
				if (pjob->ji_qs.ji_state == JOB_STATE_RUNNING)
					dup_br_for_subjob(preq, pjob, req_rerunjob2);
				else
					force_reque(pjob);
			} else {
				set_subjob_tblstate(parent, i, JOB_STATE_QUEUED);
			}
		}
		/* if not waiting on any running subjobs, can reply; else */
		/* it is taken care of when last running subjob responds  */
		if (--preq->rq_refct == 0)
			reply_send(preq);
		return;

	}
	/* what's left to handle is a range of subjobs, foreach subjob 	*/
	/* if running, all req_rerunjob2			        */

	range = get_index_from_jid(jid);
	if (range == NULL) {
		req_reject(PBSE_IVALREQ, 0, preq);
		return;
	}

	/* first check that all in the subrange are in fact running */

	vrange = range;
	while (1) {
		if ((i = parse_subjob_index(vrange, &pc, &x, &y, &z, &j)) == -1) {
			req_reject(PBSE_IVALREQ, 0, preq);
			return;
		} else if (i == 1)
			break;
		while (x <= y) {
			i = numindex_to_offset(parent, x);
			if (i >= 0) {
				if (get_subjob_state(parent, i) == JOB_STATE_RUNNING)
					anygood++;
			}
			x += z;
		}
		vrange = pc;
	}
	if (anygood == 0) {
		req_reject(PBSE_BADSTATE, 0, preq);
		return;
	}

	/* now do the deed */

	++preq->rq_refct;	/* protect the request/reply struct */

	while (1) {
		if ((i = parse_subjob_index(range, &pc, &x, &y, &z, &j)) == -1) {
			req_reject(PBSE_IVALREQ, 0, preq);
			break;
		} else if (i == 1)
			break;
		while (x <= y) {
			i = numindex_to_offset(parent, x);
			if (i < 0) {
				x += z;
				continue;
			}

			if (get_subjob_state(parent, i) == JOB_STATE_RUNNING) {
				if ((pjob = parent->ji_ajtrk->tkm_tbl[i].trk_psubjob)) {
					dup_br_for_subjob(preq, pjob, req_rerunjob2);
				}
			}
			x += z;
		}
		range = pc;
	}

	/* if not waiting on any running subjobs, can reply; else */
	/* it is taken care of when last running subjob responds  */
	if (--preq->rq_refct == 0)
		reply_send(preq);
	return;
}

/**
 * @brief
 * 		Function that causes a rerun request to return with a timeout message.
 *
 * @param[in,out]	pwt	-	work task which contains the job structure which holds the rerun request
 */
static void
timeout_rerun_request(struct work_task *pwt)
{
	job *pjob = (job *) pwt->wt_parm1;
	conn_t *conn = NULL;

	if ((pjob == NULL) || (pjob->ji_rerun_preq == NULL)) {
		return; /* nothing to timeout */
	}
	if (pjob->ji_rerun_preq->rq_conn != PBS_LOCAL_CONNECTION) {
		conn = get_conn(pjob->ji_rerun_preq->rq_conn);
	}
	reply_text(pjob->ji_rerun_preq, PBSE_INTERNAL,
			"Response timed out. Job rerun request still in progress for");

	/* clear no-timeout flag on connection */
	if (conn)
		conn->cn_authen &= ~PBS_NET_CONN_NOTIMEOUT;

	pjob->ji_rerun_preq = NULL;

}
/**
 * @brief
 * 		req_rerunjob - service the Rerun Job Request
 *
 *  @param[in,out]	preq	-	Job Request
 *  @param[in,out]	pjob	-	ptr to the subjob
 */
static void
req_rerunjob2(struct batch_request *preq, job *pjob)
{
	long	force = 0;
	struct  work_task *ptask;
	time_t  rerun_to;
	conn_t	*conn;
	int rc;
	int is_mgr = 0;
	void *force_rerun = NULL;

	if (preq->rq_extend && (strcmp(preq->rq_extend, "force") == 0))
		force = 1;

	/* See if the request is coming from a manager */
	if (preq->rq_perm & (ATR_DFLAG_MGRD | ATR_DFLAG_MGWR))
		is_mgr = 1;

	/* the job must be rerunnable or force must be on */

	if ((pjob->ji_wattr[(int)JOB_ATR_rerunable].at_val.at_long == 0) &&
		(force == 0)) {
		req_reject(PBSE_NORERUN, 0, preq);
		return;
	}

	/* the job must be running */

	if (pjob->ji_qs.ji_state != JOB_STATE_RUNNING) {
		req_reject(PBSE_BADSTATE, 0, preq);
		return;
	}
	/* a node failure tolerant job could be waiting for healthy nodes
         * and it would have a JOB_SUBSTATE_PRERUN substate.
         */
	if ((pjob->ji_qs.ji_substate != JOB_SUBSTATE_RUNNING) &&
            (pjob->ji_qs.ji_substate != JOB_SUBSTATE_PRERUN) && (force == 0)) {
		req_reject(PBSE_BADSTATE, 0, preq);
		return;
	}

	/* Set the flag for post_rerun only when
	 * force is set and request is from manager */
	if (force == 1 && is_mgr == 1)
		force_rerun = (void *)1;

	/* ask MOM to kill off the job */

	rc = issue_signal(pjob, SIG_RERUN, post_rerun, force_rerun);

	/*
	 * If force is set and request is from a PBS manager,
	 * job is re-queued regardless of issue_signal to MoM
	 * was a success or failure.
	 * Eventually, when the mom updates server about the job,
	 * server sends a discard message to mom and job is
	 * then deleted from mom as well.
	 */
	if ((rc || is_mgr) && force == 1) {

		/* Mom is down and issue signal failed or
		 * request is from a manager and "force" is on,
		 * force the requeue */

		pjob->ji_qs.ji_substate = JOB_SUBSTATE_RERUN3;
		discard_job(pjob, "Force rerun", 1);
		force_reque(pjob);
		reply_ack(preq);
		return;

	}

	if (rc != 0) {
		req_reject(rc, 0, preq);
		return;
	}

	/* So job has run and is to be rerun (not restarted) */

	pjob->ji_qs.ji_svrflags = (pjob->ji_qs.ji_svrflags &
		~(JOB_SVFLG_CHKPT | JOB_SVFLG_ChkptMig)) |
	JOB_SVFLG_HASRUN;
	svr_setjobstate(pjob, JOB_STATE_RUNNING, JOB_SUBSTATE_RERUN);

	(void)sprintf(log_buffer, msg_manager, msg_jobrerun,
		preq->rq_user, preq->rq_host);
	log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO,
		pjob->ji_qs.ji_jobid, log_buffer);

	/* The following means we've detected an outstanding rerun request  */
	/* for the same job which should not happen. But if it does, let's  */
	/* ack that previous request to also free up its request structure. */
	if (pjob->ji_rerun_preq != NULL) {
		reply_ack(pjob->ji_rerun_preq);
	}
	pjob->ji_rerun_preq = preq;

	/* put a timeout on the rerun request so that it doesn't hang 	*/
	/* indefinitely; if it does, the scheduler would also hang on a */
	/* requeue request  */
	time_now = time(NULL);
	if ((server.sv_attr[(int)SRV_ATR_JobRequeTimeout].at_flags & ATR_VFLAG_SET) == 0) {
		rerun_to = time_now + PBS_DIS_TCP_TIMEOUT_RERUN;
	} else {
		rerun_to = time_now + server.sv_attr[(int)SRV_ATR_JobRequeTimeout].at_val.at_long;
	}
	ptask = set_task(WORK_Timed, rerun_to, timeout_rerun_request, pjob);
	if (ptask) {
		/* this ensures that the ptask created gets cleared in case */
		/* pjob gets deleted before the task is served */
		append_link(&pjob->ji_svrtask, &ptask->wt_linkobj, ptask);
	}

	/* set no-timeout flag on connection to client */
	if (preq->rq_conn != PBS_LOCAL_CONNECTION) {
		conn = get_conn(preq->rq_conn);
		if (conn)
			conn->cn_authen |= PBS_NET_CONN_NOTIMEOUT;
	}

}
