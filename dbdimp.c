/*
   $Id: dbdimp.c,v 1.28 1996/10/29 18:17:23 timbo Exp $

   Copyright (c) 1994,1995  Tim Bunce

   You may distribute under the terms of either the GNU General Public
   License or the Artistic License, as specified in the Perl README file.

*/

#include "Oracle.h"


DBISTATE_DECLARE;

static SV *ora_long;
static SV *ora_trunc;
static SV *ora_cache;
static SV *ora_cache_o;	/* temp hack for ora_open() cache override */


void
dbd_init(dbistate)
    dbistate_t *dbistate;
{
    DBIS = dbistate;
    ora_long    = perl_get_sv("Oraperl::ora_long",    GV_ADDMULTI);
    ora_trunc   = perl_get_sv("Oraperl::ora_trunc",   GV_ADDMULTI);
    ora_cache   = perl_get_sv("Oraperl::ora_cache",   GV_ADDMULTI);
    ora_cache_o = perl_get_sv("Oraperl::ora_cache_o", GV_ADDMULTI);
}


/* Database specific error handling.
	This will be split up into specific routines
	for dbh and sth level.
	Also split into helper routine to set number & string.
	Err, many changes needed, ramble ...
*/

void
ora_error(h, lda, rc, what)
    SV *h;
    Lda_Def *lda;
    sb2	rc;
    char *what;
{
    D_imp_xxh(h);
    SV *errstr = DBIc_ERRSTR(imp_xxh);
    if (lda) {	/* is oracle error (allow for non-oracle errors) */
	int len;
	char msg[1024];
	/* Oracle oerhms can do duplicate free if connect fails */
	/* Ignore 'with different width due to prototype' gcc warning	*/
	oerhms(lda, rc, (text*)msg, sizeof(msg));
	len = strlen(msg);
	if (len && msg[len-1] == '\n')
	    msg[len-1] = '\0'; /* trim off \n from end of message */
	sv_setpv(errstr, (char*)msg);
    }
    else sv_setpv(errstr, what);
    sv_setiv(DBIc_ERR(imp_xxh), (IV)rc);
    if (what && lda) {
	sv_catpv(errstr, " (DBD: ");
	sv_catpv(errstr, what);
	sv_catpv(errstr, ")");
    }
    DBIh_EVENT2(h, ERROR_event, DBIc_ERR(imp_xxh), errstr);
    if (dbis->debug >= 2)
	fprintf(DBILOGFP, "%s error %d recorded: %s\n",
		what, rc, SvPV(errstr,na));
}


void
fbh_dump(fbh, i, aidx)
    imp_fbh_t *fbh;
    int i;
    int aidx;	/* array index */
{
    FILE *fp = DBILOGFP;
    fprintf(fp, "fbh %d: '%s' %s, ",
		i, fbh->cbuf, (fbh->nullok) ? "NULLable" : "");
    fprintf(fp, "type %d,  dbsize %ld, dsize %ld, p%d s%d\n",
	    fbh->dbtype, (long)fbh->dbsize, (long)fbh->dsize,
	    fbh->prec, fbh->scale);
    fprintf(fp, "   out: ftype %d, bufl %d. cache@%d: indp %d, rlen %d, rcode %d\n",
	    fbh->ftype, fbh->fb_ary->bufl, aidx, fbh->fb_ary->aindp[aidx],
	    fbh->fb_ary->arlen[aidx], fbh->fb_ary->arcode[aidx]);
}


