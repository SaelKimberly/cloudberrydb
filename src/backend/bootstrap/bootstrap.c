/*-------------------------------------------------------------------------
 *
 * bootstrap.c
 *	  routines to support running postgres in 'bootstrap' mode
 *	bootstrap mode is used to create the initial template database
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/bootstrap/bootstrap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <signal.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/toast_compression.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "bootstrap/bootstrap.h"
#include "catalog/index.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "catalog/storage_tablespace.h"
#include "commands/tablespace.h"
#include "common/link-canary.h"
#include "crypto/bufenc.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pg_getopt.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "postmaster/postmaster.h" /* TODO: verify we need this still */
#include "postmaster/startup.h"
#include "postmaster/walwriter.h"
#include "replication/walreceiver.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/rel.h"
#include "utils/relmapper.h"

uint32      bootstrap_data_checksum_version = 0;	/* No checksum */
int         bootstrap_file_encryption_method = DISABLED_ENCRYPTION_METHOD;
char        *bootstrap_old_key_datadir = NULL;	/* disabled */


static void CheckerModeMain(void);
static void BootstrapModeMain(void);
static void bootstrap_signals(void);
static void ShutdownAuxiliaryProcess(int code, Datum arg);
static Form_pg_attribute AllocateAttribute(void);
static void populate_typ_list(void);
static Oid	gettype(char *type);
static void cleanup(void);

/* ----------------
 *		global variables
 * ----------------
 */

AuxProcType MyAuxProcType = NotAnAuxProcess;	/* declared in miscadmin.h */

Relation	boot_reldesc;		/* current relation descriptor */

Form_pg_attribute attrtypes[MAXATTR];	/* points to attribute info */
int			numattr;			/* number of attributes for cur. rel */


/*
 * Basic information associated with each type.  This is used before
 * pg_type is filled, so it has to cover the datatypes used as column types
 * in the core "bootstrapped" catalogs.
 *
 *		XXX several of these input/output functions do catalog scans
 *			(e.g., F_REGPROCIN scans pg_proc).  this obviously creates some
 *			order dependencies in the catalog creation process.
 */
struct typinfo
{
	char		name[NAMEDATALEN];
	Oid			oid;
	Oid			elem;
	int16		len;
	bool		byval;
	char		align;
	char		storage;
	Oid			collation;
	Oid			inproc;
	Oid			outproc;
};

static const struct typinfo TypInfo[] = {
	{"bool", BOOLOID, 0, 1, true, TYPALIGN_CHAR, TYPSTORAGE_PLAIN, InvalidOid,
	F_BOOLIN, F_BOOLOUT},
	{"bytea", BYTEAOID, 0, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, InvalidOid,
	F_BYTEAIN, F_BYTEAOUT},
	{"char", CHAROID, 0, 1, true, TYPALIGN_CHAR, TYPSTORAGE_PLAIN, InvalidOid,
	F_CHARIN, F_CHAROUT},
	{"int2", INT2OID, 0, 2, true, TYPALIGN_SHORT, TYPSTORAGE_PLAIN, InvalidOid,
	F_INT2IN, F_INT2OUT},
	{"int4", INT4OID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_INT4IN, F_INT4OUT},
	{"float4", FLOAT4OID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_FLOAT4IN, F_FLOAT4OUT},
	{"name", NAMEOID, CHAROID, NAMEDATALEN, false, TYPALIGN_CHAR, TYPSTORAGE_PLAIN, C_COLLATION_OID,
	F_NAMEIN, F_NAMEOUT},
	{"regclass", REGCLASSOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_REGCLASSIN, F_REGCLASSOUT},
	{"regproc", REGPROCOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_REGPROCIN, F_REGPROCOUT},
	{"regtype", REGTYPEOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_REGTYPEIN, F_REGTYPEOUT},
	{"regrole", REGROLEOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_REGROLEIN, F_REGROLEOUT},
	{"regnamespace", REGNAMESPACEOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_REGNAMESPACEIN, F_REGNAMESPACEOUT},
	{"text", TEXTOID, 0, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, DEFAULT_COLLATION_OID,
	F_TEXTIN, F_TEXTOUT},
	{"oid", OIDOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_OIDIN, F_OIDOUT},
	{"tid", TIDOID, 0, 6, false, TYPALIGN_SHORT, TYPSTORAGE_PLAIN, InvalidOid,
	F_TIDIN, F_TIDOUT},
	{"xid", XIDOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_XIDIN, F_XIDOUT},
	{"cid", CIDOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_CIDIN, F_CIDOUT},
	{"pg_node_tree", PG_NODE_TREEOID, 0, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, DEFAULT_COLLATION_OID,
	F_PG_NODE_TREE_IN, F_PG_NODE_TREE_OUT},
	{"int2vector", INT2VECTOROID, INT2OID, -1, false, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_INT2VECTORIN, F_INT2VECTOROUT},
	{"oidvector", OIDVECTOROID, OIDOID, -1, false, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_OIDVECTORIN, F_OIDVECTOROUT},
	{"_int4", INT4ARRAYOID, INT4OID, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, InvalidOid,
	F_ARRAY_IN, F_ARRAY_OUT},
	{"_text", 1009, TEXTOID, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, DEFAULT_COLLATION_OID,
	F_ARRAY_IN, F_ARRAY_OUT},
	{"_oid", 1028, OIDOID, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, InvalidOid,
	F_ARRAY_IN, F_ARRAY_OUT},
	{"_char", 1002, CHAROID, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, InvalidOid,
	F_ARRAY_IN, F_ARRAY_OUT},
	{"_aclitem", 1034, ACLITEMOID, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, InvalidOid,
	F_ARRAY_IN, F_ARRAY_OUT}
};

