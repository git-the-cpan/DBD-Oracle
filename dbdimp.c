/*
   $Id: dbdimp.c,v 1.58 1999/07/12 03:20:42 timbo Exp $

   Copyright (c) 1994,1995,1996,1997,1998  Tim Bunce

   You may distribute under the terms of either the GNU General Public
   License or the Artistic License, as specified in the Perl README file,
   with the exception that it cannot be placed on a CD-ROM or similar media
   for commercial distribution without the prior approval of the author.

*/

#include "Oracle.h"


/* XXX DBI should provide a better version of this */
#define IS_DBI_HANDLE(h) \
    (SvROK(h) && SvTYPE(SvRV(h)) == SVt_PVHV && \
	SvRMAGICAL(SvRV(h)) && (SvMAGIC(SvRV(h)))->mg_type == 'P')


DBISTATE_DECLARE;

int ora_fetchtest;

static int ora_login_nomsg;	/* don't fetch real login errmsg if true  */
static int ora_sigchld_restart = 1;
#ifndef OCI_V8_SYNTAX
static int set_sigint_handler  = 0;
#endif

static int ora2sql_type _((int oratype));

void ora_free_phs_contents _((phs_t *phs));
static void dump_env_to_trace();

void
dbd_init(dbistate)
    dbistate_t *dbistate;
{
    DBIS = dbistate;
    dbd_init_oci(dbistate);

    if (getenv("DBD_ORACLE_LOGIN_NOMSG"))
	ora_login_nomsg = atoi(getenv("DBD_ORACLE_LOGIN_NOMSG"));
    if (getenv("DBD_ORACLE_SIGCHLD"))
	ora_sigchld_restart = atoi(getenv("DBD_ORACLE_SIGCHLD"));
}


int
dbd_discon_all(drh, imp_drh)
    SV *drh;
    imp_drh_t *imp_drh;
{
    dTHR;

    /* The disconnect_all concept is flawed and needs more work */
    if (!dirty && !SvTRUE(perl_get_sv("DBI::PERL_ENDING",0))) {
	sv_setiv(DBIc_ERR(imp_drh), (IV)1);
	sv_setpv(DBIc_ERRSTR(imp_drh),
		(char*)"disconnect_all not implemented");
	DBIh_EVENT2(drh, ERROR_event,
		DBIc_ERR(imp_drh), DBIc_ERRSTR(imp_drh));
	return FALSE;
    }
    if (perl_destruct_level)
	perl_destruct_level = 0;
    return FALSE;
}



void
dbd_fbh_dump(fbh, i, aidx)
    imp_fbh_t *fbh;
    int i;
    int aidx;	/* array index */
{
    FILE *fp = DBILOGFP;
    fprintf(fp, "    fbh %d: '%s'\t%s, ",
		i, fbh->name, (fbh->nullok) ? "NULLable" : "NO null ");
    fprintf(fp, "otype %3d->%3d, dbsize %ld/%ld, p%d.s%d\n",
	    fbh->dbtype, fbh->ftype, (long)fbh->dbsize,(long)fbh->disize,
	    fbh->prec, fbh->scale);
    if (fbh->fb_ary) {
    fprintf(fp, "      out: ftype %d, bufl %d. indp %d, rlen %d, rcode %d\n",
	    fbh->ftype, fbh->fb_ary->bufl, fbh->fb_ary->aindp[aidx],
	    fbh->fb_ary->arlen[aidx], fbh->fb_ary->arcode[aidx]);
    }
}


int
ora_dbtype_is_long(dbtype)
    int dbtype;
{
    /* Is it a LONG, LONG RAW, LONG VARCHAR or LONG VARRAW type?	*/
    /* Return preferred type code to use if it's a long, else 0.	*/
    if (dbtype == 8 || dbtype == 24)	/* LONG or LONG RAW		*/
	return dbtype;			/*		--> same	*/
    if (dbtype == 94)			/* LONG VARCHAR			*/
	return 8;			/*		--> LONG	*/
    if (dbtype == 95)			/* LONG VARRAW			*/
	return 24;			/*		--> LONG RAW	*/
    return 0;
}

static int
oratype_bind_ok(dbtype)	/* It's a type we support for placeholders */
    int dbtype;
{
    /* basically we support types that can be returned as strings */
    switch(dbtype) {
    case  1:	/* VARCHAR2	*/
    case  5:	/* STRING	*/
    case  8:	/* LONG		*/
    case 23:	/* RAW		*/
    case 24:	/* LONG RAW	*/
    case 96:	/* CHAR		*/
    case 97:	/* CHARZ	*/
    case 106:	/* MLSLABEL	*/
    case 102:	/* SQLT_CUR	OCI 7 cursor variable	*/
    case 112:	/* SQLT_CLOB / long	*/
    case 113:	/* SQLT_BLOB / long	*/
    case 116:	/* SQLT_RSET	OCI 8 cursor variable	*/
	return 1;
    }
    return 0;
}


/* --- allocate and free oracle oci 'array' buffers --- */

fb_ary_t *
fb_ary_alloc(bufl, size)
    int bufl;
    int size;
{
    fb_ary_t *fb_ary;
    /* these should be reworked to only to one Newz()	*/
    /* and setup the pointers in the head fb_ary struct	*/
    Newz(42, fb_ary, sizeof(fb_ary_t), fb_ary_t);
    Newz(42, fb_ary->abuf,   size * bufl, ub1);
    Newz(42, fb_ary->aindp,  size,        sb2);
    Newz(42, fb_ary->arlen,  size,        ub2);
    Newz(42, fb_ary->arcode, size,        ub2);
    fb_ary->bufl = bufl;
    return fb_ary;
}

void
fb_ary_free(fb_ary)
    fb_ary_t *fb_ary;
{
    Safefree(fb_ary->abuf);
    Safefree(fb_ary->aindp);
    Safefree(fb_ary->arlen);
    Safefree(fb_ary->arcode);
    Safefree(fb_ary);
}


/* ================================================================== */


int
dbd_db_login(dbh, imp_dbh, dbname, uid, pwd)
    SV *dbh; imp_dbh_t *imp_dbh; char *dbname; char *uid; char *pwd;
{
    return dbd_db_login6(dbh, imp_dbh, dbname, uid, pwd, Nullsv);
}


