# Oraperl Emulation Interface for Perl 5 DBD::Oracle DBI
#
# $Id: Oraperl.pm,v 1.25 1996/05/07 20:47:15 timbo Exp $
#
#   Copyright (c) 1994,1995 Tim Bunce
#
#   You may distribute under the terms of either the GNU General Public
#   License or the Artistic License, as specified in the Perl README file.
#
# To use this interface use one of the following invocations:
#
#       use Oraperl;
# or
#       eval 'use Oraperl; 1' || die $@ if $] >= 5;
#
# The second form allows oraperl scripts to be used with
# both oraperl and perl 5.

package Oraperl;

require DBI;
# use Carp;
require Exporter;

$VERSION = substr(q$Revision: 1.25 $, 10);

@ISA = qw(Exporter);

@EXPORT = qw(
    &ora_login &ora_open &ora_bind &ora_fetch &ora_close
    &ora_logoff &ora_do &ora_titles &ora_lengths &ora_types
    &ora_commit &ora_rollback &ora_autocommit &ora_version
    &ora_readblob
    $ora_cache $ora_long $ora_trunc $ora_errno $ora_errstr
    $ora_verno $ora_debug
);

$debug    = 0 unless defined $debug;
$debugdbi = 0;
# $safe		# set true before 'use Oraperl' if needed.

if ($debugdbi){
    my $sw = DBI->internal;
    $sw->debug($debugdbi);
    print "Switch: $sw->{Attribution}, $sw->{Version}\n";
    $sw->{DebugDispatch} = $debugdbi;
}

# Install Driver
$drh = DBI->install_driver('Oracle');
if ($drh) {
	print "DBD::Oracle driver installed as $drh\n" if $debug;
	$drh->debug($debug);
	$drh->{CompatMode} = 1;
	$drh->{Warn}       = 0;
}


use strict;

sub _func_ref {
    my $name = shift;
    my $pkg = ($Oraperl::safe) ? "DBI" : "DBD::Oracle";
    \&{"${pkg}::$name"};
}


#	-----------------------------------------------------------------
#
#	$lda = &ora_login($system_id, $name, $password)
#	&ora_logoff($lda)

sub ora_login {
    my($system_id, $name, $password) = @_;
    $Oraperl::drh->connect($system_id, $name, $password);
}
*ora_logoff  = _func_ref('db::disconnect');


# -----------------------------------------------------------------
#
#   $csr = &ora_open($lda, $stmt [, $cache])
#   &ora_bind($csr, $var, ...)
#   &ora_fetch($csr [, $trunc])
#   &ora_do($lda, $stmt)
#   &ora_close($csr)

sub ora_open {
    my($lda, $stmt, $cache) = @_;

    my $csr = $lda->prepare($stmt) or return undef;

    # only execute here if no bind vars specified
    $csr->execute or return undef unless $csr->{NUM_OF_PARAMS};

    $csr;
}

*ora_bind  = _func_ref('st::execute');
*ora_fetch = _func_ref('st::fetchrow');
*ora_close = _func_ref('st::finish');

sub ora_do {
    # error => undef
    # 0     => "0E0"	(0 but true)
    # >0    => >0
    my($lda, $stmt) = @_;

    return $lda->do($stmt);	# SEE DEFAULT METHOD IN DBI.pm

    # OLD CODE:
    # $csr is local, cursor will be closed on exit
    my $csr = $lda->prepare($stmt) or return undef;
    # Oracle OCI will automatically execute DDL statements in prepare()!
    # We must be carefull not to execute them again! This needs careful
    # examination and thought.
    # Perhaps oracle is smart enough not to execute them again?
    my $ret = $csr->execute;
    my $rows = $csr->rows;
    ($rows == 0) ? "0E0" : $rows;
}


# -----------------------------------------------------------------
#
#   &ora_titles($csr [, $truncate])
#   &ora_lengths($csr)
#   &ora_types($csr)