static const int n_types = sizeof(TypInfo) / sizeof(struct typinfo);

struct typmap
{								/* a hack */
	Oid			am_oid;
	FormData_pg_type am_typ;
};

static List *Typ = NIL;			/* List of struct typmap* */
static struct typmap *Ap = NULL;

static Datum values[MAXATTR];	/* current row's attribute values */
static bool Nulls[MAXATTR];

static MemoryContext nogc = NULL;	/* special no-gc mem context */

/*
 *	At bootstrap time, we first declare all the indices to be built, and
 *	then build them.  The IndexList structure stores enough information
 *	to allow us to build the indices after they've been declared.
 */

typedef struct _IndexList
{
	Oid			il_heap;
	Oid			il_ind;
	IndexInfo  *il_info;
	struct _IndexList *il_next;
} IndexList;

static IndexList *ILHead = NULL;


/*
 *	 AuxiliaryProcessMain
 *
 *	 The main entry point for auxiliary processes, such as the bgwriter,
 *	 walwriter, walreceiver, bootstrapper and the shared memory checker code.
 *
 *	 This code is here just because of historical reasons.
 */
void
AuxiliaryProcessMain(int argc, char *argv[])
{
	char	   *progname = argv[0];
	int			flag;
	char	   *userDoption = NULL;

	/*
	 * Initialize process environment (already done if under postmaster, but
	 * not if standalone).
	 */
	if (!IsUnderPostmaster)
		InitStandaloneProcess(argv[0]);

	/*
	 * process command arguments
	 */

	/* Set defaults, to be overridden by explicit options below */
	if (!IsUnderPostmaster)
		InitializeGUCOptions();

	/* Ignore the initial --boot argument, if present */
	if (argc > 1 && strcmp(argv[1], "--boot") == 0)
	{
		argv++;
		argc--;
	}

	/* If no -x argument, we are a CheckerProcess */
	MyAuxProcType = CheckerProcess;

 	while ((flag = getopt(argc, argv, "B:c:d:D:FkK:r:x:R:u:X:-:")) != -1)
	{
		switch (flag)
		{
			case 'B':
				SetConfigOption("shared_buffers", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'D':
				userDoption = pstrdup(optarg);
				break;
			case 'd':
				{
					/* Turn on debugging for the bootstrap process. */
					char	   *debugstr;

					debugstr = psprintf("debug%s", optarg);
					SetConfigOption("log_min_messages", debugstr,
									PGC_POSTMASTER, PGC_S_ARGV);
					SetConfigOption("client_min_messages", debugstr,
									PGC_POSTMASTER, PGC_S_ARGV);
					pfree(debugstr);
				}
				break;
			case 'F':
				SetConfigOption("fsync", "false", PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'k':
				bootstrap_data_checksum_version = PG_DATA_CHECKSUM_VERSION;
				break;
 			case 'K':
 				{
 					int i;
 
 					/* method 0/disabled cannot be specified */
 					for (i = DISABLED_ENCRYPTION_METHOD + 1;
 						 i < NUM_ENCRYPTION_METHODS; i++)
 						if (pg_strcasecmp(optarg, encryption_methods[i].name) == 0)
 						{
 							bootstrap_file_encryption_method = i;
 							break;
 						}
 					if (i == NUM_ENCRYPTION_METHODS)
 						ereport(ERROR,
 								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
 								 errmsg("invalid encryption method specified, optarg:%s, index:%d", optarg, i)));
 				}
 				break;
			case 'r':
				strlcpy(OutputFileName, optarg, MAXPGPATH);
				break;
			case 'x':
				MyAuxProcType = atoi(optarg);
				break;
 			case 'R':
 				terminal_fd = atoi(optarg);
 				break;
 			case 'u':
 				bootstrap_old_key_datadir = pstrdup(optarg);
 				break;
			case 'X':
				{
					int			WalSegSz = strtoul(optarg, NULL, 0);

					if (!IsValidWalSegSize(WalSegSz))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("-X requires a power of two value between 1 MB and 1 GB")));
					SetConfigOption("wal_segment_size", optarg, PGC_INTERNAL,
									PGC_S_OVERRIDE);
				}
				break;
			case 'c':
			case '-':
				{
					char	   *name,
							   *value;

					ParseLongOption(optarg, &name, &value);
					if (!value)
					{
						if (flag == '-')
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("--%s requires a value",
											optarg)));
						else
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("-c %s requires a value",
											optarg)));
					}

					SetConfigOption(name, value, PGC_POSTMASTER, PGC_S_ARGV);
					free(name);
					if (value)
						free(value);
					break;
				}
			default:
				write_stderr("Try \"%s --help\" for more information.\n",
							 progname);
				proc_exit(1);
				break;
		}
	}

	if (argc != optind)
	{
		write_stderr("%s: invalid command-line arguments\n", progname);
		proc_exit(1);
	}

	switch (MyAuxProcType)
	{
		case StartupProcess:
			MyBackendType = B_STARTUP;
			break;
		case ArchiverProcess:
			MyBackendType = B_ARCHIVER;
			break;
		case BgWriterProcess:
			MyBackendType = B_BG_WRITER;
			break;
		case CheckpointerProcess:
			MyBackendType = B_CHECKPOINTER;
			break;
		case WalWriterProcess:
			MyBackendType = B_WAL_WRITER;
			break;
		case WalReceiverProcess:
			MyBackendType = B_WAL_RECEIVER;
			break;
		default:
			MyBackendType = B_INVALID;
	}
	if (IsUnderPostmaster)
		init_ps_display(NULL);

	/* Acquire configuration parameters, unless inherited from postmaster */
	if (!IsUnderPostmaster)
	{
		if (!SelectConfigFiles(userDoption, progname))
			proc_exit(1);
	}

	if (userDoption)
	{
		/* userDoption isn't used any more */
		pfree(userDoption);
		userDoption = NULL;
	}

	/*
	 * Validate we have been given a reasonable-looking DataDir and change
	 * into it (if under postmaster, should be done already).
	 */
	if (!IsUnderPostmaster)
	{
		checkDataDir();
		ChangeToDataDir();
	}

	/* If standalone, create lockfile for data directory */
	if (!IsUnderPostmaster)
		CreateDataDirLockFile(false);

	SetProcessingMode(BootstrapProcessing);
	IgnoreSystemIndexes = true;

	/* Initialize MaxBackends (if under postmaster, was done already) */
	if (!IsUnderPostmaster)
		InitializeMaxBackends();

	BaseInit();

	/*
	 * When we are an auxiliary process, we aren't going to do the full
	 * InitPostgres pushups, but there are a couple of things that need to get
	 * lit up even in an auxiliary process.
	 */
	if (IsUnderPostmaster)
	{
		/*
		 * Create a PGPROC so we can use LWLocks.  In the EXEC_BACKEND case,
		 * this was already done by SubPostmasterMain().
		 */
#ifndef EXEC_BACKEND
		InitAuxiliaryProcess();
#endif

		/*
		 * Assign the ProcSignalSlot for an auxiliary process.  Since it
		 * doesn't have a BackendId, the slot is statically allocated based on
		 * the auxiliary process type (MyAuxProcType).  Backends use slots
		 * indexed in the range from 1 to MaxBackends (inclusive), so we use
		 * MaxBackends + AuxProcType + 1 as the index of the slot for an
		 * auxiliary process.
		 *
		 * This will need rethinking if we ever want more than one of a
		 * particular auxiliary process type.
		 */
		ProcSignalInit(MaxBackends + MyAuxProcType + 1);

		/* finish setting up bufmgr.c */
		InitBufferPoolBackend();

		/*
		 * Auxiliary processes don't run transactions, but they may need a
		 * resource owner anyway to manage buffer pins acquired outside
		 * transactions (and, perhaps, other things in future).
		 */
		CreateAuxProcessResourceOwner();

		/* Initialize statistics reporting */
		pgstat_initialize();

		/* Initialize backend status information */
		pgstat_beinit();
		pgstat_bestart();

		/* register a before-shutdown callback for LWLock cleanup */
		before_shmem_exit(ShutdownAuxiliaryProcess, 0);
	}

	/*
	 * XLOG operations
	 */
	SetProcessingMode(NormalProcessing);

	switch (MyAuxProcType)
	{
		case CheckerProcess:
			/* don't set signals, they're useless here */
			CheckerModeMain();
			proc_exit(1);		/* should never return */

		case BootstrapProcess:

			/*
			 * There was a brief instant during which mode was Normal; this is
			 * okay.  We need to be in bootstrap mode during BootStrapXLOG for
			 * the sake of multixact initialization.
			 */
			SetProcessingMode(BootstrapProcessing);
			bootstrap_signals();
			BootStrapXLOG();
			BootstrapModeMain();
			proc_exit(1);		/* should never return */

		case StartupProcess:
			StartupProcessMain();
			proc_exit(1);

		case ArchiverProcess:
			PgArchiverMain();
			proc_exit(1);

		case BgWriterProcess:
			BackgroundWriterMain();
			proc_exit(1);

		case CheckpointerProcess:
			CheckpointerMain();
			proc_exit(1);

		case WalWriterProcess:
			InitXLOGAccess();
			WalWriterMain();
			proc_exit(1);

		case WalReceiverProcess:
			WalReceiverMain();
			proc_exit(1);

		default:
			elog(PANIC, "unrecognized process type: %d", (int) MyAuxProcType);
			proc_exit(1);
	}
}

