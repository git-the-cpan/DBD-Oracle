#include "Oracle.h"

#define BIND_PARAM_INOUT_ALLOW_ARRAY

DBISTATE_DECLARE;

MODULE = DBD::Oracle    PACKAGE = DBD::Oracle

I32
constant(name=Nullch)
    char *name
    ALIAS:
    ORA_VARCHAR2 =   1
    ORA_NUMBER	 =   2
    ORA_STRING	 =   5
    ORA_LONG	 =   8
    ORA_ROWID	 =  11
    ORA_DATE	 =  12
    ORA_RAW	 =  23
    ORA_LONGRAW	 =  24
    ORA_CHAR	 =  96
    ORA_CHARZ	 =  97
    ORA_MLSLABEL = 105
    ORA_NTY	 = 108
    ORA_CLOB	 = 112
    ORA_BLOB	 = 113
    ORA_RSET	 = 116
    ORA_VARCHAR2_TABLE = ORA_VARCHAR2_TABLE
    ORA_NUMBER_TABLE   = ORA_NUMBER_TABLE
    ORA_SYSDBA	 = 0x0002
    ORA_SYSOPER	 = 0x0004
    SQLCS_IMPLICIT = SQLCS_IMPLICIT
    SQLCS_NCHAR    = SQLCS_NCHAR
    SQLT_INT     = SQLT_INT
    SQLT_FLT     = SQLT_FLT    
    CODE:
    if (!ix) {
	if (!name) name = GvNAME(CvGV(cv));
	croak("Unknown DBD::Oracle constant '%s'", name);
    }
    else RETVAL = ix;
    OUTPUT:
    RETVAL

void
ORA_OCI()
    CODE:
    SV *sv = sv_newmortal();
    sv_setnv(sv, atof(ORA_OCI_VERSION));	/* 9.1! see docs */
    sv_setpv(sv, ORA_OCI_VERSION);		/* 9.10.11.12    */
    SvNOK_on(sv); /* dualvar hack */
    ST(0) = sv;

void
ora_env_var(name)
    char *name
    CODE:
    char buf[1024];
    char *p = ora_env_var(name, buf, sizeof(buf)-1);
    SV *sv = sv_newmortal();
    if (p)
        sv_setpv(sv, p);
    ST(0) = sv;

#ifdef __CYGWIN32__
void
ora_cygwin_set_env(name, value)
    char * name
    char * value
    CODE:
    ora_cygwin_set_env(name, value);

#endif /* __CYGWIN32__ */


INCLUDE: Oracle.xsi

MODULE = DBD::Oracle    PACKAGE = DBD::Oracle::st

void
ora_fetch(sth)
    SV *	sth
    PPCODE:
    /* fetchrow: but with scalar fetch returning NUM_FIELDS for Oraperl	*/
    /* This code is called _directly_ by Oraperl.pm bypassing the DBI.	*/
    /* as a result we have to do some things ourselves (like calling	*/
    /* CLEAR_ERROR) and we loose the tracing that the DBI offers :-(	*/
    D_imp_sth(sth);
    AV *av;
    int debug = DBIc_DEBUGIV(imp_sth);
    if (DBIS->debug > debug)
	debug = DBIS->debug;
    DBIh_CLEAR_ERROR(imp_sth);
    if (GIMME == G_SCALAR) {	/* XXX Oraperl	*/
	/* This non-standard behaviour added only to increase the	*/
	/* performance of the oraperl emulation layer (Oraperl.pm)	*/
	if (!imp_sth->done_desc && !dbd_describe(sth, imp_sth))
		XSRETURN_UNDEF;
	XSRETURN_IV(DBIc_NUM_FIELDS(imp_sth));
    }
    if (debug >= 2)
	PerlIO_printf(DBILOGFP, "    -> ora_fetch\n");
    av = dbd_st_fetch(sth, imp_sth);
    if (av) {
	int num_fields = AvFILL(av)+1;
	int i;
	EXTEND(sp, num_fields);
	for(i=0; i < num_fields; ++i) {
	    PUSHs(AvARRAY(av)[i]);
	}
	if (debug >= 2)
	    PerlIO_printf(DBILOGFP, "    <- (...) [%d items]\n", num_fields);
    }
    else {
	if (debug >= 2)
	    PerlIO_printf(DBILOGFP, "    <- () [0 items]\n");
    }
    if (debug >= 2 && SvTRUE(DBIc_ERR(imp_sth)))
	PerlIO_printf(DBILOGFP, "    !! ERROR: %s %s",
	    neatsvpv(DBIc_ERR(imp_sth),0), neatsvpv(DBIc_ERRSTR(imp_sth),0));