sub ora_titles{
    my($csr, $trunc) = @_;
    warn "ora_titles: truncate option not implemented" if $trunc;
    @{$csr->{'NAME'}};
}
sub ora_lengths{
    @{shift->{'ora_lengths'}}		# oracle specific
}
sub ora_types{
    @{shift->{'ora_types'}}		# oracle specific
}


# -----------------------------------------------------------------
#
#   &ora_commit($lda)
#   &ora_rollback($lda)
#   &ora_autocommit($lda, $on_off)
#   &ora_version

*ora_commit   = _func_ref('db::commit');
*ora_rollback = _func_ref('db::rollback');

sub ora_autocommit {
    my($lda, $mode) = @_;
    $lda->{AutoCommit} = $mode;
    "0E0";
}
sub ora_version {
    my($sw)  = DBI->internal;
    print "\n";
    print "Oraperl Emulation Interface version $Oraperl::VERSION\n";
    print "Oracle Driver $Oraperl::drh->{Version}\n";
    print "$sw->{Attribution}, version $sw->{Version}\n\n";
}


# -----------------------------------------------------------------
#
#   $ora_errno
#   $ora_errstr

# This is really internal knowledge but it saves using tie and
# performance for ora_errno is very important.
*Oraperl::ora_errno  = \$DBD::Oracle::err;
*Oraperl::ora_errstr = \$DBD::Oracle::errstr;


# -----------------------------------------------------------------
#
#   $ora_verno
#   $ora_debug    not supported, use $h->debug(2) where $h is $lda or $csr
#   $ora_cache    not supported
#   $ora_long     used at ora_open()
#   $ora_trunc    used at ora_open()

$Oraperl::ora_verno = '3.000';	# to distinguish it from oraperl 2.4

$Oraperl::ora_long  = 80;	# 80, oraperl default
$Oraperl::ora_trunc = 0; 	# long trunc is error, oraperl default


# -----------------------------------------------------------------
#
# Non-oraperl extensions added here to make it easy to still run
# script using oraperl (by avoiding $csr->readblob(...))

*ora_readblob = _func_ref('st::readblob');


1;
__END__

=head1 NAME

Oraperl - Perl access to Oracle databases for old oraperl scripts

=head1 SYNOPSIS

  $lda = &ora_login($system_id, $name, $password)
  $csr = &ora_open($lda, $stmt [, $cache])
  &ora_bind($csr, $var, ...)
  &ora_fetch($csr [, $trunc])
  &ora_close($csr)
  &ora_logoff($lda)

  &ora_do($lda, $stmt)

  &ora_titles($csr)
  &ora_lengths($csr)
  &ora_types($csr)
  &ora_commit($lda)
  &ora_rollback($lda)
  &ora_autocommit($lda, $on_off)
  &ora_version

  $ora_cache
  $ora_long
  $ora_trunc
  $ora_errno
  $ora_errstr
  $ora_verno

  $ora_debug

=head1 DESCRIPTION

Oraperl is an extension to Perl which allows access to Oracle databases.

The functions which make up this extension are described in the
following sections. All functions return a false or undefined (in the
Perl sense) value to indicate failure.  You do not need to understand
the references to OCI in these descriptions. They are here to help
those who wish to extend the routines or to port them to new machines.

The text in this document is largely unchanged from the original Perl4
oraperl manual written by Kevin Stock <kstock@auspex.fr>. Any comments
specific to the DBD::Oracle Oraperl emulation are prefixed by B<DBD:>.

=head2 Principal Functions

The main functions for database access are &ora_login(), &ora_open(),
&ora_bind(), &ora_fetch(), &ora_close(), &ora_do() and &ora_logoff().

=over 2

=item * ora_login

  $lda = &ora_login($system_id, $username, $password)

In order to access information held within an Oracle database, a
program must first log in to it by calling the &ora_login() function.
This function is called with three parameters, the system ID (see
below) of the Oracle database to be used, and the Oracle username and
password. The value returned is a login identifier (actually an Oracle
Login Data Area) referred to below as $lda.

Multiple logins may be active simultaneously. This allows a simple
mechanism for correlating or transferring data between databases.

