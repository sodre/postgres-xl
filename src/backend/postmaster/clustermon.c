/*-------------------------------------------------------------------------
 *
 * clustermon.c
 *
 * Postgres-XL Cluster Monitor
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Portions Copyright (c) 2015, 2ndQuadrant Ltd
 * Portions Copyright (c) 2012-2014, TransLattice, Inc.
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/clustermon.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

#include "access/transam.h"
#include "access/xact.h"
#include "gtm/gtm_c.h"
#include "gtm/gtm_gxid.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgxc/pgxc.h"
#include "postmaster/clustermon.h"
#include "postmaster/fork_process.h"
#include "postmaster/postmaster.h"
#include "storage/proc.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"

/* Flags to tell if we are in a clustermon process */
static bool am_clustermon = false;

/* Flags set by signal handlers */
static volatile sig_atomic_t got_SIGHUP = false;
static volatile sig_atomic_t got_SIGUSR2 = false;
static volatile sig_atomic_t got_SIGTERM = false;

/* Memory context for long-lived data */
static MemoryContext ClusterMonitorMemCxt;
static ClusterMonitorCtlData *ClusterMonitorCtl = NULL; 

static void cm_sighup_handler(SIGNAL_ARGS);
static void cm_sigterm_handler(SIGNAL_ARGS);
static void ClusterMonitorSetReportedGlobalXmin(GlobalTransactionId xmin);
static GlobalTransactionId ClusterMonitorGetReportedGlobalXmin(void);

/* PID of clustser monitoring process */
int			ClusterMonitorPid = 0;

#define CLUSTER_MONITOR_NAPTIME	5

/*
 * Main loop for the cluster monitor process.
 */
int
ClusterMonitorInit(void)
{
	sigjmp_buf	local_sigjmp_buf;
	GTM_PGXCNodeType nodetype = IS_PGXC_DATANODE ?
									GTM_NODE_DATANODE :
									GTM_NODE_COORDINATOR;
	GlobalTransactionId oldestXmin;
	GlobalTransactionId newOldestXmin;
	GlobalTransactionId reportedXmin;
	GlobalTransactionId lastGlobalXmin;
	int status;

	am_clustermon = true;

	/* Identify myself via ps */
	init_ps_display("cluster monitor process", "", "", "");

	ereport(LOG,
			(errmsg("cluster monitor started")));

	if (PostAuthDelay)
		pg_usleep(PostAuthDelay * 1000000L);

	/*
	 * Set up signal handlers.  We operate on databases much like a regular
	 * backend, so we use the same signal handling.  See equivalent code in
	 * tcop/postgres.c.
	 */
	pqsignal(SIGHUP, cm_sighup_handler);
	pqsignal(SIGINT, StatementCancelHandler);
	pqsignal(SIGTERM, cm_sigterm_handler);

	pqsignal(SIGQUIT, quickdie);
	InitializeTimeouts();		/* establishes SIGALRM handler */

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.
	 */
	ClusterMonitorMemCxt = AllocSetContextCreate(TopMemoryContext,
										  "Cluster Monitor",
										  ALLOCSET_DEFAULT_MINSIZE,
										  ALLOCSET_DEFAULT_INITSIZE,
										  ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContextSwitchTo(ClusterMonitorMemCxt);

    SetProcessingMode(NormalProcessing);

	/*
	 * Register this node with the GTM
	 */
	oldestXmin = InvalidGlobalTransactionId;
	if (RegisterGTM(nodetype, &oldestXmin) < 0)
	{
		UnregisterGTM(nodetype);
		oldestXmin = InvalidGlobalTransactionId;
		if (RegisterGTM(nodetype, &oldestXmin) < 0)
		{
			ereport(LOG,
					(errcode(ERRCODE_IO_ERROR),
					 errmsg("Can not register node on GTM")));
		}
	}

	/*
	 * If the registration is successful, GTM would send us back current
	 * GlobalXmin. Initialise our local state to the same value
	 */
	ClusterMonitorSetReportedGlobalXmin(oldestXmin);
	
	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * This code is a stripped down version of PostgresMain error recovery.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Forget any pending QueryCancel or timeout request */
		disable_all_timeouts(false);
		QueryCancelPending = false;		/* second to avoid race condition */

		/* Report the error to the server log */
		EmitErrorReport();

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 */
		MemoryContextSwitchTo(ClusterMonitorMemCxt);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		MemoryContextResetAndDeleteChildren(ClusterMonitorMemCxt);

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();

		/* if in shutdown mode, no need for anything further; just go away */
		if (got_SIGTERM)
			goto shutdown;

		/*
		 * Sleep at least 1 second after any error.  We don't want to be
		 * filling the error logs as fast as we can.
		 */
		pg_usleep(1000000L);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/* must unblock signals before calling rebuild_database_list */
	PG_SETMASK(&UnBlockSig);

	/*
	 * Force statement_timeout and lock_timeout to zero to avoid letting these
	 * settings prevent regular maintenance from being executed.
	 */
	SetConfigOption("statement_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("lock_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);

	/* loop until shutdown request */
	while (!got_SIGTERM)
	{
		struct timeval nap;
		int			rc;
		bool	isIdle;

		/*
		 * Compute RecentGlobalXmin, report it to the GTM and sleep for the set
		 * interval. Keep doing this forever
		 */
		isIdle = false;
		reportedXmin = ClusterMonitorGetReportedGlobalXmin();
		lastGlobalXmin = ClusterMonitorGetGlobalXmin();
		oldestXmin = GetOldestXminInternal(NULL, false, true, &isIdle,
				lastGlobalXmin, reportedXmin);

		if (GlobalTransactionIdPrecedes(oldestXmin, reportedXmin))
			oldestXmin = reportedXmin;

		if (GlobalTransactionIdPrecedes(oldestXmin, lastGlobalXmin))
			oldestXmin = lastGlobalXmin;

		if ((status = ReportGlobalXmin(&oldestXmin, &newOldestXmin, isIdle)))
		{
			elog(DEBUG2, "Failed to report RecentGlobalXmin to GTM - %d:%d",
					status, newOldestXmin);
			if (status == GTM_ERRCODE_TOO_OLD_XMIN ||
				status == GTM_ERRCODE_NODE_EXCLUDED)
				elog(PANIC, "Global xmin computation mismatch");
		}
		else
		{
			ClusterMonitorSetReportedGlobalXmin(oldestXmin);
			elog(DEBUG2, "Updating global_xmin to %d", newOldestXmin);
			if (GlobalTransactionIdIsValid(newOldestXmin))
				ClusterMonitorSetGlobalXmin(newOldestXmin);
		}

		/*
		 * Repeat at every 30 seconds
		 */
		nap.tv_sec = CLUSTER_MONITOR_NAPTIME;
		nap.tv_usec = 0;

		/*
		 * Wait until naptime expires or we get some type of signal (all the
		 * signal handlers will wake us by calling SetLatch).
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   (nap.tv_sec * 1000L) + (nap.tv_usec / 1000L));

		ResetLatch(MyLatch);

		/* Process sinval catchup interrupts that happened while sleeping */
		ProcessCatchupInterrupt();

		/*
		 * Emergency bailout if postmaster has died.  This is to avoid the
		 * necessity for manual cleanup of all postmaster children.
		 */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		/* the normal shutdown case */
		if (got_SIGTERM)
			break;

		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
	}

	/* Normal exit from the cluster monitor is here */
shutdown:
	UnregisterGTM(nodetype);
	ereport(LOG,
			(errmsg("cluster monitor shutting down")));

	proc_exit(0);				/* done */
}

/* SIGHUP: set flag to re-read config file at next convenient time */
static void
cm_sighup_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_SIGHUP = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/* SIGTERM: time to die */
static void
cm_sigterm_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_SIGTERM = true;
	SetLatch(MyLatch);

	errno = save_errno;
}


