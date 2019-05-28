/*
 * config_log.c
 *
 * PostgreSQL extension with custom background worker to monitor
 * and record changes to postgresql.conf (experimental)
 *
 * NOTES:
 * - custom background workers are available from PostgreSQL 9.3
 * - the background worker API is not fully finalized and
 *   may change before final release (see also note in '_PG_init()')
 *
 * Written by Ian Barwick
 * barwick@gmail.com
 *
 * Copyright 2013 - 2019 Ian Barwick. This program is Free
 * Software; see the README.md file for the license conditions.
 */

#include "postgres.h"

/* Following are required for all bgworker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"

/* these headers are used by this particular worker's code */
#include "access/xact.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "pgstat.h"
#include "utils/snapmgr.h"
#include "tcop/utility.h"

PG_MODULE_MAGIC;

void _PG_init(void);

#if PG_VERSION_NUM >= 100000
void		config_log_main(Datum) pg_attribute_noreturn();
#endif

/* flags set by signal handlers */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/* GUC variables */
static char * config_log_database = NULL;
static char * config_log_schema   = NULL;


typedef struct config_log_objects
{
	const char	   *table_name;
	const char	   *function_name;
} config_log_objects;

static config_log_objects *initialize_objects(void);
static void execute_pg_settings_logger(config_log_objects *objects);
static void log_info(char *msg);

static void
config_log_sigterm(SIGNAL_ARGS)
{
	int	save_errno = errno;

	got_sigterm = true;

#if PG_VERSION_NUM >= 90500
	SetLatch(MyLatch);
#else
	if (MyProc)
		SetLatch(&MyProc->procLatch);
#endif

	errno = save_errno;
}

static void
config_log_sighup(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sighup = true;

	log_info("received sighup");

#if PG_VERSION_NUM >= 90500
	SetLatch(MyLatch);
#else
	if (MyProc)
		SetLatch(&MyProc->procLatch);
#endif

	errno = save_errno;
}

static void
log_info(char *msg) {
	ereport(LOG,
			(errmsg("%s: %s",
					MyBgworkerEntry->bgw_name,
					msg)));
}



/*
 * Initialize objects
 *
 */

static config_log_objects *
initialize_objects(void)
{
    config_log_objects *objects;

	int		ret;
	int		ntup;
	bool	isnull;
	StringInfoData	buf;

	objects = palloc(sizeof(config_log_objects));

	objects->table_name = pstrdup("pg_settings_log");
	objects->function_name = pstrdup("pg_settings_logger");

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());
	pgstat_report_activity(STATE_RUNNING, "Verifying config log objects");

	initStringInfo(&buf);
	appendStringInfo(
		&buf,
		"SELECT COUNT(*)\
           FROM information_schema.tables\
          WHERE table_schema='%s'\
            AND table_name ='%s'\
            AND table_type='BASE TABLE'",
		config_log_schema,
		objects->table_name);

	ret = SPI_execute(buf.data, true, 0);

	if (ret != SPI_OK_SELECT)
	{
         ereport(FATAL,
				 (errmsg("SPI_execute failed: SPI error code %d", ret)));
	}

    /* This should never happen */
	if (SPI_processed != 1)
	{
		elog(FATAL, "not a singleton result");
	}

	ntup = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
					   SPI_tuptable->tupdesc,
					   1, &isnull));

    /* This should never happen */
	if (isnull)
	{
		elog(FATAL, "null result");
	}

	if (ntup == 0)
	{
        ereport(FATAL,
				(errmsg("expected config log table '%s.%s' not found", config_log_schema,
						objects->table_name),
				 errhint("ensure superuser search_path includes the schema used by config_log; "
						 "check config_log.* GUC settings")));
	}

	/* check function pg_settings_logger() exists */
	resetStringInfo(&buf);

	appendStringInfo(
		&buf,
		"SELECT COUNT(*) FROM pg_catalog.pg_proc p \
     INNER JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace \
          WHERE p.proname='%s' \
            AND n.nspname='%s' \
            AND p.pronargs = 0",
		objects->function_name,
		config_log_schema);

	ret = SPI_execute(buf.data, true, 0);
	pfree(buf.data);

	if (ret != SPI_OK_SELECT)
	{
         ereport(FATAL,
				 (errmsg("SPI_execute failed: SPI error code %d", ret)));
	}

	if (SPI_processed != 1)
	{
		elog(FATAL, "not a singleton result");
	}

	ntup = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
									   SPI_tuptable->tupdesc,
									   1, &isnull));
	if (isnull)
	{
		elog(FATAL, "null result");
	}

	if (ntup == 0)
	{
        ereport(FATAL,
				(errmsg("expected config log function '%s.%s' not found", config_log_schema,
						objects->function_name),
				 errhint("ensure superuser search_path includes the schema used by config_log; "
						 "check config_log.* GUC settings")));
	}

   	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();
	pgstat_report_activity(STATE_IDLE, NULL);

	log_info("initialized, database objects validated");

	/* execute pg_settings_logger() here to catch any settings which have changed after server restart */
	execute_pg_settings_logger(objects);

	return objects;
}