Most Oracle programs (for example, SQL*Plus or SQL*Forms) examine the
environment variable ORACLE_SID or TWO_TASK to determine which database
to connect to. In an environment which uses several different
databases, it is easy to make a mistake, and attempt to run a program
on the wrong one.  Also, it is cumbersome to create a program which
works with more than one database simultaneously. Therefore, Oraperl
requires the system ID to be passed as a parameter. However, if the
system ID parameter is an empty string then oracle will use the
existing value of ORACLE_SID or TWO_TASK in the usual manner.

Example:

  $lda = &ora_login('personnel', 'scott', 'tiger') || die $ora_errstr;

This function is equivalent to the OCI olon and orlon functions.
Really, it should have been called &ora_logon(), but I made the mistake
a long time ago and fixing it would break too many existing programs.

B<DBD:> note that TNS aliases are currently not supported. If 'foo' is
an alias for 'T:host:foo' then you must explicitly use 'T:host:foo' (or
an empty string and rely on ORACLE_SID or TWO_TASK environment variables).

B<DBD:> Since the returned $lda is a Perl5 reference the database login
identifier is now automatically released if $lda is overwritten or goes
out of scope.

=item * ora_open

  $csr = &ora_open($lda, $statement [, $cache])

To specify an SQL statement to be executed, the program must call the
&ora_open() function. This function takes at least two parameters: a
login identifier (obtained from &ora_login()) and the SQL statement to
be executed. An optional third parameter specifies the size of the row
cache to be used for a SELECT statement. The value returned from
&ora_open() is a statement identifier (actually an ORACLE Cursor)
referred to below as $csr.

If the row cache size is not specified, a default size is
used. As distributed, the default is five rows, but this
may have been changed at your installation (see the
&ora_version() function and $ora_cache variable below).

B<DBD:> currently $cache is ignored. That should change soon.

Examples:

 $csr = &ora_open($lda, 'select ename, sal from emp order by ename', 10);

 $csr = &ora_open($lda, 'insert into dept values(:1, :2, :3)');

This function is equivalent to the OCI oopen and oparse functions. For
statements which do not contain substitution variables (see the section
Substitution Variables below), it also uses of the oexec function. For
SELECT statements, it also makes use of the odescr and odefin functions
to allocate memory for the values to be returned from the database.

=item * ora_bind

  &ora_bind($csr, $var, ...)

If an SQL statement contains substitution variables (see the section
Substitution Variables below), &ora_bind() is used to assign actual
values to them. This function takes a statement identifier (obtained
from &ora_open()) as its first parameter, followed by as many
parameters as are required by the statement.

Example:

 &ora_bind($csr, 50, 'management', 'Paris');

This function is equivalent to the OCI obndrn and oexec statements.

The OCI obndrn function does not allow empty strings to be bound. As
distributed, $ora_bind therefore replaces empty strings with a single
space. However, a compilation option allows this substitution to be
suppressed, causing &ora_bind() to fail. The output from the
&ora_version() function specifies which is the case at your installation.

=item * ora_fetch

 $nfields = &ora_fetch($csr)

 @data = &ora_fetch($csr [, $trunc])

The &ora_fetch() function is used in conjunction with a SQL SELECT
statement to retrieve information from a database.  This function takes
one mandatory parameter, a statement identifier (obtained from
&ora_open()).

Used in a scalar context, the function returns the number of fields
returned by the query but no data is actually fetched. This may be
useful in a program which allows a user to enter a statement interactively.

Example:

 $nfields = &ora_fetch($csr);

Used in an array context, the value returned is an array
containing the data, one element per field.

An optional second parameter may be supplied to indicate whether the
truncation of a LONG or LONG RAW field is to be permitted (non-zero) or
considered an error (zero). If this parameter is not specified, the
value of the global variable $ora_trunc is used instead. Truncation of
other datatypes is always considered a error.

B<DBD:> The optional second parameter to ora_fetch is not supported.
A DBI usage error will be generated if a second parameter is supplied.
Use the global variable $ora_trunc instead. Also note that the
experimental DBI readblob method can be used to retrieve a long:

  $csr->readblob($field, $offset, $len [, \$dest, $destoffset]);