void
ora_execute_array(sth, tuples, exe_count, tuples_status, cols=&sv_undef)
    SV *        sth
    SV *        tuples
    IV         exe_count
    SV *        tuples_status
    SV *        cols
    PREINIT:
    D_imp_sth(sth);
    int retval;
    CODE:
    /* XXX Need default bindings if any phs are so far unbound(?) */
    /* XXX this code is duplicated in selectrow_arrayref above  */
    if (DBIc_ROW_COUNT(imp_sth) > 0) /* reset for re-execute */
        DBIc_ROW_COUNT(imp_sth) = 0;
    retval = ora_st_execute_array(sth, imp_sth, tuples, tuples_status,
                                  cols, (ub4)exe_count);
    /* XXX Handle return value ... like DBI::execute_array(). */
    /* remember that dbd_st_execute must return <= -2 for error */
    if (retval == 0)            /* ok with no rows affected     */
        XST_mPV(0, "0E0");      /* (true but zero)              */
    else if (retval < -1)       /* -1 == unknown number of rows */
        XST_mUNDEF(0);          /* <= -2 means error            */
    else
        XST_mIV(0, retval);     /* typically 1, rowcount or -1  */


void
cancel(sth)
    SV *        sth
    CODE:
    D_imp_sth(sth);
    ST(0) = dbd_st_cancel(sth, imp_sth) ? &sv_yes : &sv_no;


MODULE = DBD::Oracle    PACKAGE = DBD::Oracle::db

void
reauthenticate(dbh, uid, pwd)
    SV *	dbh
    char *	uid
    char *	pwd
    CODE:
    D_imp_dbh(dbh);
    ST(0) = ora_db_reauthenticate(dbh, imp_dbh, uid, pwd) ? &sv_yes : &sv_no;

void
ora_lob_write(dbh, locator, offset, data)
    SV *dbh
    OCILobLocator   *locator
    UV	offset
    SV	*data
    PREINIT:
    D_imp_dbh(dbh);
    ub4 amtp;
    STRLEN data_len; /* bytes not chars */
    dvoid *bufp;
    sword status;
    ub2 csid;
    ub1 csform;
    CODE:
    csid = 0;
    csform = SQLCS_IMPLICIT;
    bufp = SvPV(data, data_len);
    amtp = data_len;
    /* if locator is CLOB and data is UTF8 and not in bytes pragma */
    /* if (0 && SvUTF8(data) && !IN_BYTES) { amtp = sv_len_utf8(data); }  */
    /* added by lab: */
    /* LAB do something about length here? see above comment */
    OCILobCharSetForm_log_stat( imp_dbh->envhp, imp_dbh->errhp, locator, &csform, status );
    if (status != OCI_SUCCESS) {
        oci_error(dbh, imp_dbh->errhp, status, "OCILobCharSetForm");
	ST(0) = &sv_undef;
        return;
    }
#ifdef OCI_ATTR_CHARSET_ID
    /* Effectively only used so AL32UTF8 works properly */
    OCILobCharSetId_log_stat( imp_dbh->envhp, imp_dbh->errhp, locator, &csid, status );
    if (status != OCI_SUCCESS) {
        oci_error(dbh, imp_dbh->errhp, status, "OCILobCharSetId");
	ST(0) = &sv_undef;
        return;
    }