/*
 * In shared memory checker mode, all we really want to do is create shared
 * memory and semaphores (just to prove we can do it with the current GUC
 * settings).  Since, in fact, that was already done by BaseInit(),
 * we have nothing more to do here.
 */
static void
CheckerModeMain(void)
{
	proc_exit(0);
}

/*
 *	 The main entry point for running the backend in bootstrap mode
 *
 *	 The bootstrap mode is used to initialize the template database.
 *	 The bootstrap backend doesn't speak SQL, but instead expects
 *	 commands in a special bootstrap language.
 */
static void
BootstrapModeMain(void)
{
	int			i;

	Assert(!IsUnderPostmaster);
	Assert(IsBootstrapProcessingMode());

	/*
	 * To ensure that src/common/link-canary.c is linked into the backend, we
	 * must call it from somewhere.  Here is as good as anywhere.
	 */
	if (pg_link_canary_is_frontend())
		elog(ERROR, "backend is incorrectly linked to frontend functions");

	/*
	 * Do backend-like initialization for bootstrap mode
	 */
	InitProcess();

	InitPostgres(NULL, InvalidOid, NULL, InvalidOid, NULL, false);

	/* Initialize stuff for bootstrap-file processing */
	for (i = 0; i < MAXATTR; i++)
	{
		attrtypes[i] = NULL;
		Nulls[i] = false;
	}

	/*
	 * Process bootstrap input.
	 */
	StartTransactionCommand();
	boot_yyparse();
	CommitTransactionCommand();

	/*
	 * We should now know about all mapped relations, so it's okay to write
	 * out the initial relation mapping files.
	 */
	RelationMapFinishBootstrap();

	/* Clean up and exit */
	cleanup();
	proc_exit(0);
}