If truncation occurs, $ora_errno will be set to 1406.  &ora_fetch()
will complete successfully if truncation is permitted, otherwise it
will fail.

&ora_fetch() will fail at the end of the data or if an error occurs. It
is possible to distinguish between these cases by testing the value of
the variable $ora_errno. This will be zero for end of data, non-zero if
an error has occurred.

Example:

 while (($deptno, $dname, $loc) = &ora_fetch($csr))
 {
   warn "Truncated!!!" if $ora_errno == 1406;
   # do something with the data
 }
 warn $ora_errstr if $ora_errno;

This function is equivalent to the OCI ofetch function.

=item * ora_close

 &ora_close($csr)

If an SQL statement is no longer required (for example, all the data
selected has been processed, or no more rows are to be inserted) then
the statement identifier should be released. This is done by calling
the &ora_close() function with the statement identifier as its only
parameter.

This function is equivalent to the OCI oclose function.

B<DBD:> Since $csr is a Perl5 reference the statement/cursor is now
automatically closed if $csr is overwritten or goes out of scope.


=item * ora_do

  &ora_do($lda, $statement)

Not all SQL statements return data or contain substitution
variables. In these cases the &ora_do() function may be
used as an alternative to &ora_open() and &ora_close().
This function takes two parameters, a login identifier and
the statement to be executed.

Example:

 &ora_do($lda, 'drop table employee');

This function is roughly equivalent to

 &ora_close( &ora_open($lda, $statement) )

B<DBD:> oraperl v2 used to return the string 'OK' to indicate
success with a zero numeric value. The Oraperl emulation now
uses the string '0E0' to achieve the same effect since it does
not cause any C<-w> warnings when used in a numeric context.

=item * ora_logoff

  &ora_logoff($lda)

When the program no longer needs to access a given database, the login
identifier should be released using the &ora_logoff() function.

This function is equivalent to the OCI ologoff function.

B<DBD:> Since $lda is a Perl5 reference the database login identifier
is now automatically released if $lda is overwritten or goes out of scope.

=back

=head2 Ancillary Functions

Additional functions available are: &ora_titles(),
&ora_lengths(), &ora_types(), &ora_autocommit(),
&ora_commit(), &ora_rollback() and &ora_version().

The first three are of most use within a program which
allows statements to be entered interactively. See, for
example, the sample program sql which is supplied with
Oraperl and may have been installed at your site.

=over 2

=item * ora_titles

  @titles = &ora_titles($csr)

A program may determine the field titles of an executed
query by calling &ora_titles(). This function takes a
single parameter, a statement identifier (obtained from
&ora_open()) indicating the query for which the titles are
required. The titles are returned as an array of strings,
one for each column.

Titles are truncated to the length of the field, as reported
by the &ora_lengths() function.

B<DBD:> oraperl v2.2 actually changed the behaviour such that the
titles were not truncated unless an optional second parameter was
true.  This was not reflected in the oraperl manual.  The Oraperl
emulation adopts the non truncating behaviour and doesn't support the
truncate parameter.


=item * ora_lengths

  @lengths = &ora_lengths($csr)

A program may determine the length of each of the fields
returned by a query by calling the &ora_lengths() function.
This function takes a single parameter, a statement
identifier (obtained from &ora_open()) indicating the query
for which the lengths are required. The lengths are
returned as an array of integers, one for each column.


=item * ora_types

  @types = &ora_types($csr)

A program may determine the type of each of the fields returned by a
query by calling the &ora_types() function.  This function takes a
single parameter, a statement identifier (obtained from &ora_open())
indicating the query for which the lengths are required. The types are
returned as an array of integers, one for each field.

These types are defined in your OCI documentation. The correct
interpretation for Oracle v6 is given in the file oraperl.ph.


=item * ora_autocommit

  &ora_autocommit($lda, $on_or_off)

Autocommit mode (in which each transaction is committed immediately,
without waiting for an explicit commit) may be enabled or disabled
using &ora_autocommit(). This function takes two parameters, a login
identifier (obtained from &ora_login()) and a true/false value
indicating whether autocommit is to be enabled (non-zero) or disabled
(zero).  By default, autocommit is off.