int
dbd_db_login6(dbh, imp_dbh, dbname, uid, pwd, attr)
    SV *dbh;
    imp_dbh_t *imp_dbh;
    char *dbname;
    char *uid;
    char *pwd;
    SV *attr;
{
    dTHR;
    sword status;

#ifdef OCI_V8_SYNTAX
    D_imp_drh_from_dbh;
    ub4 cred_type;
    ub4 mode_type = OCI_DEFAULT;
    SV **mode_type_sv;

    if (DBIS->debug >= 6 )
	dump_env_to_trace();

    if (!imp_drh->envhp) {
	/* OCI_OBJECT needed for OCIDescribeAny of table with LOBs else	*/
	/* you get a core dump (Not doc'd in 8.0.4). Thanks Oracle!	*/
	ub4 init_mode = OCI_OBJECT;
	SV **init_mode_sv;
	DBD_ATTRIB_GET_IV(attr, "ora_init_mode",15, init_mode_sv, mode_type);
	OCIInitialize_log_stat(init_mode, 0, 0,0,0, status);
	if (status != OCI_SUCCESS) {
	    oci_error(dbh, NULL, status, "OCIInitialize");
	    return 0;
	}
	OCIEnvInit_log_stat( &imp_drh->envhp, OCI_DEFAULT, 0, 0, status);
	if (status != OCI_SUCCESS) {
	    oci_error(dbh, (OCIError*)imp_dbh->envhp, status, "OCIEnvInit");
	    return 0;
	}
    }
    imp_dbh->envhp = imp_drh->envhp;

    OCIHandleAlloc_ok(imp_dbh->envhp, &imp_dbh->errhp, OCI_HTYPE_ERROR,  status);
    OCIHandleAlloc_ok(imp_dbh->envhp, &imp_dbh->srvhp, OCI_HTYPE_SERVER, status);
    OCIHandleAlloc_ok(imp_dbh->envhp, &imp_dbh->svchp, OCI_HTYPE_SVCCTX, status);

    OCIServerAttach_log_stat(imp_dbh, dbname, status);
    if (status != OCI_SUCCESS) {
	oci_error(dbh, imp_dbh->errhp, status, "OCIServerAttach");
	OCIHandleFree_log_stat(imp_dbh->srvhp, OCI_HTYPE_SERVER, status);
	OCIHandleFree_log_stat(imp_dbh->svchp, OCI_HTYPE_SVCCTX, status);
	OCIHandleFree_log_stat(imp_dbh->errhp, OCI_HTYPE_ERROR,  status);
	return 0;
    }

    OCIAttrSet_log_stat( imp_dbh->svchp, OCI_HTYPE_SVCCTX, imp_dbh->srvhp, 
                    (ub4) 0, OCI_ATTR_SERVER, imp_dbh->errhp, status);

    OCIHandleAlloc_ok(imp_dbh->envhp, &imp_dbh->authp, OCI_HTYPE_SESSION, status);

    cred_type = ora_parse_uid(imp_dbh, &uid, &pwd);
    DBD_ATTRIB_GET_IV(attr, "ora_session_mode",16, mode_type_sv, mode_type);
    OCISessionBegin_log_stat( imp_dbh->svchp, imp_dbh->errhp, imp_dbh->authp,
		cred_type, mode_type, status);
    if (status != OCI_SUCCESS) {
	oci_error(dbh, imp_dbh->errhp, status, "OCISessionBegin");
	OCIServerDetach_log_stat(imp_dbh->srvhp, imp_dbh->errhp, OCI_DEFAULT, status);
	OCIHandleFree_log_stat(imp_dbh->authp, OCI_HTYPE_SESSION,status);
	OCIHandleFree_log_stat(imp_dbh->srvhp, OCI_HTYPE_SERVER, status);
	OCIHandleFree_log_stat(imp_dbh->errhp, OCI_HTYPE_ERROR,  status);
	OCIHandleFree_log_stat(imp_dbh->svchp, OCI_HTYPE_SVCCTX, status);
	return 0;
    }
 
    OCIAttrSet_log_stat(imp_dbh->svchp, (ub4) OCI_HTYPE_SVCCTX,
                   imp_dbh->authp, (ub4) 0,
                   (ub4) OCI_ATTR_SESSION, imp_dbh->errhp, status);

#else
    if (DBIS->debug >= 6 )
	dump_env_to_trace();

    imp_dbh->lda = &imp_dbh->ldabuf;
    imp_dbh->hda = &imp_dbh->hdabuf[0];
    /* can give duplicate free errors (from Oracle) if connect fails	*/
    status = orlon(imp_dbh->lda, imp_dbh->hda, (text*)uid,-1, (text*)pwd,-1,0);

    if (status) {
	int rc = imp_dbh->lda->rc;
	char buf[100];
	char *msg;
	switch(rc) {	/* add helpful hints to some errors */
	case    0: msg = "login failed, check ORACLE_HOME/bin is on your PATH";  break;
	case 1019: msg = "login failed, probably a symptom of a deeper problem"; break;
	default:   msg = "login failed"; break;
	}
	if (ora_login_nomsg) {
	    /* oerhms in ora_error may hang or corrupt memory (!) after a connect */
	    /* failure in some specific versions of Oracle 7.3.x. So we provide a */
	    /* way to skip the message lookup if ora_login_nomsg is true (set via */
	    /* env var above). */
	    sprintf(buf, 
		"ORA-%05d: (Text for error %d not fetched. Use 'oerr ORA %d' command.)",
		rc, rc, rc);
	    msg = buf;
	}
	ora_error(dbh,	ora_login_nomsg ? NULL : imp_dbh->lda, rc, msg);
	return 0;
    }

    if (!set_sigint_handler) {
	set_sigint_handler = 1;
	/* perl's sign handler is sighandler */
	/* osnsui(??, sighandler, NULL?) */
	/* OCI8: osnsui(word *handlp, void (*astp), char * ctx)
	** osnsui: Operating System dependent Network Set User-side
	** Interrupt. Add an interrupt handling procedure astp. 
	** Whenever a user interrupt(such as a ^C) occurs, call astp
	** with argument ctx. Put in *handlp handle for this 
	** handler so that it may be cleared with osncui.
	** Note that there may be many handlers; each should 
	** be cleared using osncui. An error code is 
	** returned if an error occurs.
	*/
    }

#ifdef SA_RESTART
#ifndef SIGCLD
#define SIGCLD SIGCHLD
#endif
    /* If orlon has installed a handler for SIGCLD, then reinstall it	*/
    /* with SA_RESTART.  We only do this if connected ok since I've	*/
    /* seen the process loop after being interrupted after connect failed. */
    if (ora_sigchld_restart) {
	struct sigaction act;
	if (sigaction( SIGCLD, (struct sigaction*)0, &act ) == 0
		&&  (act.sa_handler != SIG_DFL && act.sa_handler != SIG_IGN)
		&&  (act.sa_flags & SA_RESTART) == 0) {
	    /* XXX we should also check that act.sa_handler is not the perl handler */
	    act.sa_flags |= SA_RESTART;
	    sigaction( SIGCLD, &act, (struct sigaction*)0 );
	    if (dbis->debug >= 3)
		warn("dbd_db_login: sigaction errno %d, handler %lx, flags %lx",
			errno,act.sa_handler,act.sa_flags);
	    if (dbis->debug >= 2)
		fprintf(DBILOGFP, "    dbd_db_login: set SA_RESTART on Oracle SIGCLD handler.\n");
	}
    }  
#endif	/* HAS_SIGACTION */

#endif	/* OCI_V8_SYNTAX */

    DBIc_IMPSET_on(imp_dbh);	/* imp_dbh set up now			*/
    DBIc_ACTIVE_on(imp_dbh);	/* call disconnect before freeing	*/
    return 1;
}