static void
execute_pg_settings_logger(config_log_objects *objects) {

	int		ret;
	bool	isnull;
	StringInfoData	buf;


	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());
	pgstat_report_activity(STATE_RUNNING, "executing configuration logger function");

	initStringInfo(&buf);

	appendStringInfo(
		&buf,
		"SELECT %s.%s()",
		config_log_schema,
		objects->function_name);

	ret = SPI_execute(buf.data, false, 0);

	pfree(buf.data);

	if (ret != SPI_OK_SELECT)
	{
		elog(FATAL, "SPI_execute failed: error code %d", ret);
	}

	if (SPI_processed != 1)
	{
		elog(FATAL, "not a singleton result");
	}

	log_info("pg_settings_logger() executed");

	if (DatumGetBool(SPI_getbinval(SPI_tuptable->vals[0],
								   SPI_tuptable->tupdesc,
								   1, &isnull)))
	{
		log_info("configuration changes recorded");
	}
	else
	{
		log_info("no configuration changes detected");
	}

   	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();
	pgstat_report_activity(STATE_IDLE, NULL);
}


#if PG_VERSION_NUM >= 100000
void
#else
static void
#endif
config_log_main(Datum main_arg)
{
	config_log_objects	   *objects;

	pqsignal(SIGTERM, config_log_sigterm);
	pqsignal(SIGHUP,  config_log_sighup);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to database */
#if PG_VERSION_NUM >= 110000
	BackgroundWorkerInitializeConnection(config_log_database, NULL, 0);
#else
	BackgroundWorkerInitializeConnection(config_log_database, NULL);
#endif

	/* Verify expected objects exist */
	objects = initialize_objects();

	while (!got_sigterm)
	{
		int		rc;

#if (PG_VERSION_NUM >= 100000)
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   100000L,
					   PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
#elif (PG_VERSION_NUM >= 90500)
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   100000L);
		ResetLatch(MyLatch);
#else
		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   100000L);
		ResetLatch(&MyProc->procLatch);
#endif


		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		/*
		 * In case of a SIGHUP, just reload the configuration.
		 */
		if (got_sighup)
		{
              got_sighup = false;
              ProcessConfigFile(PGC_SIGHUP);
              execute_pg_settings_logger(objects);
		}
    }

	proc_exit(0);
}


/*
 * Entrypoint of this module.
 */
void
_PG_init(void)
{
	BackgroundWorker	worker;

	/* get GUC settings, if available */

	DefineCustomStringVariable(
		"config_log.database",
		"Database used for config_log",
		"Database used to store config_log records (default: postgres).",
		&config_log_database,
		"postgres",
		PGC_POSTMASTER,
		0,
		NULL,
		NULL,
		NULL
    );

	DefineCustomStringVariable(
		"config_log.schema",
		"Schema used for config_log",
		"Schema used to store config_log records (default: public).",
		&config_log_schema,
		"public",
		PGC_POSTMASTER,
		0,
		NULL,
		NULL,
		NULL
    );

	/*
	 * Register the worker processes.
	 *
	 * For details see: https://www.postgresql.org/docs/current/bgworker.html
	 *
	 * Note that the API has changed over time.
	 */
	memset(&worker, 0, sizeof(worker));

	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = 1;


#if (PG_VERSION_NUM >= 100000)
	sprintf(worker.bgw_library_name, "config_log");
	sprintf(worker.bgw_function_name, "config_log_main");

	worker.bgw_main_arg = (Datum) 0;

	/* these values are shown in the process list */
	snprintf(worker.bgw_name, BGW_MAXLEN, "config_log worker");

#if (PG_VERSION_NUM >= 110000)
	snprintf(worker.bgw_type, BGW_MAXLEN, "config_log");
#endif

#else
	/* this value is shown in the process list */
	snprintf(worker.bgw_name, BGW_MAXLEN, "config_log");

	worker.bgw_main = config_log_main;
	worker.bgw_main_arg = (Datum) 0;
#endif

#if PG_VERSION_NUM >= 90400
	worker.bgw_notify_pid = 0;
#endif

	RegisterBackgroundWorker(&worker);
}