Note that autocommit can only be set per login, not per statement. If
you need to control autocommit by statement (for example, to allow
deletions to be rolled back, but insertions to be committed
immediately) you should make multiple calls to &ora_login() and use a
separate login identifier for each statement.


=item * ora_commit, ora_rollback

  &ora_commit($lda)
  &ora_rollback($lda)

Modifications to a database may be committed or rolled back using the
&ora_commit() and &ora_rollback() functions.  These functions take a
single parameter, a login identifier obtained from &ora_login().

Transactions which have been committed (either explicitly by a call to
&ora_commit() or implicitly through the use of &ora_autocommit())
cannot be subsequently rolled back.

Note that commit and rollback can only be used per login, not per
statement. If you need to commit or rollback by statement you should
make multiple calls to &ora_login() and use a separate login identifier
for each statement.


=item * ora_version

  &ora_version()

The &ora_version() function prints the version number and
copyright information concerning Oraperl. It also prints
the values of various compilation time options. It does not
return any value, and should not normally be used in a
program.

Example:

  oraperl -e '&ora_version'

  This is Oraperl, version 2, patch level 0.

  Debugging is available, including the -D flag.
  Default fetch row cache size is 5.
  Empty bind values are replaced by a space.

  Perl is copyright by Larry Wall; type oraperl -v for details.
  Additions for oraperl: Copyright 1991, 1992, Kevin Stock.

  Oraperl may be distributed under the same conditions as Perl.

This function is the equivalent of Perl's C<-v> flag.

B<DBD:> The Oraperl emulation printout is similar but not identical.

=back

=head1 VARIABLES

Six special variables are provided, $ora_cache, $ora_long,
$ora_trunc, $ora_errno, $ora_errstr and $ora_verno.

=head2 Customisation Variables

These variables are used to dictate the behaviour of Oraperl
under certain conditions.

=over 2

=item * $ora_cache

The $ora_cache variable determines the default cache size used by the
&ora_open() function for SELECT statements if an explicit cache size is
not given.

It is initialised to the default value reported by &ora_version() but
may be set within a program to apply to all subsequent calls to
&ora_open(). Cursors which are already open are not affected. As
distributed, the default value is five, but may have been altered at
your installation.

As a special case, assigning zero to $ora_cache resets it to the
default value. Attempting to set $ora_cache to a negative value results
in a warning.

B<DBD:> Not yet supported.


=item * $ora_long

Normally, Oraperl interrogates the database to determine the length of
each field and allocates buffer space accordingly.  This is not
possible for fields of type LONG or LONGRAW. To allocate space
according to the maximum possible length (65535 bytes) would obviously
be extremely wasteful of memory.

Therefore, when &ora_open() determines that a field is a LONG type, it
allocates the amount of space indicated by the $ora_long variable. This
is initially set to 80 (for compatibility with Oracle products) but may
be set within a program to whatever size is required.


=item * $ora_trunc

Since Oraperl cannot determine exactly the maximum length of a LONG
field, it is possible that the length indicated by $ora_long is not
sufficient to store the data fetched. In such a case, the optional
second parameter to &ora_fetch() indicates whether the truncation
should be allowed or should provoke an error.

If this second parameter is not specified, the value of $ora_trunc is
used as a default. This only applies to LONG and LONGRAW data types.
Truncation of a field of any other type is always considered an error
(principally because it indicates a bug in Oraperl).

=back

=head2 Status Variables

These variables report information about error conditions or about
Oraperl itself. They may only be read; a fatal error occurs if a
program attempts to change them.

=over 2

=item * $ora_errno

$ora_errno contains the Oracle error code provoked by the last function
call.

There are two cases of particular interest concerning &ora_fetch(). If
a LONG or LONGRAW field is truncated (and truncation is allowed) then
&ora_fetch() will complete successfully but $ora_errno will be set to
1406 to indicate the truncation. When &ora_fetch() fails, $ora_errno
will be set to zero if this was due to the end of data or an error code
if it was due to an actual error.