/* ----------------------------------------------------------------
 *						misc functions
 * ----------------------------------------------------------------
 */

/*
 * Set up signal handling for a bootstrap process
 */
static void
bootstrap_signals(void)
{
	Assert(!IsUnderPostmaster);

	/*
	 * We don't actually need any non-default signal handling in bootstrap
	 * mode; "curl up and die" is a sufficient response for all these cases.
	 * Let's set that handling explicitly, as documentation if nothing else.
	 */
	pqsignal(SIGHUP, SIG_DFL);
	pqsignal(SIGINT, SIG_DFL);
	pqsignal(SIGTERM, SIG_DFL);
	pqsignal(SIGQUIT, SIG_DFL);
}

/*
 * Begin shutdown of an auxiliary process.  This is approximately the equivalent
 * of ShutdownPostgres() in postinit.c.  We can't run transactions in an
 * auxiliary process, so most of the work of AbortTransaction() is not needed,
 * but we do need to make sure we've released any LWLocks we are holding.
 * (This is only critical during an error exit.)
 */
static void
ShutdownAuxiliaryProcess(int code, Datum arg)
{
	LWLockReleaseAll();
	ConditionVariableCancelSleep();
	pgstat_report_wait_end();
}

/* ----------------------------------------------------------------
 *				MANUAL BACKEND INTERACTIVE INTERFACE COMMANDS
 * ----------------------------------------------------------------
 */

