/*
 * config_log.c
 *
 * PostgreSQL extension with custom background worker to monitor 
 * and record changes to postgresql.conf (experimental)
 *
 * TODO:
 *  - replace elog() with ereport()?
 *
 * Written by Ian Barwick
 * barwick@gmail.com
 *
 * Copyright 2013 Ian Barwick. This program is Free
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


/* flags set by signal handlers */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

typedef struct config_log_objects
{
	const char	   *schema;
	const char	   *table_name;
	const char	   *function_name;
} config_log_objects;

config_log_objects *initialize_objects(void);
static void execute_pg_settings_logger(config_log_objects *objects);
static void log_info(char *msg);

static void
config_log_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sigterm = true;
	if (MyProc) 
	{
		SetLatch(&MyProc->procLatch);
	}

	errno = save_errno;
}

static void
config_log_sighup(SIGNAL_ARGS)
{
	log_info("received sighup");
	got_sighup = true;

	if (MyProc)
	{
		SetLatch(&MyProc->procLatch);
	}
}

static void
log_info(char *msg) {
	elog(LOG, "%s: %s",
		 MyBgworkerEntry->bgw_name,
		 msg
		);
}



/*
 * Initialize objects
 *
 */
 
config_log_objects *
initialize_objects(void)
{
    config_log_objects *objects;

	int		ret;
	int		ntup;
	bool	isnull;
	StringInfoData	buf;

	objects = palloc(sizeof(config_log_objects));

	/* TODO : make schema/object names configurable */
	objects->schema = pstrdup("public");
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
		"SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='%s' AND table_name ='%s'",
		objects->schema,
		objects->table_name 
		);

	ret = SPI_execute(buf.data, true, 0);
	if (ret != SPI_OK_SELECT) 
	{
		elog(FATAL, "SPI_execute failed: error code %d", ret);
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
		elog(FATAL, "Expected config log table '%s.%s' not found", objects->schema,
			 objects->table_name );
	}

	/* check unction pg_settings_logger() exists */

	resetStringInfo(&buf);

	appendStringInfo(
		&buf, 
		"SELECT COUNT(*) FROM pg_catalog.pg_proc p \
     INNER JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace \
          WHERE p.proname='%s' \
            AND n.nspname='%s' \
            AND p.pronargs = 0",
		objects->function_name,
		objects->schema
		);

	ret = SPI_execute(buf.data, true, 0);
	if (ret != SPI_OK_SELECT) 
	{
		elog(FATAL, "SPI_execute failed: error code %d", ret);
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
		elog(FATAL, "Expected config log function '%s.%s' not found",
			 objects->schema,
			 objects->function_name );
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
		objects->schema,
		objects->function_name
		);

	ret = SPI_execute(buf.data, false, 0);
	if (ret != SPI_OK_SELECT)
	{
		elog(FATAL, "SPI_execute failed: error code %d", ret);
	}

	if (SPI_processed != 1)
	{
		elog(FATAL, "not a singleton result");
	}

	log_info("pg_settings_logger() executed"); 
	
	if(DatumGetBool(SPI_getbinval(SPI_tuptable->vals[0],
				  SPI_tuptable->tupdesc,
				  1, &isnull))) 
	{
		log_info("Configuration changes recorded");
	}
	else 
	{
		log_info("No configuration changes detected");
	}

   	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();
	pgstat_report_activity(STATE_IDLE, NULL);
}


static void
config_log_main(void *main_arg)
{
	config_log_objects	   *objects;

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to database */
	/* TODO: make this configurable */
	BackgroundWorkerInitializeConnection("postgres", NULL);

	/* Verify expected objects exist */
	objects = initialize_objects();

	while (!got_sigterm)
	{
		int		rc;

		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   100000L);
		ResetLatch(&MyProc->procLatch);

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

	/* register the worker processes */
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_main = config_log_main;
	worker.bgw_sighup = config_log_sighup;
	worker.bgw_sigterm = config_log_sigterm;
	/* this value is shown in the process list */
	worker.bgw_name = "config_log";
	worker.bgw_restart_time = 1;
	worker.bgw_main_arg = NULL;

	RegisterBackgroundWorker(&worker);
}