=item * $ora_errstr

The $ora_errstr variable contains the Oracle error message
corresponding to the current value of $ora_errno.


=item * $ora_verno

The $ora_verno variable contains the version number of Oraperl in the
form v.ppp where v is the major version number and ppp is the
patchlevel. For example, in Oraperl version 3, patch level 142,
$ora_verno would contain the value 3.142 (more or less, allowing for
floating point error).

=back


=head1 SUBSTITUTION VARIABLES

Oraperl allows an SQL statement to contain substitution variables.
These consist of a colon followed by a number.  For example, a program
which added records to a telephone list might use the following call to
&ora_open():

  $csr = &ora_open($csr, "insert into telno values(:1, :2)");

The two names :1 and :2 are called substitution variables.  The
function &ora_bind() is used to assign values to these variables. For
example, the following statements would add two new people to the
list:

  &ora_bind($csr, "Annette", "472-8836");
  &ora_bind($csr, "Brian", "937-1823");

Note that the substitution variables must be assigned consecutively
beginning from 1 for each SQL statement, as &ora_bind() assigns its
parameters in this order. Named substitution variables (for example,
:NAME, :TELNO) are not permitted.

=head1 DEBUGGING

B<DBD:> The Oraperl $ora_debug variable is not supported. However
detailed debugging can be enabled at any time by executing

  $h->debug(2);

where $h is either a $lda or a $csr. If debugging is enabled on an
$lda then it is automatically passed on to any cursors returned by
&ora_open().

=head1 EXAMPLE

  format STDOUT_TOP =
  Name Phone
  ==== =====
  .

  format STDOUT =
  @<<<<<<<<<< @>>>>>>>>>>
  $name, $phone
  .

  die "You should use oraperl, not perl\n" unless defined &ora_login;
  $ora_debug = shift if $ARGV[0] =~ /^\-#/;

  $lda = &ora_login('t', 'kstock', 'kstock')
            || die $ora_errstr;
  $csr = &ora_open($lda, 'select * from telno order by name')
            || die $ora_errstr;

  $nfields = &ora_fetch($csr);
  print "Query will return $nfields fields\n\n";

  while (($name, $phone) = &ora_fetch($csr)) { write; }
  warn $ora_errstr if $ora_errno;

  die "fetch error: $ora_errstr" if $ora_errno;

  do ora_close($csr) || die "can't close cursor";
  do ora_logoff($lda) || die "can't log off Oracle";


=head1 NOTES

In keeping with the philosophy of Perl, there is no pre-defined limit
to the number of simultaneous logins or SQL statements which may be
active, nor to the number of data fields which may be returned by a
query. The only limits are those imposed by the amount of memory
available, or by Oracle.


=head1 WARNINGS

The Oraperl emulation software shares no code with the original
oraperl. It is built on top of the new Perl5 DBI and DBD::Oracle
modules.  These modules are still evolving. (One of the goals of
the Oraperl emulation software is to allow useful work to be done
with the DBI and DBD::Oracle modules whilst insulating users from
the ongoing changes in their interfaces.)

It is quite possible, indeed probable, that some differences in
behaviour will exist. This should be confined to error handling.

B<All> differences in behaviour which are not documented here should be
reported to Tim.Bunce@ig.co.uk and CC'd to dbi-users@fugue.com.


=head1 SEE ALSO

=over 2

=item Oracle Documentation

SQL Language Reference Manual.
Programmer's Guide to the Oracle Call Interfaces.

=item Books

Programming Perl by Larry Wall and Randal Schwartz.
Learning Perl by Randal Schwartz.

=item Manual Pages

perl(1)

=back

=head1 AUTHORS

Perl by Larry Wall <lwall@netlabs.com>.

ORACLE by Oracle Corporation, California.

Original Oraperl 2.4 code and documentation
by Kevin Stock <kstock@auspex.fr>.

DBI and Oraperl emulation using DBD::Oracle
by <Tim.Bunce@ig.co.uk>

=cut