/* ----------------
 *		boot_openrel
 *
 * Execute BKI OPEN command.
 * ----------------
 */
void
boot_openrel(char *relname)
{
	int			i;

	if (strlen(relname) >= NAMEDATALEN)
		relname[NAMEDATALEN - 1] = '\0';

	/*
	 * pg_type must be filled before any OPEN command is executed, hence we
	 * can now populate Typ if we haven't yet.
	 */
	if (Typ == NIL)
		populate_typ_list();

	if (boot_reldesc != NULL)
		closerel(NULL);

	elog(DEBUG4, "open relation %s, attrsize %d",
		 relname, (int) ATTRIBUTE_FIXED_PART_SIZE);

	boot_reldesc = table_openrv(makeRangeVar(NULL, relname, -1), NoLock);
	numattr = RelationGetNumberOfAttributes(boot_reldesc);
	for (i = 0; i < numattr; i++)
	{
		if (attrtypes[i] == NULL)
			attrtypes[i] = AllocateAttribute();
		memmove((char *) attrtypes[i],
				(char *) TupleDescAttr(boot_reldesc->rd_att, i),
				ATTRIBUTE_FIXED_PART_SIZE);

		{
			Form_pg_attribute at = attrtypes[i];

			elog(DEBUG4, "create attribute %d name %s len %d num %d type %u",
				 i, NameStr(at->attname), at->attlen, at->attnum,
				 at->atttypid);
		}
	}
}

/* ----------------
 *		closerel
 * ----------------
 */
void
closerel(char *name)
{
	if (name)
	{
		if (boot_reldesc)
		{
			if (strcmp(RelationGetRelationName(boot_reldesc), name) != 0)
				elog(ERROR, "close of %s when %s was expected",
					 name, RelationGetRelationName(boot_reldesc));
		}
		else
			elog(ERROR, "close of %s before any relation was opened",
				 name);
	}

	if (boot_reldesc == NULL)
		elog(ERROR, "no open relation to close");
	else
	{
		elog(DEBUG4, "close relation %s",
			 RelationGetRelationName(boot_reldesc));
		table_close(boot_reldesc, NoLock);
		boot_reldesc = NULL;
	}
}



/* ----------------
 * DEFINEATTR()
 *
 * define a <field,type> pair
 * if there are n fields in a relation to be created, this routine
 * will be called n times
 * ----------------
 */