/*
 * IsClusterMonitor functions
 *		Return whether this is either a cluster monitor process or a worker
 *		process.
 */
bool
IsClusterMonitorProcess(void)
{
	return am_clustermon;
}

/* Report shared-memory space needed by ClusterMonitor */
Size
ClusterMonitorShmemSize(void)
{
	return sizeof (ClusterMonitorCtlData);
}

void
ClusterMonitorShmemInit(void)
{
	bool		found;

	ClusterMonitorCtl = (ClusterMonitorCtlData *)
		ShmemInitStruct("Cluster Monitor Ctl", ClusterMonitorShmemSize(), &found);

	if (!found)
	{
		/* First time through, so initialize */
		MemSet(ClusterMonitorCtl, 0, ClusterMonitorShmemSize());
		SpinLockInit(&ClusterMonitorCtl->mutex);
	}
}

GlobalTransactionId
ClusterMonitorGetGlobalXmin(void)
{
	GlobalTransactionId xmin;

	SpinLockAcquire(&ClusterMonitorCtl->mutex);
	xmin = ClusterMonitorCtl->gtm_recent_global_xmin;
	SpinLockRelease(&ClusterMonitorCtl->mutex);

	return xmin;
}

void
ClusterMonitorSetGlobalXmin(GlobalTransactionId xmin)
{
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	ProcArrayCheckXminConsistency(xmin);

	SpinLockAcquire(&ClusterMonitorCtl->mutex);
	ClusterMonitorCtl->gtm_recent_global_xmin = xmin;
	SpinLockRelease(&ClusterMonitorCtl->mutex);

	LWLockRelease(ProcArrayLock);
}

static void
ClusterMonitorSetReportedGlobalXmin(GlobalTransactionId xmin)
{
	elog(DEBUG2, "ClusterMonitorSetReportedGlobalXmin - old %d, new %d",
			ClusterMonitorCtl->reported_recent_global_xmin,
			xmin);
	SpinLockAcquire(&ClusterMonitorCtl->mutex);
	ClusterMonitorCtl->reported_recent_global_xmin = xmin;
	SpinLockRelease(&ClusterMonitorCtl->mutex);
}

static GlobalTransactionId
ClusterMonitorGetReportedGlobalXmin(void)
{
	GlobalTransactionId reported_xmin;

	SpinLockAcquire(&ClusterMonitorCtl->mutex);
	reported_xmin = ClusterMonitorCtl->reported_recent_global_xmin;
	SpinLockRelease(&ClusterMonitorCtl->mutex);

	return reported_xmin;
}