static int
dbtype_is_long(dbtype)
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
dbtype_is_string(dbtype)	/* 'can we use SvPV to pass buffer?'	*/
    int dbtype;
{
    switch(dbtype) {
    case  1:	/* VARCHAR2	*/
    case  5:	/* STRING	*/
    case  8:	/* LONG		*/
    case 23:	/* RAW		*/
    case 24:	/* LONG RAW	*/
    case 96:	/* CHAR		*/
    case 97:	/* CHARZ	*/
    case 106:	/* MLSLABEL	*/
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



static void
dump_error_status(cda)
    struct cda_def *cda;
{
    fprintf(DBILOGFP,
	"(rc %ld, v2 %ld, ft %ld, rpc %ld, peo %ld, fc %ld, ose %ld)\n",
	(long)cda->rc, (long)cda->v2_rc, (long)cda->ft, (long)cda->rpc,
	(long)cda->peo, (long)cda->fc, (long)cda->ose
    );
}


/* ================================================================== */

int
dbd_db_login(dbh, dbname, uid, pwd)
    SV *dbh;
    char *dbname;
    char *uid;
    char *pwd;
{
    D_imp_dbh(dbh);
    int ret;

    /* can give duplicate free errors (from Oracle) if connect fails */
    ret = orlon(&imp_dbh->lda, imp_dbh->hda, (text*)uid,-1, (text*)pwd,-1,0);
    if (ret) {
	ora_error(dbh, &imp_dbh->lda, imp_dbh->lda.rc, "login failed");
	return 0;
    }
    DBIc_IMPSET_on(imp_dbh);	/* imp_dbh set up now			*/
    DBIc_ACTIVE_on(imp_dbh);	/* call disconnect before freeing	*/
    return 1;
}


int
dbd_db_commit(dbh)
    SV *dbh;
{
    D_imp_dbh(dbh);
    if (ocom(&imp_dbh->lda)) {
	ora_error(dbh, &imp_dbh->lda, imp_dbh->lda.rc, "commit failed");
	return 0;
    }
    return 1;
}

int
dbd_db_rollback(dbh)
    SV *dbh;
{
    D_imp_dbh(dbh);
    if (orol(&imp_dbh->lda)) {
	ora_error(dbh, &imp_dbh->lda, imp_dbh->lda.rc, "rollback failed");
	return 0;
    }
    return 1;
}


int
dbd_db_disconnect(dbh)
    SV *dbh;
{
    D_imp_dbh(dbh);
    /* We assume that disconnect will always work	*/
    /* since most errors imply already disconnected.	*/
    DBIc_ACTIVE_off(imp_dbh);
    if (ologof(&imp_dbh->lda)) {
	ora_error(dbh, &imp_dbh->lda, imp_dbh->lda.rc, "disconnect error");
	return 0;
    }
    /* We don't free imp_dbh since a reference still exists	*/
    /* The DESTROY method is the only one to 'free' memory.	*/
    /* Note that statement objects may still exists for this dbh!	*/
    return 1;
}


void
dbd_db_destroy(dbh)
    SV *dbh;
{
    D_imp_dbh(dbh);
    if (DBIc_ACTIVE(imp_dbh))
	dbd_db_disconnect(dbh);
    /* Nothing in imp_dbh to be freed	*/
    DBIc_IMPSET_off(imp_dbh);
}


int
dbd_db_STORE(dbh, keysv, valuesv)
    SV *dbh;
    SV *keysv;
    SV *valuesv;
{
    D_imp_dbh(dbh);
    STRLEN kl;
    char *key = SvPV(keysv,kl);
    SV *cachesv = NULL;
    int on = SvTRUE(valuesv);

    if (kl==10 && strEQ(key, "AutoCommit")) {
	if ( (on) ? ocon(&imp_dbh->lda) : ocof(&imp_dbh->lda) ) {
	    ora_error(dbh, &imp_dbh->lda, imp_dbh->lda.rc, "ocon/ocof failed");
	    /* XXX um, we can't return FALSE and true isn't acurate */
	    /* the best we can do is cache an undef	*/
	    cachesv = &sv_undef;
	} else {
	    cachesv = (on) ? &sv_yes : &sv_no;	/* cache new state */
	}
    } else {
	return FALSE;
    }
    if (cachesv) /* cache value for later DBI 'quick' fetch? */
	hv_store((HV*)SvRV(dbh), key, kl, cachesv, 0);
    return TRUE;
}


SV *
dbd_db_FETCH(dbh, keysv)
    SV *dbh;
    SV *keysv;
{
    /* D_imp_dbh(dbh); */
    STRLEN kl;
    char *key = SvPV(keysv,kl);
    SV *retsv = NULL;
    /* Default to caching results for DBI dispatch quick_FETCH	*/
    int cacheit = TRUE;

    if (1) {		/* no attribs defined yet	*/
	return Nullsv;
    }
    if (cacheit) {	/* cache for next time (via DBI quick_FETCH)	*/
	SV **svp = hv_fetch((HV*)SvRV(dbh), key, kl, 1);
	sv_free(*svp);
	*svp = retsv;
	(void)SvREFCNT_inc(retsv);	/* so sv_2mortal won't free it	*/
    }
    return sv_2mortal(retsv);
}



/* ================================================================== */


int
dbd_st_prepare(sth, statement, attribs)
    SV *sth;
    char *statement;
    SV *attribs;
{
    D_imp_sth(sth);
    D_imp_dbh_from_sth;
    sword oparse_defer = 0;  /* PARSE_NO_DEFER */
    ub4   oparse_lng   = 1;  /* auto v6 or v7 as suits db connected to	*/

    imp_sth->done_desc = 0;
    imp_sth->cda = &imp_sth->cdabuf;

    if (attribs) {
	SV **svp;
	DBD_ATTRIB_GET_IV(  attribs, "ora_parse_lang", 14, svp, oparse_lng);
	DBD_ATTRIB_GET_BOOL(attribs, "ora_parse_defer",15, svp, oparse_defer);
    }

    if (oopen(imp_sth->cda, &imp_dbh->lda, (text*)0, -1, -1, (text*)0, -1)) {
        ora_error(sth, imp_sth->cda, imp_sth->cda->rc, "oopen error");
        return 0;
    }

    /* scan statement for '?', ':1' and/or ':foo' style placeholders	*/
    dbd_preparse(imp_sth, statement);

    /* parse the (possibly edited) SQL statement */
    /* Note that (if oparse_defer=0, the default) Data Definition	*/
    /* statements will be executed at once. This is a major pain!	*/
    imp_sth->cda->peo = 0;
    if (oparse(imp_sth->cda, (text*)imp_sth->statement, (sb4)-1,
                (sword)oparse_defer, (ub4)oparse_lng)) {
	SV  *msgsv, *sqlsv;
	char msg[99];
	sqlsv = sv_2mortal(newSVpv(imp_sth->statement,0));
	sv_insert(sqlsv, imp_sth->cda->peo, 0, "<*>",3);
	sprintf(msg,"error possibly near <*> indicator at char %d in '",
		imp_sth->cda->peo+1);
	msgsv = sv_2mortal(newSVpv(msg,0));
	sv_catsv(msgsv, sqlsv);
	sv_catpv(msgsv, "'");
	ora_error(sth, imp_sth->cda, imp_sth->cda->rc, SvPV(msgsv,na));
	oclose(imp_sth->cda);	/* close the cursor	*/
	return 0;
    }

    /* long_buflen:	length for long/longraw (if >0)  */
    /* long_trunc_ok:	is truncating a long an error    XXX not implemented */

    if (DBIc_COMPAT(imp_sth)) {		/* is an Oraperl handle		*/
	imp_sth->long_buflen   = SvIV(ora_long);
	/* ora_trunc is checked at fetch time */
    } else {
	imp_sth->long_buflen   = 80;	/* typical oracle default	*/
	imp_sth->long_trunc_ok = 1;	/* can use blob_read()		*/
    }

    if (dbis->debug >= 2)
	fprintf(DBILOGFP, "    dbd_st_prepare'd sql f%d (lb %ld, lt %d)\n",
	    imp_sth->cda->ft, (long)imp_sth->long_buflen, imp_sth->long_trunc_ok);

    /* Describe and allocate storage for results. This could	*/
    /* and possibly should be deferred until execution or some	*/
    /* output related information is fetched.			*/
/* was defered prior to 0.43 */
    if (!dbd_describe(sth, imp_sth)) {
	return 0;
    }
    DBIc_IMPSET_on(imp_sth);
    return 1;
}


void
dbd_preparse(imp_sth, statement)
    imp_sth_t *imp_sth;
    char *statement;
{
    bool in_literal = FALSE;
    char *src, *start, *dest;
    phs_t phs_tpl;
    SV *phs_sv;
    int idx=0, style=0, laststyle=0;
    STRLEN namelen;

    /* allocate room for copy of statement with spare capacity	*/
    /* for editing '?' or ':1' into ':p1' so we can use obndrv.	*/
    imp_sth->statement = (char*)safemalloc(strlen(statement) * 3);

    /* initialise phs ready to be cloned per placeholder	*/
    memset(&phs_tpl, 0, sizeof(phs_tpl));
    phs_tpl.ftype = 1;	/* VARCHAR2 */

    src  = statement;
    dest = imp_sth->statement;
    while(*src) {
	if (*src == '\'')
	    in_literal = ~in_literal;
	if ((*src != ':' && *src != '?') || in_literal) {
	    *dest++ = *src++;
	    continue;
	}
	start = dest;			/* save name inc colon	*/ 
	*dest++ = *src++;
	if (*start == '?') {		/* X/Open standard	*/
	    sprintf(start,":p%d", ++idx); /* '?' -> ':p1' (etc)	*/
	    dest = start+strlen(start);
	    style = 3;

	} else if (isDIGIT(*src)) {	/* ':1'		*/
	    idx = atoi(src);
	    *dest++ = 'p';		/* ':1'->':p1'	*/
	    if (idx <= 0)
		croak("Placeholder :%d must be a positive number", idx);
	    while(isDIGIT(*src))
		*dest++ = *src++;
	    style = 1;

	} else if (isALNUM(*src)) {	/* ':foo'	*/
	    while(isALNUM(*src))	/* includes '_'	*/
		*dest++ = *src++;
	    style = 2;
	} else {			/* perhaps ':=' PL/SQL construct */
	    continue;
	}
	*dest = '\0';			/* handy for debugging	*/
	namelen = (dest-start);
	if (laststyle && style != laststyle)
	    croak("Can't mix placeholder styles (%d/%d)",style,laststyle);
	laststyle = style;
	if (imp_sth->all_params_hv == NULL)
	    imp_sth->all_params_hv = newHV();
	phs_tpl.sv = &sv_undef;
	phs_sv = newSVpv((char*)&phs_tpl, sizeof(phs_tpl)+namelen+1);
	hv_store(imp_sth->all_params_hv, start, namelen, phs_sv, 0);
	strcpy( ((phs_t*)SvPVX(phs_sv))->name, start);
	/* warn("params_hv: '%s'\n", start);	*/
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
dbd_describe(h, imp_sth)
    SV *h;
    imp_sth_t *imp_sth;
{
    static sb4 *f_cbufl;		/* XXX not thread safe	*/
    static U32  f_cbufl_max;

    sb1 *cbuf_ptr;
    int t_cbufl=0;
    I32 num_fields;
    int has_longs = 0;
    int i = 0;

    if (imp_sth->done_desc)
	return 1;	/* success, already done it */
    imp_sth->done_desc = 1;

    if (imp_sth->cda->ft != FT_SELECT) {
	if (dbis->debug >= 2)
	    fprintf(DBILOGFP, "    dbd_describe skipped for non-select (sql f%d)\n",
		imp_sth->cda->ft);
	/* imp_sth memory was cleared when created so no setup required here	*/
	return 1;
    }

    if (dbis->debug >= 2)
	fprintf(DBILOGFP, "    dbd_describe (for sql f%d after oci f%d)...\n",
			imp_sth->cda->ft, imp_sth->cda->fc);

    if (!f_cbufl) {
	f_cbufl_max = 120;
	New(1, f_cbufl, f_cbufl_max, sb4);
    }

    /* Get number of fields and space needed for field names	*/
    while(++i) {	/* break out within loop		*/
	sb1 cbuf[257];	/* generous max column name length	*/
	sb2 dbtype = 0;	/* workaround for Oracle bug #405032	*/
	sb4 dbsize;
	if (i >= f_cbufl_max) {
	    f_cbufl_max *= 2;
	    Renew(f_cbufl, f_cbufl_max, sb4);
	}
	f_cbufl[i] = sizeof(cbuf);
	odescr(imp_sth->cda, i, &dbsize, &dbtype,
		cbuf, &f_cbufl[i], (sb4*)0, (sb2*)0, (sb2*)0, (sb2*)0);
        if (imp_sth->cda->rc || dbtype == 0)
	    break;
	t_cbufl  += f_cbufl[i];
	if (dbsize)
	    imp_sth->t_dbsize += dbsize;
	else if (dbtype_is_long(dbtype))
	    ++has_longs;	/* hint to auto cache sizing code	*/
    }
    if (imp_sth->cda->rc && imp_sth->cda->rc != 1007) {
	D_imp_dbh_from_sth;
	ora_error(h, &imp_dbh->lda, imp_sth->cda->rc, "odescr failed");
	return 0;
    }
    imp_sth->cda->rc = 0;
    num_fields = i - 1;
    DBIc_NUM_FIELDS(imp_sth) = num_fields;

    /* --- Setup the row cache for this query --- */
    if      (SvOK(ora_cache_o)) imp_sth->cache_size = SvIV(ora_cache_o);
    else if (SvOK(ora_cache))   imp_sth->cache_size = SvIV(ora_cache);
    else                        imp_sth->cache_size = 0;   /* auto size	*/
    /* deal with default (auto-size) and out of range cases		*/
    if (imp_sth->cache_size < 1) {	/* 0 == try to pick 'optimal'	*/
	/* Guess a maximum on-the-wire row width (but note t_dbsize	*/
	/* doesn't include longs yet so this could be suboptimal) 	*/
	int width = 8 + imp_sth->t_dbsize + num_fields*5;
	/* How many rows fit in 2Kb? (2Kb is a reasonable compromise)	*/
	imp_sth->cache_size = 2048 / width;
	if (imp_sth->cache_size < 5)	       /* cache at least 5	*/
	    imp_sth->cache_size = 5;
	else	/* if query includes longs, limit auto-size to 10 rows	*/
	if (has_longs && imp_sth->cache_size > 10)
	    imp_sth->cache_size = 10;
    }
    if (imp_sth->cache_size > 32767)	/* keep within Oracle's limits  */
	imp_sth->cache_size = 32767;
    /* Initialise cache counters */
    imp_sth->in_cache    = 0;
    imp_sth->eod_errno = 0;


    /* allocate field buffers				*/
    Newz(42, imp_sth->fbh,      num_fields, imp_fbh_t);
    /* allocate a buffer to hold all the column names	*/
    Newz(42, imp_sth->fbh_cbuf, t_cbufl + num_fields, char);

    cbuf_ptr = (sb1*)imp_sth->fbh_cbuf;
    for(i=1; i <= num_fields && imp_sth->cda->rc != 10; ++i) {
	imp_fbh_t *fbh = &imp_sth->fbh[i-1];
	fb_ary_t  *fb_ary;
	int dbtype;

	fbh->imp_sth = imp_sth;
	fbh->cbuf    = cbuf_ptr;
	fbh->cbufl   = f_cbufl[i];
	/* DESCRIBE */
	odescr(imp_sth->cda, i,
		&fbh->dbsize, &fbh->dbtype,  fbh->cbuf,  &fbh->cbufl,
		&fbh->dsize,  &fbh->prec,   &fbh->scale, &fbh->nullok);
	fbh->cbuf[fbh->cbufl] = '\0';	 /* ensure null terminated	*/
	cbuf_ptr += fbh->cbufl + 1;	 /* increment name pointer	*/

	/* Now define the storage for this field data.			*/

	/* Is it a LONG, LONG RAW, LONG VARCHAR or LONG VARRAW?		*/
	/* If so we need to implement oraperl truncation hacks.		*/
	/* This may change in a future release.				*/
	if ( (dbtype = dbtype_is_long(fbh->dbtype)) ) {
	    sb4 buflen = imp_sth->long_buflen;
	    if (buflen < 0)
		buflen = 80;		/* typical oracle app default	*/
	    fbh->dbsize = buflen;
	    fbh->dsize  = buflen;
	    fbh->ftype  = dbtype;	/* get long in non-var form	*/
	    imp_sth->t_dbsize += buflen;
	} else {
	    /* for the time being we fetch everything (except longs)	*/
	    /* as strings, that'll change (IV, NV and binary data etc)	*/
	    fbh->ftype = 5;		/* oraperl used 5 'STRING'	*/
	    /* dbsize can be zero for 'select NULL ...'			*/
	}

	fbh->fb_ary = fb_ary = fb_ary_alloc(
			fbh->dsize+1,	/* +1: STRING null terminator   */
			imp_sth->cache_size
		    );

	/* DEFINE output column variable storage */
	if (odefin(imp_sth->cda, i, fb_ary->abuf, fb_ary->bufl,
		fbh->ftype, -1, fb_ary->aindp, (text*)0, -1, -1,
		fb_ary->arlen, fb_ary->arcode)) {
	    warn("odefin error on %s: %d", fbh->cbuf, imp_sth->cda->rc);
	}

	if (dbis->debug >= 2)
	    fbh_dump(fbh, i, 0);
    }

    if (dbis->debug >= 2)
	fprintf(DBILOGFP,
	    "    dbd_describe'd %d columns (~%d data bytes, %d cache rows)\n",
	    (int)num_fields, imp_sth->t_dbsize, imp_sth->cache_size);

    if (imp_sth->cda->rc && imp_sth->cda->rc != 1007) {
	D_imp_dbh_from_sth;
	ora_error(h, &imp_dbh->lda, imp_sth->cda->rc, "odescr failed");
	return 0;
    }

    return 1;
}



static int 
_dbd_rebind_ph(sth, imp_sth, phs, maxlen) 
    SV *sth;
    imp_sth_t *imp_sth;
    phs_t *phs;
    int maxlen;
{
    maxlen = (maxlen < phs->alen) ? phs->alen : maxlen;

    if (dbis->debug >= 2)
	fprintf(DBILOGFP, "bind %s <== '%.200s' (size %d/%d, ora_type %d)\n",
	    phs->name, (char*)phs->progv, phs->alen,maxlen, phs->ftype);

    if (phs->alen_incnull)
	++phs->alen;

    /* Since we don't support LONG VAR types we must check	*/
    /* for lengths too big to pass to obndrv as an sword.	*/
    if (maxlen > SWORDMAXVAL)	/* generally 64K	*/
	croak("Can't bind %s, value is too long (%d bytes, max %d)",
		    phs->name, maxlen, SWORDMAXVAL);

    if (0) {	/* old code */
	if (obndrv(imp_sth->cda, (text*)phs->name, -1,
		(ub1*)phs->progv, (sword)phs->alen,
		phs->ftype, -1, &phs->indp,
		(text*)0, -1, -1)) {
	    D_imp_dbh_from_sth;
	    ora_error(sth, &imp_dbh->lda, imp_sth->cda->rc, "obndrv failed");
	    return 0;
	}
    }
    else {
	if (obndra(imp_sth->cda, (text *)phs->name, -1,
		(ub1*)phs->progv, maxlen, (sword)phs->ftype, -1,
		&phs->indp, &phs->alen, &phs->arcode, 0, (ub4 *)0,
		(text *)0, -1, -1)) {
	    D_imp_dbh_from_sth;
	    ora_error(sth, &imp_dbh->lda, imp_sth->cda->rc, "obndra failed");
	    return 0;
	}
    }
    return 1;
}


int
dbd_bind_ph(sth, ph_namesv, newvalue, attribs, is_inout, maxlen)
    SV *sth;
    SV *ph_namesv;
    SV *newvalue;
    SV *attribs;
    int is_inout;
    IV maxlen;
{
    D_imp_sth(sth);
    SV **phs_svp;
    STRLEN name_len;
    char *name;
    char namebuf[30];
    phs_t *phs;

    if (SvNIOK(ph_namesv) ) {	/* passed as a number	*/
	name = namebuf;
	sprintf(name, ":p%d", (int)SvIV(ph_namesv));
	name_len = strlen(name);
    } else {
	name = SvPV(ph_namesv, name_len);
    }

    if (dbis->debug >= 2)
	fprintf(DBILOGFP, "bind %s <== '%.200s' (attribs: %s)\n",
		name, SvPV(newvalue,na), attribs ? SvPV(attribs,na) : "" );

    phs_svp = hv_fetch(imp_sth->all_params_hv, name, name_len, 0);
    if (phs_svp == NULL)
	croak("Can't bind unknown placeholder '%s'", name);
    phs = (phs_t*)SvPVX(*phs_svp);	/* placeholder struct	*/

    if (phs->sv == &sv_undef) {	/* first bind for this placeholder	*/
	phs->ftype = 1;			/* our default type VARCHAR2	*/
	if (is_inout) {
	    /* ensure room for result, 28 is magic number (see sv_2pv)	*/
	    I32 grow_len = (maxlen < 28) ? 28 : maxlen+1;
	    ++imp_sth->has_inout_params;
	    phs->is_inout = 1;
	    phs->sv = SvREFCNT_inc(newvalue);
	    if (SvOOK(phs->sv))
		sv_backoff(phs->sv);
	    /* pre-upgrade to cut down risks of SvPVX realloc/move	*/
	    (void)SvUPGRADE(phs->sv, SVt_PVNV);
	    /* ensure we have a string to point oracle at		*/
	    (void)SvPV(phs->sv, na);
	    SvGROW(phs->sv, grow_len);
	    phs->progv = SvPVX(phs->sv);
	    /* build array of phs's so we can deal with out vars fast	*/
	    if (!imp_sth->out_params_av)
		imp_sth->out_params_av = newAV();
	    av_push(imp_sth->out_params_av, SvREFCNT_inc(*phs_svp));
	}
	else {
	    phs->sv = newSV(0);
	    phs->is_inout = 0;
	}

	if (attribs) {	/* only look for ora_type on first bind of var	*/
	    SV **svp;
	    /* Setup / Clear attributes as defined by attribs.		*/
	    /* XXX If attribs is EMPTY then reset attribs to default?	*/
	    if ( (svp=hv_fetch((HV*)SvRV(attribs), "ora_type",8, 0)) != NULL) {
		int ora_type = SvIV(*svp);
		if (!dbtype_is_string(ora_type))	/* mean but safe	*/
		    croak("Can't bind %s, ora_type %d not a simple string type",
			    phs->name, ora_type);
		phs->ftype = ora_type;
	    }
	}

	/* some types require the trailing null included in the length.	*/
	phs->alen_incnull = (phs->ftype==SQLT_STR || phs->ftype==SQLT_AVC);

    }
    else if (is_inout != phs->is_inout) {
	croak("Can't change param %s in/out mode", phs->name);
    }


    /* At the moment we always do sv_setsv() and rebind.	*/
    /* Later we may optimise this so that more often we can	*/
    /* just copy the value & length over and not rebind.	*/

    if (!SvOK(newvalue)) {	/* undef == NULL		*/
	phs->indp  = -1;
	phs->progv = "";
	phs->alen  = 0;
    }
    else {
	STRLEN value_len;
	phs->indp = 0;
	/* XXX need to consider oraperl null vs space issues?	*/
	if (is_inout) {	/* XXX */
	    phs->progv = SvPV(phs->sv, value_len);
	}
	else {
	    sv_setsv(phs->sv, newvalue);
	    phs->progv = SvPV(phs->sv, value_len);
	}
	phs->alen = value_len + phs->alen_incnull;
    }

    return _dbd_rebind_ph(sth, imp_sth, phs, maxlen);
}



int
dbd_st_execute(sth)	/* <0 is error, >=0 is ok (row count) */
    SV *sth;
{
    D_imp_sth(sth);
    int debug = dbis->debug;
    int outparams = (imp_sth->out_params_av) ? AvFILL(imp_sth->out_params_av)+1 : 0;

    if (!imp_sth->done_desc) {
	/* describe and allocate storage for results (if any needed)	*/
	if (!dbd_describe(sth, imp_sth))
	    return -1; /* dbd_describe already called ora_error()	*/
    }

    if (debug >= 2)
	fprintf(DBILOGFP,
	    "    dbd_st_execute (for sql f%d after oci f%d)...\n",
			imp_sth->cda->ft, imp_sth->cda->fc);

    if (outparams) {	/* check validity of bound SV's	*/
	int i = outparams;
	STRLEN phs_len;
	while(--i >= 0) {
	    phs_t *phs = (phs_t*)SvPVX(AvARRAY(imp_sth->out_params_av)[i]);
	    /* Make sure we have the value in string format. Typically a number	*/
	    /* will be converted back into a string using the same bound buffer	*/
	    /* so the progv test below will not trip.			*/
	    if (!SvPOK(phs->sv)) {		/* ooops, no string ($var = 42)	*/
		SvPV(phs->sv, phs_len);		/* get a string ("42")		*/
		phs->alen = phs_len + phs->alen_incnull;
	    }
	    else {
		if (SvOOK(phs->sv))
		    sv_backoff(phs->sv);
		phs->alen = SvCUR(phs->sv) + phs->alen_incnull;
	    }
	    /* Some checks for mutated storage since we pointed oracle at it.	*/
	    /* XXX Instead of croaking we could rebind (probably will later).	*/
	    if (SvTYPE(phs->sv) != SVt_PVNV)
		croak("Placeholder %s value mutated type after bind.\n", phs->name);
	    if (SvPVX(phs->sv) != phs->progv)
		croak("Placeholder %s value mutated location after bind.\n", phs->name);
	    if (debug >= 2)
		warn("pre %s = '%s' (len %d)\n", phs->name, SvPVX(phs->sv), phs->alen);
	}
    }

    /* reset cache counters */
    imp_sth->in_cache   = 0;
    imp_sth->next_entry = 0;
    imp_sth->eod_errno  = 0;

    /* Trigger execution of the statement */
    if (DBIc_NUM_FIELDS(imp_sth) > 0) {  	/* is a SELECT	*/
	/* The number of fields is used because imp_sth->cda->ft is unreliable.	*/
	/* Specifically an update (5) may change to select (4) after odesc().	*/
	if (oexfet(imp_sth->cda, (ub4)imp_sth->cache_size, 0, 0)
		&& imp_sth->cda->rc != 1403 /* other than no more data */ ) {
	    ora_error(sth, imp_sth->cda, imp_sth->cda->rc, "oexfet error");
	    return -1;
	}
	imp_sth->in_cache = imp_sth->cda->rpc;	/* cache loaded */
	if (imp_sth->cda->rc == 1403)
	    imp_sth->eod_errno = 1403;
    }
    else {					/* NOT a select */
	if (oexec(imp_sth->cda)) {
	    ora_error(sth, imp_sth->cda, imp_sth->cda->rc, "oexec error");
	    return -1;
	}
    }

    if (debug >= 2)
	fprintf(DBILOGFP, "    dbd_st_execute complete (rc%d, w%02x, rpc%ld, eod%d, out%d)\n",
		imp_sth->cda->rc,  imp_sth->cda->wrn,
		imp_sth->cda->rpc, imp_sth->eod_errno,
		imp_sth->has_inout_params);

    if (outparams) {	/* check validity of bound SV's	*/
	int i = outparams;
	STRLEN retlen;
	while(--i >= 0) {
	    phs_t *phs = (phs_t*)SvPVX(AvARRAY(imp_sth->out_params_av)[i]);
	    SV *sv = phs->sv;
	    if (phs->indp == 0) {			/* is okay	*/
		SvPOK_only(sv);
		SvCUR(sv) = phs->alen;
		*SvEND(sv) = '\0';
		if (debug >= 2)
		    warn("    %s = '%s' (len %d)\n", phs->name, SvPV(sv,retlen),retlen);
	    }
	    else
	    if (phs->indp > 0 || phs->indp == -2) {	/* truncated	*/
		SvPOK_only(sv);
		SvCUR(sv) = phs->alen;
		*SvEND(sv) = '\0';
		if (debug >= 2)
		    warn("    %s = '%s' (TRUNCATED from %d to %d)\n", phs->name,
			    SvPV(sv,retlen), phs->indp, retlen);
	    }
	    else
	    if (phs->indp == -1) {			/* is NULL	*/
		(void)SvOK_off(phs->sv);
		if (debug >= 2)
		     warn("    %s = undef (NULL)\n", phs->name);
	    }
	    else croak("%s bad indp %d", phs->name, phs->indp);
	}
    }

    DBIc_ACTIVE_on(imp_sth);	/* XXX should only set for select ?	*/
    return imp_sth->cda->rpc;	/* row count	*/
}



AV *
dbd_st_fetch(sth)
    SV *	sth;
{
    D_imp_sth(sth);
    int debug = dbis->debug;
    int num_fields;
    int i;
    AV *av;

    /* Check that execute() was executed sucessfully. This also implies	*/
    /* that dbd_describe() executed sucessfuly so the memory buffers	*/
    /* are allocated and bound.						*/
    if ( !DBIc_ACTIVE(imp_sth) ) {
	ora_error(sth, NULL, 1, "no statement executing");
	return Nullav;
    }

    if (!imp_sth->in_cache) {	/* refill cache if empty	*/
	int rows_returned;

	if (imp_sth->eod_errno) {
    end_of_data:
	    if (imp_sth->eod_errno != 1403) {	/* was not just end-of-fetch	*/
		ora_error(sth, imp_sth->cda, imp_sth->eod_errno, "ofetch error");
	    } else {				/* is simply no more data	*/
		sv_setiv(DBIc_ERR(imp_sth), 0);	/* ensure errno set to 0 here	*/
		if (debug >= 2)
		    fprintf(DBILOGFP, "    dbd_st_fetch no-more-data, rc=%d, rpc=%ld\n",
			imp_sth->cda->rc, imp_sth->cda->rpc);
	    }
	    imp_sth->eod_errno = 0;		/* let user retry if they want	*/
	    return Nullav;
	}

	rows_returned = imp_sth->cda->rpc;	/* remember rpc before re-fetch	*/
	if (ofen(imp_sth->cda, imp_sth->cache_size)) {
	    /* Note that errors may happen after one or more rows have been	*/
	    /* added to the cache. We record the error but don't handle it till	*/
	    /* the cache is empty (which may be at once if no rows returned).	*/
	    imp_sth->eod_errno = imp_sth->cda->rc;	/* store rc for later	*/
	    if (imp_sth->cda->rpc == rows_returned)	/* no more rows fetched	*/
		goto end_of_data;
	    /* else fall through and return the first of the fetched rows	*/
	}
	imp_sth->in_cache   = imp_sth->cda->rpc - rows_returned;
	imp_sth->next_entry = 0;
    }

    av = DBIS->get_fbav(imp_sth);
    num_fields = AvFILL(av)+1;

    if (debug >= 3)
	fprintf(DBILOGFP, "    dbd_st_fetch %d fields (cache: %d/%d/%d)\n",
		num_fields, imp_sth->next_entry, imp_sth->in_cache,
		imp_sth->cache_size);

    for(i=0; i < num_fields; ++i) {
	imp_fbh_t *fbh = &imp_sth->fbh[i];
	int cache_entry = imp_sth->next_entry;
	fb_ary_t *fb_ary = fbh->fb_ary;
	int rc = fb_ary->arcode[cache_entry];
	SV *sv = AvARRAY(av)[i]; /* Note: we (re)use the SV in the AV	*/

	if (rc == 1406 && dbtype_is_long(fbh->dbtype)) {
	    /* We have a LONG field which has been truncated.		*/
	    int oraperl = DBIc_COMPAT(imp_sth);
	    if ((oraperl) ? SvIV(ora_trunc) : imp_sth->long_trunc_ok) {
		/* Oraperl recorded the truncation in ora_errno.	*/
		/* We do so but it's not part of the DBI spec.		*/
		sv_setiv(DBIc_ERR(imp_sth), (IV)rc); /* record it	*/
		rc = 0;			/* but don't provoke an error	*/
	    }
	}

	if (rc == 0) {			/* the normal case		*/
	    sv_setpvn(sv, (char*)&fb_ary->abuf[cache_entry * fb_ary->bufl],
			  fb_ary->arlen[cache_entry]);

	} else if (rc == 1405) {	/* field is null - return undef	*/
	    (void)SvOK_off(sv);

	} else {  /* See odefin rcode arg description in OCI docs	*/
	    /* These may get case-by-case treatment eventually.	*/
	    /* Some should probably be treated as warnings but	*/
	    /* for now we just treat them all as errors		*/
	    ora_error(sth, imp_sth->cda, rc, "ofetch rcode");
	    (void)SvOK_off(sv);
	}

	if (debug >= 3)
	    fprintf(DBILOGFP, "        %d: rc=%d '%s'\n",
		i, rc, SvPV(sv,na));

    }

    /* update cache counters */
    --imp_sth->in_cache;
    ++imp_sth->next_entry;

    return av;
}




int
dbd_st_blob_read(sth, field, offset, len, destrv, destoffset)
    SV *sth;
    int field;
    long offset;
    long len;
    SV *destrv;
    long destoffset;
{
    D_imp_sth(sth);
    ub4 retl;
    SV *bufsv;

    bufsv = SvRV(destrv);
    sv_setpvn(bufsv,"",0);	/* ensure it's writable string	*/
    SvGROW(bufsv, len+destoffset+1);	/* SvGROW doesn't do +1	*/

    if (oflng(imp_sth->cda, (sword)field+1,
	    ((ub1*)SvPVX(bufsv)) + destoffset, len,
	    imp_sth->fbh[field].ftype, /* original long type	*/
	    &retl, offset)) {
	ora_error(sth, imp_sth->cda, imp_sth->cda->rc, "oflng error");
	/* XXX database may have altered the buffer contents	*/
	return 0;
    }
    /* Sadly, even though retl is a ub4, oracle will cap the	*/
    /* value of retl at 65535 even if more was returned!	*/
    /* This is according to the OCI manual for Oracle 7.0.	*/
    /* Once again Oracle causes us grief. How can we tell what	*/
    /* length to assign to destrv? We do have a compromise: if	*/
    /* retl is exactly 65535 we assume that all data was read.	*/
    SvCUR_set(bufsv, destoffset+((retl == 65535) ? len : retl));
    *SvEND(bufsv) = '\0'; /* consistent with perl sv_setpvn etc	*/

    return 1;
}


int
dbd_st_rows(sth)
    SV *sth;
{
    D_imp_sth(sth);
    return imp_sth->cda->rpc;
}


int
dbd_st_finish(sth)
    SV *sth;
{
    D_imp_sth(sth);
    /* Cancel further fetches from this cursor.                 */
    /* We don't close the cursor till DESTROY (dbd_st_destroy). */
    /* The application may re execute(...) it.                  */
    if (DBIc_ACTIVE(imp_sth) && ocan(imp_sth->cda) ) {
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
    DBIc_ACTIVE_off(imp_sth);
    return 1;
}


void
dbd_st_destroy(sth)
    SV *sth;
{
    D_imp_sth(sth);
    D_imp_dbh_from_sth;
    int fields;
    int i;
    /* Check if an explicit disconnect() or global destruction has	*/
    /* disconnected us from the database before attempting to close.	*/
    if (DBIc_ACTIVE(imp_dbh) && oclose(imp_sth->cda)) {
	/* Check for ORA-01041: 'hostdef extension doesn't exist'	*/
	/* which indicates that the lda had already been logged out	*/
	/* in which case only complain if not in 'global destruction'.	*/
	/* NOT NEEDED NOW? if ( ! (imp_sth->cda->rc == 1041 && dirty) ) */
	    ora_error(sth, imp_sth->cda, imp_sth->cda->rc, "oclose error");
	/* fall through */
    } 

    /* Free off contents of imp_sth	*/

    fields = DBIc_NUM_FIELDS(imp_sth);
    imp_sth->in_cache    = 0;
    imp_sth->eod_errno = 1403;
    for(i=0; i < fields; ++i) {
	imp_fbh_t *fbh = &imp_sth->fbh[i];
	fb_ary_free(fbh->fb_ary);
    }
    Safefree(imp_sth->fbh);
    Safefree(imp_sth->fbh_cbuf);
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
		phs_t *phs_tpl = (phs_t*)SvPVX(sv);
		sv_free(phs_tpl->sv);
	    }
	}
	sv_free((SV*)imp_sth->all_params_hv);
    }

    DBIc_IMPSET_off(imp_sth);		/* let DBI know we've done it	*/
}


int
dbd_st_STORE(sth, keysv, valuesv)
    SV *sth;
    SV *keysv;
    SV *valuesv;
{
    return FALSE;
#ifdef not_used_yet
    D_imp_sth(sth);
    STRLEN kl;
    char *key = SvPV(keysv,kl);
    int on = SvTRUE(valuesv);
    SV *cachesv = NULL;
    int oraperl = DBIc_COMPAT(imp_sth);

    if (cachesv) /* cache value for later DBI 'quick' fetch? */
	hv_store((HV*)SvRV(sth), key, kl, cachesv, 0);
    return TRUE;
#endif
}


SV *
dbd_st_FETCH(sth, keysv)
    SV *sth;
    SV *keysv;
{
    D_imp_sth(sth);
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
	/* dbd_describe has already called ora_error()		*/
	/* we can't return Nullsv here because the xs code will	*/
	/* then just pass the attribute name to DBI for FETCH.	*/
	croak("Describe failed during %s->FETCH(%s)",
		SvPV(sth,na), key);
    }

    i = DBIc_NUM_FIELDS(imp_sth);

    if (kl==11 && strEQ(key, "ora_lengths")) {
	AV *av = newAV();
	retsv = newRV(sv_2mortal((SV*)av));
	while(--i >= 0)
	    av_store(av, i, newSViv((IV)imp_sth->fbh[i].dsize));

    } else if (kl==9 && strEQ(key, "ora_types")) {
	AV *av = newAV();
	retsv = newRV(sv_2mortal((SV*)av));
	while(--i >= 0)
	    av_store(av, i, newSViv(imp_sth->fbh[i].dbtype));

    } else if (kl==4 && strEQ(key, "NAME")) {
	AV *av = newAV();
	retsv = newRV(sv_2mortal((SV*)av));
	while(--i >= 0)
	    av_store(av, i, newSVpv((char*)imp_sth->fbh[i].cbuf,0));

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