void
DefineAttr(char *name, char *type, int attnum, int nullness)
{
	Oid			typeoid;

	if (boot_reldesc != NULL)
	{
		elog(WARNING, "no open relations allowed with CREATE command");
		closerel(NULL);
	}

	if (attrtypes[attnum] == NULL)
		attrtypes[attnum] = AllocateAttribute();
	MemSet(attrtypes[attnum], 0, ATTRIBUTE_FIXED_PART_SIZE);

	namestrcpy(&attrtypes[attnum]->attname, name);
	elog(DEBUG4, "column %s %s", NameStr(attrtypes[attnum]->attname), type);
	attrtypes[attnum]->attnum = attnum + 1;

	typeoid = gettype(type);

	if (Typ != NIL)
	{
		attrtypes[attnum]->atttypid = Ap->am_oid;
		attrtypes[attnum]->attlen = Ap->am_typ.typlen;
		attrtypes[attnum]->attbyval = Ap->am_typ.typbyval;
		attrtypes[attnum]->attalign = Ap->am_typ.typalign;
		attrtypes[attnum]->attstorage = Ap->am_typ.typstorage;
		attrtypes[attnum]->attcompression = InvalidCompressionMethod;
		attrtypes[attnum]->attcollation = Ap->am_typ.typcollation;
		/* if an array type, assume 1-dimensional attribute */
		if (Ap->am_typ.typelem != InvalidOid && Ap->am_typ.typlen < 0)
			attrtypes[attnum]->attndims = 1;
		else
			attrtypes[attnum]->attndims = 0;
	}
	else
	{
		attrtypes[attnum]->atttypid = TypInfo[typeoid].oid;
		attrtypes[attnum]->attlen = TypInfo[typeoid].len;
		attrtypes[attnum]->attbyval = TypInfo[typeoid].byval;
		attrtypes[attnum]->attalign = TypInfo[typeoid].align;
		attrtypes[attnum]->attstorage = TypInfo[typeoid].storage;
		attrtypes[attnum]->attcompression = InvalidCompressionMethod;
		attrtypes[attnum]->attcollation = TypInfo[typeoid].collation;
		/* if an array type, assume 1-dimensional attribute */
		if (TypInfo[typeoid].elem != InvalidOid &&
			attrtypes[attnum]->attlen < 0)
			attrtypes[attnum]->attndims = 1;
		else
			attrtypes[attnum]->attndims = 0;
	}

	/*
	 * If a system catalog column is collation-aware, force it to use C
	 * collation, so that its behavior is independent of the database's
	 * collation.  This is essential to allow template0 to be cloned with a
	 * different database collation.
	 */
	if (OidIsValid(attrtypes[attnum]->attcollation))
		attrtypes[attnum]->attcollation = C_COLLATION_OID;

	attrtypes[attnum]->attstattarget = -1;
	attrtypes[attnum]->attcacheoff = -1;
	attrtypes[attnum]->atttypmod = -1;
	attrtypes[attnum]->attislocal = true;

	if (nullness == BOOTCOL_NULL_FORCE_NOT_NULL)
	{
		attrtypes[attnum]->attnotnull = true;
	}
	else if (nullness == BOOTCOL_NULL_FORCE_NULL)
	{
		attrtypes[attnum]->attnotnull = false;
	}
	else
	{
		Assert(nullness == BOOTCOL_NULL_AUTO);

		/*
		 * Mark as "not null" if type is fixed-width and prior columns are
		 * likewise fixed-width and not-null.  This corresponds to case where
		 * column can be accessed directly via C struct declaration.
		 */
		if (attrtypes[attnum]->attlen > 0)
		{
			int			i;

			/* check earlier attributes */
			for (i = 0; i < attnum; i++)
			{
				if (attrtypes[i]->attlen <= 0 ||
					!attrtypes[i]->attnotnull)
					break;
			}
			if (i == attnum)
				attrtypes[attnum]->attnotnull = true;
		}
	}
}


/* ----------------
 *		InsertOneTuple
 *
 * If objectid is not zero, it is a specific OID to assign to the tuple.
 * Otherwise, an OID will be assigned (if necessary) by heap_insert.
 * ----------------
 */
void
InsertOneTuple(void)
{
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	int			i;

	elog(DEBUG4, "inserting row with %d columns", numattr);

	tupDesc = CreateTupleDesc(numattr, attrtypes);
	tuple = heap_form_tuple(tupDesc, values, Nulls);
	pfree(tupDesc);				/* just free's tupDesc, not the attrtypes */

	simple_heap_insert(boot_reldesc, tuple);
	heap_freetuple(tuple);
	elog(DEBUG4, "row inserted");

	/*
	 * Reset null markers for next tuple
	 */
	for (i = 0; i < numattr; i++)
		Nulls[i] = false;
}

/* ----------------
 *		InsertOneValue
 * ----------------
 */
void
InsertOneValue(char *value, int i)
{
	Oid			typoid;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char		typdelim;
	Oid			typioparam;
	Oid			typinput;
	Oid			typoutput;

	AssertArg(i >= 0 && i < MAXATTR);

	elog(DEBUG4, "inserting column %d value \"%s\"", i, value);

	typoid = TupleDescAttr(boot_reldesc->rd_att, i)->atttypid;

	boot_get_type_io_data(typoid,
						  &typlen, &typbyval, &typalign,
						  &typdelim, &typioparam,
						  &typinput, &typoutput);

	values[i] = OidInputFunctionCall(typinput, value, typioparam, -1);

	/*
	 * We use ereport not elog here so that parameters aren't evaluated unless
	 * the message is going to be printed, which generally it isn't
	 */
	ereport(DEBUG4,
			(errmsg_internal("inserted -> %s",
							 OidOutputFunctionCall(typoutput, values[i]))));
}

/* ----------------
 *		InsertOneNull
 * ----------------
 */