#endif /* OCI_ATTR_CHARSET_ID */
    /* if data is utf8 but charset isn't then switch to utf8 csid */
    csid = (SvUTF8(data) && !CS_IS_UTF8(csid)) ? utf8_csid : CSFORM_IMPLIED_CSID(csform);

    OCILobWrite_log_stat(imp_dbh->svchp, imp_dbh->errhp, locator,
	    &amtp, (ub4)offset,
	    bufp, (ub4)data_len, OCI_ONE_PIECE,
	    NULL, NULL,
	    (ub2)0, csform , status);
    if (status != OCI_SUCCESS) {
        oci_error(dbh, imp_dbh->errhp, status, "OCILobWrite");
	ST(0) = &sv_undef;
    }
    else {
	ST(0) = &sv_yes;
    }

void
ora_lob_append(dbh, locator, data)
    SV *dbh
    OCILobLocator   *locator
    SV	*data
    PREINIT:
    D_imp_dbh(dbh);
    ub4 amtp;
    STRLEN data_len; /* bytes not chars */
    dvoid *bufp;
    sword status;
#if defined(ORA_OCI_8) || !defined(OCI_HTYPE_DIRPATH_FN_CTX) /* Oracle is < 9.0 */
    ub4 startp;
#endif
    ub1 csform;
    ub2 csid;
    CODE:
    csid = 0;
    csform = SQLCS_IMPLICIT;
    bufp = SvPV(data, data_len);
    amtp = data_len;
    /* if locator is CLOB and data is UTF8 and not in bytes pragma */
    /* if (1 && SvUTF8(data) && !IN_BYTES) */
    /* added by lab: */
    /* LAB do something about length here? see above comment */
    OCILobCharSetForm_log_stat( imp_dbh->envhp, imp_dbh->errhp, locator, &csform, status );
    if (status != OCI_SUCCESS) {
        oci_error(dbh, imp_dbh->errhp, status, "OCILobCharSetForm");
	ST(0) = &sv_undef;
        return;
    }
#ifdef OCI_ATTR_CHARSET_ID
    /* Effectively only used so AL32UTF8 works properly */
    OCILobCharSetId_log_stat( imp_dbh->envhp, imp_dbh->errhp, locator, &csid, status );
    if (status != OCI_SUCCESS) {
        oci_error(dbh, imp_dbh->errhp, status, "OCILobCharSetId");
	ST(0) = &sv_undef;
        return;
    }
#endif /* OCI_ATTR_CHARSET_ID */
    /* if data is utf8 but charset isn't then switch to utf8 csid */
    csid = (SvUTF8(data) && !CS_IS_UTF8(csid)) ? utf8_csid : CSFORM_IMPLIED_CSID(csform);
#if !defined(ORA_OCI_8) && defined(OCI_HTYPE_DIRPATH_FN_CTX) /* Oracle is >= 9.0 */
    OCILobWriteAppend_log_stat(imp_dbh->svchp, imp_dbh->errhp, locator,
			       &amtp, bufp, (ub4)data_len, OCI_ONE_PIECE,
			       NULL, NULL,
			       csid, csform, status);
    if (status != OCI_SUCCESS) {
       oci_error(dbh, imp_dbh->errhp, status, "OCILobWriteAppend");
       ST(0) = &sv_undef;
    }
    else {
       ST(0) = &sv_yes;
    }
#else
    OCILobGetLength_log_stat(imp_dbh->svchp, imp_dbh->errhp, locator, &startp, status);
    if (status != OCI_SUCCESS) {
       oci_error(dbh, imp_dbh->errhp, status, "OCILobGetLength");
       ST(0) = &sv_undef;
    } else {
       /* start one after the end -- the first position in the LOB is 1 */
       startp++;
       if (DBIS->debug >= 2 )
            PerlIO_printf(DBILOGFP, "    Calling OCILobWrite with csid=%d csform=%d\n",csid, csform );
       OCILobWrite_log_stat(imp_dbh->svchp, imp_dbh->errhp, locator,
			    &amtp, startp,
			    bufp, (ub4)data_len, OCI_ONE_PIECE,
			    NULL, NULL,
			    csid, csform , status);
       if (status != OCI_SUCCESS) {
	  oci_error(dbh, imp_dbh->errhp, status, "OCILobWrite");
	  ST(0) = &sv_undef;
       }
       else {
	  ST(0) = &sv_yes;
       }
    }