int
dbd_db_commit(dbh, imp_dbh)
    SV *dbh;
    imp_dbh_t *imp_dbh;
{
#ifdef OCI_V8_SYNTAX
    sword status;
    OCITransCommit_log_stat(imp_dbh->svchp, imp_dbh->errhp, OCI_DEFAULT, status);
    if (status != OCI_SUCCESS) {
	oci_error(dbh, imp_dbh->errhp, status, "OCITransCommit");
#else
    if (ocom(imp_dbh->lda)) {
	ora_error(dbh, imp_dbh->lda, imp_dbh->lda->rc, "commit failed");
#endif
	return 0;
    }
    return 1;
}

int
dbd_db_rollback(dbh, imp_dbh)
    SV *dbh;
    imp_dbh_t *imp_dbh;
{
#ifdef OCI_V8_SYNTAX
    sword status;
    OCITransRollback_log_stat(imp_dbh->svchp, imp_dbh->errhp, OCI_DEFAULT, status);
    if (status != OCI_SUCCESS) {
	oci_error(dbh, imp_dbh->errhp, status, "OCITransRollback");
#else
    if (orol(imp_dbh->lda)) {
	ora_error(dbh, imp_dbh->lda, imp_dbh->lda->rc, "rollback failed");
#endif
	return 0;
    }
    return 1;
}


int
dbd_db_disconnect(dbh, imp_dbh)
    SV *dbh;
    imp_dbh_t *imp_dbh;
{
    dTHR;

    /* We assume that disconnect will always work	*/
    /* since most errors imply already disconnected.	*/
    DBIc_ACTIVE_off(imp_dbh);

    /* Oracle will commit on an orderly disconnect.	*/
    /* See DBI Driver.xst file for the DBI approach.	*/

#ifdef OCI_V8_SYNTAX
    {
        sword s_se, s_sd;
	OCISessionEnd_log_stat(imp_dbh->svchp, imp_dbh->errhp, imp_dbh->authp,
			  OCI_DEFAULT, s_se);
	if (s_se) oci_error(dbh, imp_dbh->errhp, s_se, "OCISessionEnd");
	OCIServerDetach_log_stat(imp_dbh->srvhp, imp_dbh->errhp, OCI_DEFAULT, s_sd);
	if (s_sd) oci_error(dbh, imp_dbh->errhp, s_sd, "OCIServerDetach");
	if (s_se || s_sd)
	    return 0;
    }
#else
    if (ologof(imp_dbh->lda)) {
	ora_error(dbh, imp_dbh->lda, imp_dbh->lda->rc, "disconnect error");
	return 0;
    }
#endif
    /* We don't free imp_dbh since a reference still exists	*/
    /* The DESTROY method is the only one to 'free' memory.	*/
    /* Note that statement objects may still exists for this dbh!	*/
    return 1;
}


void
dbd_db_destroy(dbh, imp_dbh)
    SV *dbh;
    imp_dbh_t *imp_dbh;
{
    if (DBIc_ACTIVE(imp_dbh))
	dbd_db_disconnect(dbh, imp_dbh);
#ifdef OCI_V8_SYNTAX
    {   sword status;
	OCIHandleFree_log_stat(imp_dbh->srvhp, OCI_HTYPE_SERVER, status);
	OCIHandleFree_log_stat(imp_dbh->svchp, OCI_HTYPE_SVCCTX, status);
	OCIHandleFree_log_stat(imp_dbh->errhp, OCI_HTYPE_ERROR,  status);
    }
#else
    /* Nothing in imp_dbh to be freed	*/
#endif
    DBIc_IMPSET_off(imp_dbh);
}


int
dbd_db_STORE_attrib(dbh, imp_dbh, keysv, valuesv)
    SV *dbh;
    imp_dbh_t *imp_dbh;
    SV *keysv;
    SV *valuesv;
{
    STRLEN kl;
    char *key = SvPV(keysv,kl);
    SV *cachesv = NULL;
    int on = SvTRUE(valuesv);

    if (kl==10 && strEQ(key, "AutoCommit")) {
#ifndef OCI_V8_SYNTAX
	if ( (on) ? ocon(imp_dbh->lda) : ocof(imp_dbh->lda) ) {
	    ora_error(dbh, imp_dbh->lda, imp_dbh->lda->rc, "ocon/ocof failed");
	    /* XXX um, we can't return FALSE and true isn't acurate so we croak */
	    croak(SvPV(DBIc_ERRSTR(imp_dbh),na));
	}
#endif	/* OCI V8 handles this as OCIExecuteStmt	*/
	DBIc_set(imp_dbh,DBIcf_AutoCommit, on);
    }
    else if (kl==12 && strEQ(key, "RowCacheSize")) {
	imp_dbh->RowCacheSize = SvIV(valuesv);
    }
    else {
	return FALSE;
    }
    if (cachesv) /* cache value for later DBI 'quick' fetch? */
	hv_store((HV*)SvRV(dbh), key, kl, cachesv, 0);
    return TRUE;
}


SV *
dbd_db_FETCH_attrib(dbh, imp_dbh, keysv)
    SV *dbh;
    imp_dbh_t *imp_dbh;
    SV *keysv;
{
    STRLEN kl;
    char *key = SvPV(keysv,kl);
    SV *retsv = Nullsv;
    /* Default to caching results for DBI dispatch quick_FETCH	*/
    int cacheit = FALSE;

    /* AutoCommit FETCH via DBI */

    if (kl==10 && strEQ(key, "AutoCommit")) {
        retsv = boolSV(DBIc_has(imp_dbh,DBIcf_AutoCommit));
    }
    else if (kl==12 && strEQ(key, "RowCacheSize")) {
	retsv = newSViv(imp_dbh->RowCacheSize);
    }
    if (!retsv)
	return Nullsv;
    if (cacheit) {	/* cache for next time (via DBI quick_FETCH)	*/
	SV **svp = hv_fetch((HV*)SvRV(dbh), key, kl, 1);
	sv_free(*svp);
	*svp = retsv;
	(void)SvREFCNT_inc(retsv);	/* so sv_2mortal won't free it	*/
    }
    if (retsv == &sv_yes || retsv == &sv_no)
	return retsv; /* no need to mortalize yes or no */
    return sv_2mortal(retsv);
}



/* ================================================================== */



void
dbd_preparse(imp_sth, statement)
    imp_sth_t *imp_sth;
    char *statement;
{
    bool in_literal = FALSE;
    char in_comment = '\0';
    char *src, *start, *dest;
    phs_t phs_tpl;
    SV *phs_sv;
    int idx=0;
    char *style="", *laststyle=Nullch;
    STRLEN namelen;

    /* allocate room for copy of statement with spare capacity	*/
    /* for editing '?' or ':1' into ':p1' so we can use obndrv.	*/
    imp_sth->statement = (char*)safemalloc(strlen(statement) * 3);

    /* initialise phs ready to be cloned per placeholder	*/
    memset(&phs_tpl, 0, sizeof(phs_tpl));
    phs_tpl.imp_sth = imp_sth;
    phs_tpl.ftype = 1;	/* VARCHAR2 */
    phs_tpl.maxlen_bound = -1;	/* not yet bound */

    src  = statement;
    dest = imp_sth->statement;
    while(*src) {

	if (in_comment) {
	    /* 981028-jdl on mocha.  Adding all code which deals with           */
	    /*  in_comment variable (its declaration plus 2 code blocks).       */
	    /*  Text appearing within comments should be scanned for neither    */
	    /*  placeholders nor for single quotes (which toggle the in_literal */
	    /*  boolean).  Comments like "3:00" demonstrate the former problem, */
	    /*  and contractions like "don't" demonstrate the latter problem.   */
	    /* The comment style is stored in in_comment; each style is */
	    /* terminated in a different way.                          */
	    if (in_comment == '-' && *src == '\n') {
		in_comment = '\0';
	    }
	    else if (in_comment == '/' && *src == '*' && *(src+1) == '/') {
		*dest++ = *src++; /* avoids asterisk-slash-asterisk issues */
		in_comment = '\0';
	    }
	    *dest++ = *src++;
	    continue;
	}

	if (in_literal) {
	    if (*src == in_literal)
		in_literal = 0;
	    *dest++ = *src++;
	    continue;
	}

	/* Look for comments: '-- oracle-style' or C-style	*/
	if ((*src == '-' && *(src+1) == '-') ||
	    (*src == '/' && *(src+1) == '*'))
	{
	    in_comment = *src;
	    /* We know *src & the next char are to be copied, so do */
	    /*  it.  In the case of C-style comments, it happens to */
	    /*  help us avoid slash-asterisk-slash oddities.        */
	    *dest++ = *src++;
	    *dest++ = *src++;
	    continue;
	}

	if (*src != ':' && *src != '?') {

	    if (*src == '\'' || *src == '"')
		in_literal = *src;

	    *dest++ = *src++;
	    continue;
	}

	/* only here for : or ? outside of a comment or literal	*/

	start = dest;			/* save name inc colon	*/ 
	*dest++ = *src++;
	if (*start == '?') {		/* X/Open standard	*/
	    sprintf(start,":p%d", ++idx); /* '?' -> ':p1' (etc)	*/
	    dest = start+strlen(start);
	    style = "?";

	} else if (isDIGIT(*src)) {	/* ':1'		*/
	    idx = atoi(src);
	    *dest++ = 'p';		/* ':1'->':p1'	*/
	    if (idx <= 0)
		croak("Placeholder :%d invalid, placeholders must be >= 1", idx);
	    while(isDIGIT(*src))
		*dest++ = *src++;
	    style = ":1";

	} else if (isALNUM(*src)) {	/* ':foo'	*/
	    while(isALNUM(*src))	/* includes '_'	*/
		*dest++ = *src++;
	    style = ":foo";
	} else {			/* perhaps ':=' PL/SQL construct */
	    continue;
	}
	*dest = '\0';			/* handy for debugging	*/
	namelen = (dest-start);
	if (laststyle && style != laststyle)
	    croak("Can't mix placeholder styles (%s/%s)",style,laststyle);
	laststyle = style;
	if (imp_sth->all_params_hv == NULL)
	    imp_sth->all_params_hv = newHV();
	phs_tpl.sv = &sv_undef;
	phs_sv = newSVpv((char*)&phs_tpl, sizeof(phs_tpl)+namelen+1);
	hv_store(imp_sth->all_params_hv, start, namelen, phs_sv, 0);
	strcpy( ((phs_t*)(void*)SvPVX(phs_sv))->name, start);
    }
    *dest = '\0';
    if (imp_sth->all_params_hv) {
	DBIc_NUM_PARAMS(imp_sth) = (int)HvKEYS(imp_sth->all_params_hv);
	if (dbis->debug >= 2)
	    fprintf(DBILOGFP, "    dbd_preparse scanned %d distinct placeholders\n",
		(int)DBIc_NUM_PARAMS(imp_sth));
    }
}


int
calc_cache_rows(num_fields, est_width, cache_rows, has_longs)
    int num_fields, est_width, cache_rows, has_longs;
{
    /* Use guessed average on-the-wire row width calculated above	*/
    /* and add in overhead of 5 bytes per field plus 8 bytes per row.	*/
    /* The n*5+8 was determined by studying SQL*Net v2 packets.		*/
    /* It could probably benefit from a more detailed analysis.		*/
    est_width += num_fields*5 + 8;

    if (has_longs)			/* override/disable caching	*/
	cache_rows = 1;			/* else read_blob can't work	*/

    else if (cache_rows < 1) {		/* automatically size the cache	*/
	int txfr_size;
	/*  0 == try to pick 'optimal' cache for this query (default)	*/
	/* <0 == base cache on target transfer size of -n bytes.	*/
	if (cache_rows == 0) {
	    /* Oracle packets on ethernet have max size of around 1460.	*/
	    /* We'll aim to fill our row cache with around 10 per go.	*/
	    /* Using 10 means any 'runt' packets will have less impact.	*/
	    txfr_size = 10 * 1460;	/* default transfer/cache size	*/
	}
	else {	/* user is specifying desired transfer size in bytes	*/
	    txfr_size = -cache_rows;
	}
	cache_rows = txfr_size / est_width;	/* maybe 1 or 0	*/
	/* To ensure good performance with large rows (near or larger	*/
	/* than our target transfer size) we set a minimum cache size.	*/
	if (cache_rows < 6)	/* is cache a 'useful' size?	*/
	    cache_rows = (cache_rows>0) ? 6 : 4;
    }
    if (cache_rows > 32767)	/* keep within Oracle's limits  */
	cache_rows = 32767;

    return cache_rows;
}


static int
ora_sql_type(imp_sth, name, sql_type)
    imp_sth_t *imp_sth;
    char *name;
    int sql_type;
{
    /* XXX should detect DBI reserved standard type range here */

    switch (sql_type) {
    case SQL_NUMERIC:
    case SQL_DECIMAL:
    case SQL_INTEGER:
    case SQL_BIGINT:
    case SQL_TINYINT:
    case SQL_SMALLINT:
    case SQL_FLOAT:
    case SQL_REAL:
    case SQL_DOUBLE:
    case SQL_VARCHAR:
	return 1;	/* Oracle VARCHAR2	*/

    case SQL_CHAR:
	return 96;	/* Oracle CHAR		*/

    case SQL_BINARY:
    case SQL_VARBINARY:
	return 23;	/* Oracle RAW		*/

    case SQL_LONGVARBINARY:
	return 24;	/* Oracle LONG RAW	*/

    case SQL_LONGVARCHAR:
	return 8;	/* Oracle LONG		*/

    case SQL_DATE:
    case SQL_TIME:
    case SQL_TIMESTAMP:
    default:
	if (DBIc_WARN(imp_sth) && imp_sth && name)
	    warn("SQL type %d for '%s' is not fully supported, bound as SQL_VARCHAR instead");
	return ora_sql_type(imp_sth, name, SQL_VARCHAR);
    }
}



static int 
dbd_rebind_ph_char(sth, imp_sth, phs, alen_ptr_ptr) 
    SV *sth;
    imp_sth_t *imp_sth;
    phs_t *phs;
    ub2 **alen_ptr_ptr;
{
    STRLEN value_len;

/* for inserting longs: */
/*    sv_insert +4	*/
/*    sv_chop(phs->sv, SvPV(phs->sv,na)+4);	XXX */

    /* convert to a string ASAP */
    if (!SvPOK(phs->sv) && SvOK(phs->sv))
	sv_2pv(phs->sv, &na);

    if (dbis->debug >= 2) {
	char *val = neatsvpv(phs->sv,0);
 	fprintf(DBILOGFP, "       bind %s <== %.1000s (", phs->name, val);
 	if (SvOK(phs->sv)) 
 	     fprintf(DBILOGFP, "size %ld/%ld/%ld, ",
 		(long)SvCUR(phs->sv),(long)SvLEN(phs->sv),phs->maxlen);
	else fprintf(DBILOGFP, "NULL, ");
 	fprintf(DBILOGFP, "ptype %d, otype %d%s)\n",
 	    (int)SvTYPE(phs->sv), phs->ftype,
 	    (phs->is_inout) ? ", inout" : "");
    }

    /* At the moment we always do sv_setsv() and rebind.	*/
    /* Later we may optimise this so that more often we can	*/
    /* just copy the value & length over and not rebind.	*/

    if (phs->is_inout) {	/* XXX */
	if (SvREADONLY(phs->sv))
	    croak(no_modify);
	if (imp_sth->ora_pad_empty)
	    croak("Can't use ora_pad_empty with bind_param_inout");
	/* phs->sv _is_ the real live variable, it may 'mutate' later	*/
	/* pre-upgrade high to reduce risk of SvPVX realloc/move	*/
	(void)SvUPGRADE(phs->sv, SVt_PVNV);
	/* ensure room for result, 28 is magic number (see sv_2pv)	*/
	SvGROW(phs->sv, ((phs->maxlen < 28) ? 28 : phs->maxlen)+1/*for null*/);
    }
    else {
	/* phs->sv is copy of real variable, upgrade to at least string	*/
	(void)SvUPGRADE(phs->sv, SVt_PV);
    }

    /* At this point phs->sv must be at least a PV with a valid buffer,	*/
    /* even if it's undef (null)					*/
    /* Here we set phs->progv, phs->indp, and value_len.		*/
    if (SvOK(phs->sv)) {
	phs->progv = SvPV(phs->sv, value_len);
	phs->indp  = 0;
    }
    else {	/* it's null but point to buffer incase it's an out var	*/
	phs->progv = SvPVX(phs->sv);	/* can be NULL (undef) */
	phs->indp  = -1;
	value_len  = 0;
    }
    if (imp_sth->ora_pad_empty && value_len==0) {
	sv_setpv(phs->sv, " ");
	phs->progv = SvPV(phs->sv, value_len);
    }
    phs->sv_type = SvTYPE(phs->sv);	/* part of mutation check	*/
    phs->maxlen  = SvLEN(phs->sv)-1;	/* avail buffer space		*/
    if (phs->maxlen < 0)		/* can happen with nulls	*/
	phs->maxlen = 0;

#ifdef OCI_V8_SYNTAX
    phs->alen = value_len + phs->alen_incnull;
#else
    if (value_len + phs->alen_incnull <= UB2MAXVAL) {
	phs->alen = value_len + phs->alen_incnull;
	*alen_ptr_ptr = &phs->alen;
	if (((IV)phs->alen) > phs->maxlen && phs->indp != -1)
	    croak("panic: dbd_rebind_ph alen %ld > maxlen %ld (incnul %d)",
			phs->alen,phs->maxlen, phs->alen_incnull);
    }
    else {
	phs->alen = 0;
	*alen_ptr_ptr = NULL; /* Can't use alen for long LONGs (>64k) */
	if (phs->is_inout)
	    croak("Can't bind LONG values (>%ld) as in/out parameters", (long)UB2MAXVAL);
    }
#endif

    if (dbis->debug >= 3) {
	fprintf(DBILOGFP, "       bind %s <== '%.*s' (size %ld/%ld, otype %d, indp %d)\n",
 	    phs->name,
	    (int)(phs->alen>SvIV(DBIS->neatsvpvlen) ? SvIV(DBIS->neatsvpvlen) : phs->alen),
	    (phs->progv) ? phs->progv : "",
 	    (long)phs->alen, (long)phs->maxlen, phs->ftype, phs->indp);
    }

    return 1;
}


int
pp_exec_rset(SV *sth, imp_sth_t *imp_sth, phs_t *phs, int pre_exec) 
{
    if (pre_exec) {	/* pre-execute - allocate a statement handle */
	dSP;
	D_imp_dbh_from_sth;
	SV *sth_i;
	HV *init_attr = newHV();
	int count;
	if (DBIS->debug >= 3)
	    fprintf(DBILOGFP, "       bind %s - allocating new sth...\n", phs->name);
#ifdef OCI_V8_SYNTAX
    {
	sword status;
	if (!phs->desc_h || 1) { /* XXX phs->desc_t != OCI_HTYPE_STMT) { */
	    if (phs->desc_h) {
		OCIHandleFree_log_stat(phs->desc_h, phs->desc_t, status);
		phs->desc_h = NULL;
	    }
	    phs->desc_t = OCI_HTYPE_STMT;
	    OCIHandleAlloc_ok(imp_sth->envhp, &phs->desc_h, phs->desc_t, status);
	}
	phs->progv = (void*)&phs->desc_h;
	phs->maxlen = 0;
	OCIBindByName_log_stat(imp_sth->stmhp, &phs->bndhp, imp_sth->errhp,
		(text*)phs->name, strlen(phs->name),
		phs->progv, 0,
		phs->ftype, 0, /* using &phs->indp triggers ORA-01001 errors! */
		NULL, 0, 0, NULL, OCI_DEFAULT, status);
	if (status != OCI_SUCCESS) {
	    oci_error(sth, imp_sth->errhp, status, "OCIBindByName SQLT_RSET");
	    return 0;
	}
    }
#else
    {
	Cda_Def *cda;
	assert(phs->ftype == 102);	/* SQLT_CUR */
	Newz(0, cda, 1, Cda_Def);
	if (oopen(cda, imp_dbh->lda, (text*)0, -1, -1, (text*)0, -1)) {
	    ora_error(sth, cda, cda->rc, "oopen error for cursor");
	    Safefree(cda);
	    return 0;
	}
	if (obndra(imp_sth->cda, (text *)phs->name, -1,
	    (ub1*)cda, (sword)-1, /* cast reduces max size */
	    (sword)phs->ftype, -1, 0, 0, &phs->arcode, 0, (ub4 *)0, (text *)0, -1, -1)
	) {
	    D_imp_dbh_from_sth;
	    ora_error(sth, imp_dbh->lda, imp_sth->cda->rc, "obndra failed for cursor");
	    Safefree(cda);
	    return 0;
	}
	phs->progv = (void*)cda;
	phs->maxlen = -1;
    }
#endif
	ENTER;
	PUSHMARK(SP);
	XPUSHs(sv_2mortal(newRV(DBIc_MY_H(imp_dbh))));
	XPUSHs(sv_2mortal(newRV((SV*)init_attr)));
	PUTBACK;
	count = perl_call_pv("DBI::_new_sth", G_ARRAY);
	SPAGAIN;
	if (count != 2)
	    croak("panic: DBI::_new_sth returned %d values instead of 2", count);
	sth_i = SvREFCNT_inc(POPs);
	sv_setsv(phs->sv, SvREFCNT_inc(POPs));	/* outer handle */
	PUTBACK;
	LEAVE;
	if (DBIS->debug >= 3)
	    fprintf(DBILOGFP, "       bind %s - allocated %s...\n",
		phs->name, neatsvpv(phs->sv, 0));

    }
    else {		/* post-execute - setup the statement handle */
	dTHR;
	SV * sth_csr = phs->sv;
	D_impdata(imp_sth_csr, imp_sth_t, sth_csr);

	if (DBIS->debug >= 3)
	    fprintf(DBILOGFP, "       bind %s - initialising new %s for cursor 0x%lx...\n",
		phs->name, neatsvpv(sth_csr,0), (unsigned long)phs->progv);

#ifdef OCI_V8_SYNTAX
	/* copy appropriate handles from parent statement	*/
	imp_sth_csr->envhp = imp_sth->envhp;
	imp_sth_csr->errhp = imp_sth->errhp;
	imp_sth_csr->srvhp = imp_sth->srvhp;
	imp_sth_csr->svchp = imp_sth->svchp;

	/* assign statement handle from placeholder descriptor	*/
	imp_sth_csr->stmhp = phs->desc_h;
	imp_sth_csr->disable_finish = 1;  /* else finish core dumps in kpuccan()! */
	phs->desc_h = NULL;		  /* tell phs that we own it now	*/

	/* force stmt_type since OCIAttrGet(OCI_ATTR_STMT_TYPE) doesn't work! */
	imp_sth_csr->stmt_type = OCI_STMT_SELECT;
#else

	imp_sth_csr->cda = (void*)phs->progv;
	imp_sth_csr->cda->ft = 4;	/* persuade dbd_describe it's a SELECT	*/
	phs->progv = NULL;		/* tell phs that we own it now		*/

#endif

	DBIc_IMPSET_on(imp_sth);

	/* set ACTIVE so dbd_describe doesn't do explicit OCI describe */
	DBIc_ACTIVE_on(imp_sth_csr);
	if (!dbd_describe(sth_csr, imp_sth_csr)) {
	    return 0;
	}
#ifndef OCI_V8_SYNTAX
	imp_sth_csr->cda->rpc= 0;	/* nothing already fetched into cache	*/
#endif
    }
    return 1;
}


#ifndef OCI_V8_SYNTAX
static int 
dbd_rebind_ph_cursor(sth, imp_sth, phs) 
    SV *sth;
    imp_sth_t *imp_sth;
    phs_t *phs;
{
    assert(phs->ftype == 102);
    phs->out_prepost_exec = pp_exec_rset;
    if (dbis->debug >= 3)
 	fprintf(DBILOGFP, "       bind %s to cursor (at execute)\n", phs->name);
    return 2;
}
#endif




static int 
dbd_rebind_ph(sth, imp_sth, phs) 
    SV *sth;
    imp_sth_t *imp_sth;
    phs_t *phs;
{
    ub2 *alen_ptr = NULL;
	int done = 0;

    switch (phs->ftype) {
#ifdef OCI_V8_SYNTAX
    case SQLT_CLOB:
    case SQLT_BLOB:
	    done = dbd_rebind_ph_lob(sth, imp_sth, phs);
	    break;
    case SQLT_RSET:
	    done = dbd_rebind_ph_rset(sth, imp_sth, phs);
	    break;
#else
    case 102:	/* SQLT_CUR */
	    done = dbd_rebind_ph_cursor(sth, imp_sth, phs);
	    break;
#endif
    default:
	    done = dbd_rebind_ph_char(sth, imp_sth, phs, &alen_ptr);
    }
    if (done != 1) {
	if (done == 2) { /* the rebind did the OCI bind call itself successfully */
	    if (dbis->debug >= 3)
		fprintf(DBILOGFP, "       bind %s done for ftype %d\n",
			phs->name, phs->ftype);
	    return 1;
	}
	return 0;	 /* the rebind failed	*/
    }

#ifdef OCI_V8_SYNTAX
    if (phs->maxlen > phs->maxlen_bound) {
	sword status;
	int at_exec = (phs->desc_h == NULL);
	OCIBindByName_log_stat(imp_sth->stmhp, &phs->bndhp, imp_sth->errhp,
		(text*)phs->name, strlen(phs->name),
		phs->progv,
		phs->maxlen ? (sb4)phs->maxlen : 1,	/* else bind "" fails	*/
		phs->ftype, &phs->indp,
		NULL,	/* ub2 *alen_ptr not needed with OCIBindDynamic */
		&phs->arcode,
		0,		/* max elements that can fit in allocated array	*/
		NULL,	/* (ptr to) current number of elements in array	*/
		at_exec ? OCI_DATA_AT_EXEC : OCI_DEFAULT,
		status
	);
	if (status != OCI_SUCCESS) {
	    oci_error(sth, imp_sth->errhp, status, "OCIBindByName");
	    return 0;
	}
	if (at_exec) {
	    OCIBindDynamic_log(phs->bndhp, imp_sth->errhp, (dvoid *)phs, dbd_phs_in,
				(dvoid *)phs, dbd_phs_out, status);
	    if (status != OCI_SUCCESS) {
		oci_error(sth, imp_sth->errhp, status, "OCIBindByName");
		return 0;
	    }
	}
    }

#else
    /* Since we don't support LONG VAR types we must check	*/
    /* for lengths too big to pass to obndrv as an sword.	*/
    if (phs->maxlen > MINSWORDMAXVAL && sizeof(sword)<4)	/* generally 32K	*/
	croak("Can't bind %s, value is too long (%ld bytes, max %d)",
		    phs->name, phs->maxlen, MINSWORDMAXVAL);

    if (obndra(imp_sth->cda, (text *)phs->name, -1,
	    (ub1*)phs->progv, (sword)phs->maxlen, /* cast reduces max size */
	    (sword)phs->ftype, -1,
	    &phs->indp, alen_ptr, &phs->arcode, 0, (ub4 *)0,
	    (text *)0, -1, -1)) {
	D_imp_dbh_from_sth;
	ora_error(sth, imp_dbh->lda, imp_sth->cda->rc, "obndra failed");
	return 0;
    }
#endif
    phs->maxlen_bound = phs->maxlen ? phs->maxlen : 1;
    if (dbis->debug >= 3)
 	fprintf(DBILOGFP, "       bind %s done\n", phs->name);
    return 1;
}


int
dbd_bind_ph(sth, imp_sth, ph_namesv, newvalue, sql_type, attribs, is_inout, maxlen)
    SV *sth;
    imp_sth_t *imp_sth;
    SV *ph_namesv;
    SV *newvalue;
    IV sql_type;
    SV *attribs;
    int is_inout;
    IV maxlen;
{
    SV **phs_svp;
    STRLEN name_len;
    char *name = Nullch;
    char namebuf[30];
    phs_t *phs;

    /* check if placeholder was passed as a number	*/

    if (SvGMAGICAL(ph_namesv))	/* eg if from tainted expression */
	mg_get(ph_namesv);
    if (!SvNIOKp(ph_namesv)) {
	name = SvPV(ph_namesv, name_len);
    }
    if (SvNIOKp(ph_namesv) || (name && isDIGIT(name[0]))) {
	sprintf(namebuf, ":p%d", (int)SvIV(ph_namesv));
	name = namebuf;
	name_len = strlen(name);
    }
    assert(name != Nullch);

    if (SvTYPE(newvalue) > SVt_PVLV) /* hook for later array logic	*/
	croak("Can't bind a non-scalar value (%s)", neatsvpv(newvalue,0));
    if (SvROK(newvalue) && !IS_DBI_HANDLE(newvalue))
	/* dbi handle allowed for cursor variables */
	croak("Can't bind a reference (%s)", neatsvpv(newvalue,0));
    if (SvTYPE(newvalue) == SVt_PVLV && is_inout)	/* may allow later */
	croak("Can't bind ``lvalue'' mode scalar as inout parameter (currently)");

    if (dbis->debug >= 2) {
	fprintf(DBILOGFP, "       bind %s <== %s (type %ld",
		name, neatsvpv(newvalue,0), (long)sql_type);
	if (is_inout)
	    fprintf(DBILOGFP, ", inout 0x%lx, maxlen %ld",
		(long)newvalue, (long)maxlen);
	if (attribs)
	    fprintf(DBILOGFP, ", attribs: %s", neatsvpv(attribs,0));
	fprintf(DBILOGFP, ")\n");
    }

    phs_svp = hv_fetch(imp_sth->all_params_hv, name, name_len, 0);
    if (phs_svp == NULL)
	croak("Can't bind unknown placeholder '%s' (%s)", name, neatsvpv(ph_namesv,0));
    phs = (phs_t*)(void*)SvPVX(*phs_svp);	/* placeholder struct	*/

    if (phs->sv == &sv_undef) {	/* first bind for this placeholder	*/
	phs->ftype    = 1;		/* our default type: VARCHAR2	*/
	phs->is_inout = is_inout;
	if (is_inout) {
	    /* phs->sv assigned in the code below */
	    ++imp_sth->has_inout_params;
	    /* build array of phs's so we can deal with out vars fast	*/
	    if (!imp_sth->out_params_av)
		imp_sth->out_params_av = newAV();
	    av_push(imp_sth->out_params_av, SvREFCNT_inc(*phs_svp));
	}

	if (attribs) {	/* only look for ora_type on first bind of var	*/
	    SV **svp;
	    /* Setup / Clear attributes as defined by attribs.		*/
	    /* XXX If attribs is EMPTY then reset attribs to default?	*/
	    if ( (svp=hv_fetch((HV*)SvRV(attribs), "ora_type",8, 0)) != NULL) {
		int ora_type = SvIV(*svp);
		if (!oratype_bind_ok(ora_type))
		    croak("Can't bind %s, ora_type %d not supported by DBD::Oracle",
			    phs->name, ora_type);
		if (sql_type)
		    croak("Can't specify both TYPE (%d) and ora_type (%d) for %s",
			    sql_type, ora_type, phs->name);
		phs->ftype = ora_type;
	    }
	    if ( (svp=hv_fetch((HV*)SvRV(attribs), "ora_field",9, 0)) != NULL) {
		phs->ora_field = SvREFCNT_inc(*svp);
	    }
	}
	if (sql_type)
	    phs->ftype = ora_sql_type(imp_sth, phs->name, sql_type);

#ifndef OCI_V8_SYNTAX
	/* treat Oracle8 LOBS as simple LONGs for Oracle7 	*/
	if (phs->ftype==112 || phs->ftype==113)
	    phs->ftype = 8;
	/* treat Oracle8 SQLT_RSET as SQLT_CUR for Oracle7	*/
	if (phs->ftype==116)
	    phs->ftype = 102;
#else
	/* treat Oracle7 SQLT_CUR as SQLT_RSET for Oracle8	*/
	if (phs->ftype==102)
	    phs->ftype = 116;
#endif

	/* some types require the trailing null included in the length.	*/
	phs->alen_incnull = (phs->ftype==SQLT_STR || phs->ftype==SQLT_AVC);

    }	/* was first bind for this placeholder  */

	/* check later rebinds for any changes */
    else if (is_inout != phs->is_inout) {
	croak("Can't rebind or change param %s in/out mode after first bind (%d => %d)",
		phs->name, phs->is_inout , is_inout);
    }
    else if (sql_type && phs->ftype != ora_sql_type(imp_sth, phs->name, sql_type)) {
	croak("Can't change TYPE of param %s to %d after initial bind",
		phs->name, sql_type);
    }

    phs->maxlen = maxlen;		/* 0 if not inout		*/

    if (!is_inout) {	/* normal bind to take a (new) copy of current value	*/
	if (phs->sv == &sv_undef)	/* (first time bind) */
	    phs->sv = newSV(0);
	sv_setsv(phs->sv, newvalue);
    }
    else if (newvalue != phs->sv) {
	if (phs->sv)
	    SvREFCNT_dec(phs->sv);
	phs->sv = SvREFCNT_inc(newvalue);	/* point to live var	*/
    }

    return dbd_rebind_ph(sth, imp_sth, phs);
}


int
dbd_st_execute(sth, imp_sth)	/* <= -2:error, >=0:ok row count, (-1=unknown count) */
    SV *sth;
    imp_sth_t *imp_sth;
{
    dTHR;
    ub4 row_count = 0;
    int debug = dbis->debug;
    int outparams = (imp_sth->out_params_av) ? AvFILL(imp_sth->out_params_av)+1 : 0;

#ifdef OCI_V8_SYNTAX
    D_imp_dbh_from_sth;
    sword status;
    int is_select = (imp_sth->stmt_type == OCI_STMT_SELECT);

    if (debug >= 2)
	fprintf(DBILOGFP, "    dbd_st_execute %s (out%d, lob%d)...\n",
	    oci_stmt_type_name(imp_sth->stmt_type), outparams, imp_sth->has_lobs);
#else

    if (!imp_sth->done_desc) {
	/* describe and allocate storage for results (if any needed)	*/
	if (!dbd_describe(sth, imp_sth))
	    return -2; /* dbd_describe already called ora_error()	*/
    }
    if (debug >= 2)
	fprintf(DBILOGFP,
	    "    dbd_st_execute (for sql f%d after oci f%d, out%d)...\n",
		imp_sth->cda->ft, imp_sth->cda->fc, outparams);
#endif

    if (outparams) {	/* check validity of bind_param_inout SV's	*/
	int i = outparams;
	while(--i >= 0) {
	    phs_t *phs = (phs_t*)(void*)SvPVX(AvARRAY(imp_sth->out_params_av)[i]);
	    /* Make sure we have the value in string format. Typically a number	*/
	    /* will be converted back into a string using the same bound buffer	*/
	    /* so the progv test below will not trip.			*/

	    /* is the value a null? */
	    phs->indp = (SvOK(phs->sv)) ? 0 : -1;

	    /* Some checks for mutated storage since we pointed oracle at it.	*/
	    if (phs->out_prepost_exec) {
		if (!phs->out_prepost_exec(sth, imp_sth, phs, 1))
		    return -2; /* out_prepost_exec already called ora_error()	*/
	    }
	    else
	    if (SvTYPE(phs->sv) != phs->sv_type
		    || (SvOK(phs->sv) && !SvPOK(phs->sv))
		    /* SvROK==!SvPOK so cursor (SQLT_CUR) handle will call dbd_rebind_ph */
		    /* that suits us for now */
		    || SvPVX(phs->sv) != phs->progv
		    || (SvPOK(phs->sv) && SvCUR(phs->sv) > UB2MAXVAL)
	    ) {
		if (!dbd_rebind_ph(sth, imp_sth, phs))
		    croak("Can't rebind placeholder %s", phs->name);
	    }
	    else {
 		/* String may have grown or shrunk since it was bound	*/
 		/* so tell Oracle about it's current length		*/
		phs->alen = SvCUR(phs->sv) + phs->alen_incnull;
		if (debug >= 2)
 		    fprintf(DBILOGFP,
 		        "      with %s = '%.*s' (len %ld/%ld, indp %d, otype %d, ptype %d)\n",
 			phs->name, (int)phs->alen,
			(phs->indp == -1) ? "" : SvPVX(phs->sv),
			(long)phs->alen, (long)phs->maxlen, phs->indp,
			phs->ftype, (int)SvTYPE(phs->sv));
	    }
	}
    }

#ifdef OCI_V8_SYNTAX

    OCIStmtExecute_log_stat(imp_sth->svchp, imp_sth->stmhp, imp_sth->errhp,
		(is_select) ? 0 : 1,
		0, 0, 0,
		/* we don't AutoCommit on select so LOB locators work */
		(DBIc_has(imp_dbh,DBIcf_AutoCommit) && !is_select)
			? OCI_COMMIT_ON_SUCCESS : OCI_DEFAULT,
		status);
    if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO) {
	oci_error(sth, imp_sth->errhp, status,
		ora_sql_error(imp_sth,"OCIStmtExecute"));
	return -2;
    }
    if (is_select) {
	DBIc_ACTIVE_on(imp_sth);
	DBIc_ROW_COUNT(imp_sth) = 0; /* reset (possibly re-exec'ing) */
	row_count = 0;
    }
    else {
	OCIAttrGet_stmhp_stat(imp_sth, &row_count, 0, OCI_ATTR_ROW_COUNT, status);
    }

    if (debug >= 2) {
	ub2 sqlfncode;
	OCIAttrGet_stmhp_stat(imp_sth, &sqlfncode, 0, OCI_ATTR_SQLFNCODE, status);
	fprintf(DBILOGFP,
	    "    dbd_st_execute %s returned (%s, rpc%ld, fn%d, out%d)\n",
		oci_stmt_type_name(imp_sth->stmt_type),
		oci_status_name(status),
		row_count, sqlfncode, imp_sth->has_inout_params);
    }

    if (is_select && !imp_sth->done_desc) {
	/* describe and allocate storage for results (if any needed)	*/
	if (!dbd_describe(sth, imp_sth))
	    return -2; /* dbd_describe already called oci_error()	*/
    }
    if (imp_sth->has_lobs && imp_sth->stmt_type != OCI_STMT_SELECT) {
	if (!post_execute_lobs(sth, imp_sth, row_count))
	    return -2; /* post_insert_lobs already called oci_error()	*/
    }

#else

    /* reset cache counters */
    imp_sth->in_cache   = 0;
    imp_sth->next_entry = 0;
    imp_sth->eod_errno  = 0;

    /* Trigger execution of the statement */
    if (DBIc_NUM_FIELDS(imp_sth) > 0) {  	/* is a SELECT	*/
	/* The number of fields is used because imp_sth->cda->ft is unreliable.	*/
	/* Specifically an update (5) may change to select (4) after odesc().	*/
	if (oexfet(imp_sth->cda, (ub4)imp_sth->cache_rows, 0, 0)
		&& imp_sth->cda->rc != 1403 /* other than no more data */ ) {
	    ora_error(sth, imp_sth->cda, imp_sth->cda->rc, "oexfet error");
	    return -2;
	}
	DBIc_ACTIVE_on(imp_sth);
	imp_sth->in_cache = imp_sth->cda->rpc;	/* cache loaded */
	if (imp_sth->cda->rc == 1403)
	    imp_sth->eod_errno = 1403;
    }
    else {					/* NOT a select */
	if (oexec(imp_sth->cda)) {
	    char *msg = "oexec error";
	    switch(imp_sth->cda->rc) {
	    case 3108:
		msg = "perhaps you're using Oracle 8 functionality but this DBD::Oracle was built for Oracle 7";
		break;
	    }
	    ora_error(sth, imp_sth->cda, imp_sth->cda->rc, msg);
	    return -2;
	}
    }
    row_count = imp_sth->cda->rpc;

    if (debug >= 2)
	fprintf(DBILOGFP,
	    "    dbd_st_execute complete (rc%d, w%02x, rpc%ld, eod%d, out%d)\n",
		imp_sth->cda->rc,  imp_sth->cda->wrn,
		row_count, imp_sth->eod_errno,
		imp_sth->has_inout_params);
#endif

    if (outparams) {	/* check validity of bound output SV's	*/
	int i = outparams;
	while(--i >= 0) {
	    phs_t *phs = (phs_t*)(void*)SvPVX(AvARRAY(imp_sth->out_params_av)[i]);
	    SV *sv = phs->sv;

	    if (phs->out_prepost_exec) {
		if (!phs->out_prepost_exec(sth, imp_sth, phs, 0))
		    return -2; /* out_prepost_exec already called ora_error()	*/
	    }
	    else
 	    /* phs->alen has been updated by Oracle to hold the length of the result	*/
	    if (phs->indp == 0) {			/* is okay	*/
		SvPOK_only(sv);
		SvCUR(sv) = phs->alen;
		*SvEND(sv) = '\0';
		if (debug >= 2)
		    fprintf(DBILOGFP,
			"       out %s = '%s'\t(len %ld, arcode %d)\n",
			phs->name, SvPV(sv,na), (long)phs->alen, phs->arcode);
	    }
	    else
	    if (phs->indp > 0 || phs->indp == -2) {	/* truncated	*/
		SvPOK_only(sv);
		SvCUR(sv) = phs->alen;
		*SvEND(sv) = '\0';
		if (debug >= 2)
		    fprintf(DBILOGFP,
			"       out %s = '%s'\t(TRUNCATED from %d to %ld, arcode %d)\n",
			phs->name, SvPV(sv,na), phs->indp, (long)phs->alen, phs->arcode);
	    }
	    else
	    if (phs->indp == -1) {			/* is NULL	*/
		(void)SvOK_off(phs->sv);
		if (debug >= 2)
		    fprintf(DBILOGFP,
			"       out %s = undef (NULL, arcode %d)\n",
			phs->name, phs->arcode);
	    }
	    else croak("panic: %s bad indp %d, arcode %d",
		phs->name, phs->indp, phs->arcode);
	}
    }

    return row_count;	/* row count (0 will be returned as "0E0")	*/
}




int
dbd_st_blob_read(sth, imp_sth, field, offset, len, destrv, destoffset)
    SV *sth;
    imp_sth_t *imp_sth;
    int field;
    long offset;
    long len;
    SV *destrv;
    long destoffset;
{
    ub4 retl = 0;
    SV *bufsv;
    imp_fbh_t *fbh = &imp_sth->fbh[field];
    int ftype = fbh->ftype;

    bufsv = SvRV(destrv);
    sv_setpvn(bufsv,"",0);	/* ensure it's writable string	*/
    SvGROW(bufsv, destoffset+len+1);	/* SvGROW doesn't do +1	*/

#ifdef OCI_V8_SYNTAX
    retl = ora_blob_read_piece(sth, imp_sth, fbh, bufsv,
				 offset, len, destoffset);
    if (!SvOK(bufsv))	/* ora_blob_read_piece recorded error */
	return 0;
    ftype = ftype;	/* no unused */

#else

    if (len > 65535) {
	warn("Oracle OCI7 doesn't allow blob_read to reliably fetch chunks longer than 65535 bytes");
	len = 65535;
    }

    switch (fbh->ftype) {
	case 94: ftype =  8;	break;
	case 95: ftype = 24;	break;
    }

    /* The +1 on field was a mistake that's too late to fix :-(	*/
    if (oflng(imp_sth->cda, (sword)field+1,
	    ((ub1*)SvPVX(bufsv)) + destoffset, len,
	    ftype, &retl, offset)) {
	ora_error(sth, imp_sth->cda, imp_sth->cda->rc, "oflng error");
	/* XXX database may have altered the buffer contents	*/
	return 0;
    }
#endif

    if (dbis->debug >= 3)
	fprintf(DBILOGFP,
	    "    blob_read field %d+1, ftype %d, offset %ld, len %ld, destoffset %ld, retlen %ld\n",
	    field, imp_sth->fbh[field].ftype, offset, len, destoffset, retl);

    SvCUR_set(bufsv, destoffset+retl);

    *SvEND(bufsv) = '\0'; /* consistent with perl sv_setpvn etc	*/

    return 1;
}


int
dbd_st_rows(sth, imp_sth)
    SV *sth;
    imp_sth_t *imp_sth;
{
#ifdef OCI_V8_SYNTAX
    ub4 row_count = 0;
    sword status;
    OCIAttrGet_stmhp_stat(imp_sth, &row_count, 0, OCI_ATTR_ROW_COUNT, status);
    if (status != OCI_SUCCESS) {
	oci_error(sth, imp_sth->errhp, status, "OCIAttrGet OCI_ATTR_ROW_COUNT");
	return -1;
    }
    return row_count;
#else
    /* spot common mistake of checking $h->rows just after ->execute	*/
    if (   imp_sth->in_cache > 0		 /* has unfetched rows	*/
	&& imp_sth->in_cache== imp_sth->cda->rpc /* NO rows fetched yet	*/
	&& DBIc_WARN(imp_sth)	/* provide a way to disable warning	*/
    ) {
	warn("$h->rows count is incomplete before all rows fetched.\n");
    }
    /* imp_sth->in_cache should always be 0 for non-select statements	*/
    return imp_sth->cda->rpc - imp_sth->in_cache;	/* fetched rows	*/
#endif
}


int
dbd_st_finish(sth, imp_sth)
    SV *sth;
    imp_sth_t *imp_sth;
{
    dTHR;
    D_imp_dbh_from_sth;

    if (!DBIc_ACTIVE(imp_sth))
	return 1;

    /* Cancel further fetches from this cursor.                 */
    /* We don't close the cursor till DESTROY (dbd_st_destroy). */
    /* The application may re execute(...) it.                  */

    /* Turn off ACTIVE here regardless of errors below.		*/
    DBIc_ACTIVE_off(imp_sth);

    if (dirty)			/* don't walk on the wild side	*/
	return 1;

    if (imp_sth->disable_finish)	/* see ref cursors	*/
	return 1;

    if (!DBIc_ACTIVE(imp_dbh))		/* no longer connected	*/
	return 1;

#ifdef OCI_V8_SYNTAX
{   sword status;
    OCIStmtFetch_log_stat(imp_sth->stmhp, imp_sth->errhp, 0, OCI_FETCH_NEXT,
                          OCI_DEFAULT, status);
    if (status != OCI_SUCCESS) {
	oci_error(sth, imp_sth->errhp, status, "Finish OCIStmtFetch");
	return 0;
    }
}
#else
    if (ocan(imp_sth->cda)) {
	/* oracle 7.3 code can core dump looking up an error message	*/
	/* if we have logged out of the database. This typically	*/
	/* happens during global destruction. This should catch most:	*/
	if (dirty && imp_sth->cda->rc == 3114)
	    ora_error(sth, NULL, imp_sth->cda->rc,
		"ORA-03114: not connected to ORACLE (ocan)");
	else
	    ora_error(sth, imp_sth->cda, imp_sth->cda->rc, "ocan error");
	return 0;
    }
#endif
    return 1;
}


void
ora_free_fbh_contents(fbh)
    imp_fbh_t *fbh;
{
    if (fbh->fb_ary)
	fb_ary_free(fbh->fb_ary);
    sv_free(fbh->name_sv);
#ifdef OCI_V8_SYNTAX
    if (fbh->desc_h)
	OCIDescriptorFree_log(fbh->desc_h, fbh->desc_t);
#endif
}

void
ora_free_phs_contents(phs)
    phs_t *phs;
{
#ifdef OCI_V8_SYNTAX
    if (phs->desc_h)
	OCIDescriptorFree_log(phs->desc_h, phs->desc_t);
#else
    if (phs->ftype == 102 && phs->progv) {	/* SQLT_CUR */
	/* should not normally happen since new child sth takes	*/
	/* ownership of the cursor and sets phs->progv to NULL.	*/
	oclose((Cda_Def*)phs->progv);
	Safefree(phs->progv);
	phs->progv = NULL;
    }
#endif
    sv_free(phs->ora_field);
    sv_free(phs->sv);
}


void
dbd_st_destroy(sth, imp_sth)
    SV *sth;
    imp_sth_t *imp_sth;
{
    D_imp_dbh_from_sth;
    int fields;
    int i;

    /* Check if an explicit disconnect() or global destruction has	*/
    /* disconnected us from the *database* before attempting to close.	*/

    if (DBIc_ACTIVE(imp_dbh)) {
	/* dbd_st_finish has already been called by .xs code if needed.	*/
	/* so this should never get called */
    } 

#ifdef OCI_V8_SYNTAX
    {
	sword status;
	if (imp_sth->lob_refetch)
	    ora_free_lob_refetch(sth, imp_sth);
	OCIHandleFree_log_stat(imp_sth->stmhp, OCI_HTYPE_STMT, status);
	if (status != OCI_SUCCESS)
	    oci_error(sth, imp_sth->errhp, status, "OCIHandleFree");
    }
#else
    oclose(imp_sth->cda);
    if (imp_sth->cda != &imp_sth->cdabuf) {
	/* we assume that the cda was allocated for a ref cursor	*/
	/* bound to a placeholder on a different statement.		*/
	/* We own the cda buffer now so we need to free it.		*/
	Safefree(imp_sth->cda);
    }
    imp_sth->cda = NULL;
#endif

    /* Free off contents of imp_sth	*/

    fields = DBIc_NUM_FIELDS(imp_sth);
    imp_sth->in_cache  = 0;
    imp_sth->eod_errno = 1403;
    for(i=0; i < fields; ++i) {
	imp_fbh_t *fbh = &imp_sth->fbh[i];
	ora_free_fbh_contents(fbh);
    }
    Safefree(imp_sth->fbh);
    Safefree(imp_sth->statement);

    if (imp_sth->out_params_av)
	sv_free((SV*)imp_sth->out_params_av);

    if (imp_sth->all_params_hv) {
	HV *hv = imp_sth->all_params_hv;
	SV *sv;
	char *key;
	I32 retlen;
	hv_iterinit(hv);
	while( (sv = hv_iternextsv(hv, &key, &retlen)) != NULL ) {
	    if (sv != &sv_undef) {
		phs_t *phs = (phs_t*)(void*)SvPVX(sv);
		ora_free_phs_contents(phs);
	    }
	}
	sv_free((SV*)imp_sth->all_params_hv);
    }

    DBIc_IMPSET_off(imp_sth);		/* let DBI know we've done it	*/
}


int
dbd_st_STORE_attrib(sth, imp_sth, keysv, valuesv)
    SV *sth;
    imp_sth_t *imp_sth;
    SV *keysv;
    SV *valuesv;
{
    STRLEN kl;
    SV *cachesv = NULL;
    char *key = SvPV(keysv,kl);
/*
    int on = SvTRUE(valuesv);
    int oraperl = DBIc_COMPAT(imp_sth); */

    if (strEQ(key, "ora_fetchtest")) {
	ora_fetchtest = SvIV(valuesv);
    }
    else
	return FALSE;

    if (cachesv) /* cache value for later DBI 'quick' fetch? */
	hv_store((HV*)SvRV(sth), key, kl, cachesv, 0);
    return TRUE;
}


SV *
dbd_st_FETCH_attrib(sth, imp_sth, keysv)
    SV *sth;
    imp_sth_t *imp_sth;
    SV *keysv;
{
    STRLEN kl;
    char *key = SvPV(keysv,kl);
    int i;
    SV *retsv = NULL;
    /* Default to caching results for DBI dispatch quick_FETCH	*/
    int cacheit = TRUE;
    /* int oraperl = DBIc_COMPAT(imp_sth); */

    if (kl==13 && strEQ(key, "NUM_OF_PARAMS"))	/* handled by DBI */
	return Nullsv;	

    if (!imp_sth->done_desc && !dbd_describe(sth, imp_sth)) {
	STRLEN lna;
	/* dbd_describe has already called ora_error()		*/
	/* we can't return Nullsv here because the xs code will	*/
	/* then just pass the attribute name to DBI for FETCH.	*/
	croak("Describe failed during %s->FETCH(%s): %ld: %s",
		SvPV(sth,na), key, (long)SvIV(DBIc_ERR(imp_sth)),
		SvPV(DBIc_ERRSTR(imp_sth),lna)
	);
    }

    i = DBIc_NUM_FIELDS(imp_sth);

    if (kl==11 && strEQ(key, "ora_lengths")) {
	AV *av = newAV();
	retsv = newRV(sv_2mortal((SV*)av));
	while(--i >= 0)
	    av_store(av, i, newSViv((IV)imp_sth->fbh[i].disize));

    } else if (kl==9 && strEQ(key, "ora_types")) {
	AV *av = newAV();
	retsv = newRV(sv_2mortal((SV*)av));
	while(--i >= 0)
	    av_store(av, i, newSViv(imp_sth->fbh[i].dbtype));

    } else if (kl==4 && strEQ(key, "TYPE")) {
	AV *av = newAV();
	retsv = newRV(sv_2mortal((SV*)av));
	while(--i >= 0)
	    av_store(av, i, newSViv(ora2sql_type(imp_sth->fbh[i].dbtype)));

    } else if (kl==5 && strEQ(key, "SCALE")) {
	AV *av = newAV();
	retsv = newRV(sv_2mortal((SV*)av));
	while(--i >= 0)
	    av_store(av, i, newSViv(imp_sth->fbh[i].scale));

    } else if (kl==9 && strEQ(key, "PRECISION")) {
	AV *av = newAV();
	retsv = newRV(sv_2mortal((SV*)av));
	while(--i >= 0)
	    av_store(av, i, newSViv(imp_sth->fbh[i].prec));

#ifndef OCI_V8_SYNTAX
#ifdef XXXXX
    } else if (kl==9 && strEQ(key, "ora_rowid")) {
	/* return current _binary_ ROWID (oratype 11) uncached	*/
	/* Use { ora_type => 11 } when binding to a placeholder	*/
	retsv = newSVpv((char*)&imp_sth->cda->rid, sizeof(imp_sth->cda->rid));
	cacheit = FALSE;
#endif
#endif

    } else if (kl==17 && strEQ(key, "ora_est_row_width")) {
	retsv = newSViv(imp_sth->est_width);
	cacheit = TRUE;

    } else if (kl==4 && strEQ(key, "NAME")) {
	AV *av = newAV();
	retsv = newRV(sv_2mortal((SV*)av));
	while(--i >= 0)
	    av_store(av, i, newSVpv((char*)imp_sth->fbh[i].name,0));

    } else if (kl==8 && strEQ(key, "NULLABLE")) {
	AV *av = newAV();
	retsv = newRV(sv_2mortal((SV*)av));
	while(--i >= 0)
	    av_store(av, i, boolSV(imp_sth->fbh[i].nullok));

    } else {
	return Nullsv;
    }
    if (cacheit) { /* cache for next time (via DBI quick_FETCH)	*/
	SV **svp = hv_fetch((HV*)SvRV(sth), key, kl, 1);
	sv_free(*svp);
	*svp = retsv;
	(void)SvREFCNT_inc(retsv);	/* so sv_2mortal won't free it	*/
    }
    return sv_2mortal(retsv);
}

/* --------------------------------------- */

static int
ora2sql_type(oratype)
   int oratype;
{
    switch(oratype) {	/* oracle Internal (not external) types */
    case SQLT_CHR:  return SQL_VARCHAR;
    case SQLT_NUM:  return SQL_DECIMAL;
    case SQLT_LNG:  return SQL_LONGVARCHAR;	/* long */
    case SQLT_DAT:  return SQL_DATE;
    case SQLT_BIN:  return SQL_BINARY;		/* raw */
    case SQLT_LBI:  return SQL_LONGVARBINARY;	/* long raw */
    case SQLT_AFC:  return SQL_CHAR;		/* Ansi fixed char */
    }
    /* else map type into DBI reserved standard range */
    return -9000 - oratype;
}

static void
dump_env_to_trace() {
    FILE *fp = DBILOGFP;
    int i = 0;
    char *p;
    extern char **environ;
    fprintf(fp, "Environment variables:\n");
    do {
	p = (char*)environ[i++];
	fprintf(fp,"\t%s\n",p);
    } while ((char*)environ[i] != '\0');
}