void
InsertOneNull(int i)
{
	elog(DEBUG4, "inserting column %d NULL", i);
	Assert(i >= 0 && i < MAXATTR);
	if (TupleDescAttr(boot_reldesc->rd_att, i)->attnotnull)
		elog(ERROR,
			 "NULL value specified for not-null column \"%s\" of relation \"%s\"",
			 NameStr(TupleDescAttr(boot_reldesc->rd_att, i)->attname),
			 RelationGetRelationName(boot_reldesc));
	values[i] = PointerGetDatum(NULL);
	Nulls[i] = true;
}

/* ----------------
 *		cleanup
 * ----------------
 */
static void
cleanup(void)
{
	if (boot_reldesc != NULL)
		closerel(NULL);
}

/* ----------------
 *		populate_typ_list
 *
 * Load the Typ list by reading pg_type.
 * ----------------
 */
static void
populate_typ_list(void)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	MemoryContext old;

	Assert(Typ == NIL);

	rel = table_open(TypeRelationId, NoLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	old = MemoryContextSwitchTo(TopMemoryContext);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_type typForm = (Form_pg_type) GETSTRUCT(tup);
		struct typmap *newtyp;

		newtyp = (struct typmap *) palloc(sizeof(struct typmap));
		Typ = lappend(Typ, newtyp);

		newtyp->am_oid = typForm->oid;
		memcpy(&newtyp->am_typ, typForm, sizeof(newtyp->am_typ));
	}
	MemoryContextSwitchTo(old);
	table_endscan(scan);
	table_close(rel, NoLock);
}

/* ----------------
 *		gettype
 *
 * NB: this is really ugly; it will return an integer index into TypInfo[],
 * and not an OID at all, until the first reference to a type not known in
 * TypInfo[].  At that point it will read and cache pg_type in Typ,
 * and subsequently return a real OID (and set the global pointer Ap to
 * point at the found row in Typ).  So caller must check whether Typ is
 * still NIL to determine what the return value is!
 * ----------------
 */
static Oid
gettype(char *type)
{
	if (Typ != NIL)
	{
		ListCell   *lc;

		foreach(lc, Typ)
		{
			struct typmap *app = lfirst(lc);

			if (strncmp(NameStr(app->am_typ.typname), type, NAMEDATALEN) == 0)
			{
				Ap = app;
				return app->am_oid;
			}
		}

		/*
		 * The type wasn't known; reload the pg_type contents and check again
		 * to handle composite types, added since last populating the list.
		 */

		list_free_deep(Typ);
		Typ = NIL;
		populate_typ_list();

		/*
		 * Calling gettype would result in infinite recursion for types
		 * missing in pg_type, so just repeat the lookup.
		 */
		foreach(lc, Typ)
		{
			struct typmap *app = lfirst(lc);

			if (strncmp(NameStr(app->am_typ.typname), type, NAMEDATALEN) == 0)
			{
				Ap = app;
				return app->am_oid;
			}
		}
	}
	else
	{
		int			i;

		for (i = 0; i < n_types; i++)
		{
			if (strncmp(type, TypInfo[i].name, NAMEDATALEN) == 0)
				return i;
		}
		/* Not in TypInfo, so we'd better be able to read pg_type now */
		elog(DEBUG4, "external type: %s", type);
		populate_typ_list();
		return gettype(type);
	}
	elog(ERROR, "unrecognized type \"%s\"", type);
	/* not reached, here to make compiler happy */
	return 0;
}

/* ----------------
 *		boot_get_type_io_data
 *
 * Obtain type I/O information at bootstrap time.  This intentionally has
 * almost the same API as lsyscache.c's get_type_io_data, except that
 * we only support obtaining the typinput and typoutput routines, not
 * the binary I/O routines.  It is exported so that array_in and array_out
 * can be made to work during early bootstrap.
 * ----------------
 */