#endif


void
ora_lob_read(dbh, locator, offset, length)
    SV *dbh
    OCILobLocator   *locator
    UV	offset
    UV	length
    PREINIT:
    D_imp_dbh(dbh);
    ub4 amtp;
    STRLEN bufp_len;
    SV *dest_sv;
    dvoid *bufp;
    sword status;
    ub1 csform;
    CODE:
    csform = SQLCS_IMPLICIT;
    dest_sv = sv_2mortal(newSV(length*4)); /*LAB: crude hack that works... tim did it else where XXX */
    SvPOK_on(dest_sv);
    bufp_len = SvLEN(dest_sv);	/* XXX bytes not chars? (lab: yes) */
    bufp = SvPVX(dest_sv);
    amtp = length;	/* if utf8 and clob/nclob: in: chars, out: bytes */
    /* http://www.lc.leidenuniv.nl/awcourse/oracle/appdev.920/a96584/oci16m40.htm#427818 */
    /* if locator is CLOB and data is UTF8 and not in bytes pragma */
    /* if (0 && SvUTF8(dest_sv) && !IN_BYTES) { amtp = sv_len_utf8(dest_sv); }  */
    /* added by lab: */
    OCILobCharSetForm_log_stat( imp_dbh->envhp, imp_dbh->errhp, locator, &csform, status );
    if (status != OCI_SUCCESS) {
        oci_error(dbh, imp_dbh->errhp, status, "OCILobCharSetForm");
	dest_sv = &sv_undef;
        return;
    }
    OCILobRead_log_stat(imp_dbh->svchp, imp_dbh->errhp, locator,
	    &amtp, (ub4)offset, /* offset starts at 1 */
	    bufp, (ub4)bufp_len,
	    0, 0, (ub2)0, csform, status);
    if (status != OCI_SUCCESS) {
        oci_error(dbh, imp_dbh->errhp, status, "OCILobRead");
        dest_sv = &sv_undef;
    }
    else {
        SvCUR(dest_sv) = amtp; /* always bytes here */
        *SvEND(dest_sv) = '\0';
	if (CSFORM_IMPLIES_UTF8(csform))
	    SvUTF8_on(dest_sv);

    }
    ST(0) = dest_sv;

void
ora_lob_trim(dbh, locator, length)
    SV *dbh
    OCILobLocator   *locator
    UV	length
    PREINIT:
    D_imp_dbh(dbh);
    sword status;
    CODE:
    OCILobTrim_log_stat(imp_dbh->svchp, imp_dbh->errhp, locator, length, status);
    if (status != OCI_SUCCESS) {
        oci_error(dbh, imp_dbh->errhp, status, "OCILobTrim");
	ST(0) = &sv_undef;
    }
    else {
	ST(0) = &sv_yes;
    }

void
ora_lob_length(dbh, locator)
    SV *dbh
    OCILobLocator   *locator
    PREINIT:
    D_imp_dbh(dbh);
    sword status;
    ub4 len = 0;
    CODE:
    OCILobGetLength_log_stat(imp_dbh->svchp, imp_dbh->errhp, locator, &len, status);
    if (status != OCI_SUCCESS) {
        oci_error(dbh, imp_dbh->errhp, status, "OCILobTrim");
	ST(0) = &sv_undef;
    }
    else {
	ST(0) = sv_2mortal(newSVuv(len));
    }



MODULE = DBD::Oracle    PACKAGE = DBD::Oracle::dr

void
init_oci(drh)
    SV *	drh
    CODE:
    D_imp_drh(drh);
	dbd_init_oci(DBIS) ;
	dbd_init_oci_drh(imp_drh) ;

    

	