void
boot_get_type_io_data(Oid typid,
					  int16 *typlen,
					  bool *typbyval,
					  char *typalign,
					  char *typdelim,
					  Oid *typioparam,
					  Oid *typinput,
					  Oid *typoutput)
{
	if (Typ != NIL)
	{
		/* We have the boot-time contents of pg_type, so use it */
		struct typmap *ap = NULL;
		ListCell   *lc;

		foreach(lc, Typ)
		{
			ap = lfirst(lc);
			if (ap->am_oid == typid)
				break;
		}

		if (!ap || ap->am_oid != typid)
			elog(ERROR, "type OID %u not found in Typ list", typid);

		*typlen = ap->am_typ.typlen;
		*typbyval = ap->am_typ.typbyval;
		*typalign = ap->am_typ.typalign;
		*typdelim = ap->am_typ.typdelim;

		/* XXX this logic must match getTypeIOParam() */
		if (OidIsValid(ap->am_typ.typelem))
			*typioparam = ap->am_typ.typelem;
		else
			*typioparam = typid;

		*typinput = ap->am_typ.typinput;
		*typoutput = ap->am_typ.typoutput;
	}
	else
	{
		/* We don't have pg_type yet, so use the hard-wired TypInfo array */
		int			typeindex;

		for (typeindex = 0; typeindex < n_types; typeindex++)
		{
			if (TypInfo[typeindex].oid == typid)
				break;
		}
		if (typeindex >= n_types)
			elog(ERROR, "type OID %u not found in TypInfo", typid);

		*typlen = TypInfo[typeindex].len;
		*typbyval = TypInfo[typeindex].byval;
		*typalign = TypInfo[typeindex].align;
		/* We assume typdelim is ',' for all boot-time types */
		*typdelim = ',';

		/* XXX this logic must match getTypeIOParam() */
		if (OidIsValid(TypInfo[typeindex].elem))
			*typioparam = TypInfo[typeindex].elem;
		else
			*typioparam = typid;

		*typinput = TypInfo[typeindex].inproc;
		*typoutput = TypInfo[typeindex].outproc;
	}
}

/* ----------------
 *		AllocateAttribute
 *
 * Note: bootstrap never sets any per-column ACLs, so we only need
 * ATTRIBUTE_FIXED_PART_SIZE space per attribute.
 * ----------------
 */
static Form_pg_attribute
AllocateAttribute(void)
{
	return (Form_pg_attribute)
		MemoryContextAllocZero(TopMemoryContext, ATTRIBUTE_FIXED_PART_SIZE);
}

/*
 *	index_register() -- record an index that has been set up for building
 *						later.
 *
 *		At bootstrap time, we define a bunch of indexes on system catalogs.
 *		We postpone actually building the indexes until just before we're
 *		finished with initialization, however.  This is because the indexes
 *		themselves have catalog entries, and those have to be included in the
 *		indexes on those catalogs.  Doing it in two phases is the simplest
 *		way of making sure the indexes have the right contents at the end.
 */
void
index_register(Oid heap,
			   Oid ind,
			   IndexInfo *indexInfo)
{
	IndexList  *newind;
	MemoryContext oldcxt;

	/*
	 * XXX mao 10/31/92 -- don't gc index reldescs, associated info at
	 * bootstrap time.  we'll declare the indexes now, but want to create them
	 * later.
	 */

	if (nogc == NULL)
		nogc = AllocSetContextCreate(NULL,
									 "BootstrapNoGC",
									 ALLOCSET_DEFAULT_SIZES);

	oldcxt = MemoryContextSwitchTo(nogc);

	newind = (IndexList *) palloc(sizeof(IndexList));
	newind->il_heap = heap;
	newind->il_ind = ind;
	newind->il_info = (IndexInfo *) palloc(sizeof(IndexInfo));

	memcpy(newind->il_info, indexInfo, sizeof(IndexInfo));
	/* expressions will likely be null, but may as well copy it */
	newind->il_info->ii_Expressions =
		copyObject(indexInfo->ii_Expressions);
	newind->il_info->ii_ExpressionsState = NIL;
	/* predicate will likely be null, but may as well copy it */
	newind->il_info->ii_Predicate =
		copyObject(indexInfo->ii_Predicate);
	newind->il_info->ii_PredicateState = NULL;
	/* no exclusion constraints at bootstrap time, so no need to copy */
	Assert(indexInfo->ii_ExclusionOps == NULL);
	Assert(indexInfo->ii_ExclusionProcs == NULL);
	Assert(indexInfo->ii_ExclusionStrats == NULL);

	newind->il_next = ILHead;
	ILHead = newind;

	MemoryContextSwitchTo(oldcxt);
}


/*
 * build_indices -- fill in all the indexes registered earlier
 */
void
build_indices(void)
{
	for (; ILHead != NULL; ILHead = ILHead->il_next)
	{
		Relation	heap;
		Relation	ind;

		/* need not bother with locks during bootstrap */
		heap = table_open(ILHead->il_heap, NoLock);
		ind = index_open(ILHead->il_ind, NoLock);

		index_build(heap, ind, ILHead->il_info, false, false);

		index_close(ind, NoLock);
		table_close(heap, NoLock);
	}
}
