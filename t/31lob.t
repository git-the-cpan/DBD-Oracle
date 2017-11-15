#!/usr/bin/perl

use strict;
use Test::More tests => 12;
use DBD::Oracle qw(:ora_types);
use DBI;

unshift @INC ,'t';
require 'nchar_test_lib.pl';

my $dbh;
$| = 1;
SKIP: {

    $dbh = db_handle();
    plan skip_all => "Not connected to oracle" if not $dbh;

    my $table = table();
    drop_table($dbh);
    
    $dbh->do(qq{
	CREATE TABLE $table (
	    id INTEGER NOT NULL,
	    data BLOB
	)
    });

my ($stmt, $sth, $id, $loc);
## test with insert empty blob and select locator.
$stmt = "INSERT INTO $table (id,data) VALUES (1, EMPTY_BLOB())";
$dbh->do($stmt);

$stmt = "SELECT data FROM $table WHERE id = ?";
$sth = $dbh->prepare($stmt, {ora_auto_lob => 0});
$id = 1;
$sth->bind_param(1, $id);
$sth->execute;
($loc) = $sth->fetchrow;
is (ref $loc, "OCILobLocatorPtr", "returned valid locator");

## test inserting a large value

$stmt = "INSERT INTO $table (id,data) VALUES (666, ?)";
$sth = $dbh->prepare($stmt);
my $content = join(q{}, map { chr } ( 32 .. 64 )) x 16384;
$sth->bind_param(1, $content, { ora_type => ORA_BLOB, ora_field => 'data' });
eval { $sth->execute($content) };
is $@, '', 'inserted into BLOB successfully';
{
  local $dbh->{LongReadLen} = 1_000_000;
  my ($fetched) = $dbh->selectrow_array("select data from $table where id = 666");
  is $fetched, $content, 'got back what we put in';
}


## test with insert empty blob returning blob to a var.
($id, $loc) = (2, undef);
$stmt = "INSERT INTO $table (id,data) VALUES (?, EMPTY_BLOB()) RETURNING data INTO ?";
$sth = $dbh->prepare($stmt, {ora_auto_lob => 0});
$sth->bind_param(1, $id);
$sth->bind_param_inout(2, \$loc, 0, {ora_type => ORA_BLOB});
$sth->execute;
is (ref $loc, "OCILobLocatorPtr", "returned valid locator");

sub temp_lob_count {
    my $dbh  = shift;
    my $stmt = "
     SELECT cache_lobs + nocache_lobs AS temp_lob_count
     FROM v\$temporary_lobs templob,
          v\$session sess
     WHERE sess.sid = templob.sid
     AND sess.audsid = userenv('sessionid') ";
    my ($count) = $dbh->selectrow_array($stmt);
    return $count;
}

sub have_v_session {
 
 $dbh->do('select * from v$session where 0=1');
 if ($dbh->err){
   return if ($dbh->err == 942);
 }
 return 1;
}



## test writing / reading large data
{
    # LOB locators cannot span transactions - turn off AutoCommit
    local $dbh->{AutoCommit} = 0;
    my ( $large_value, $len );

    # get a new locator
    $stmt = "INSERT INTO $table (id,data) VALUES (3, EMPTY_BLOB())";
    $dbh->do($stmt);
    $stmt = "SELECT data FROM $table WHERE id = ?";
    $sth  = $dbh->prepare( $stmt, { ora_auto_lob => 0 } );
    $id   = 3;
    $sth->bind_param( 1, $id );
    $sth->execute;
    ($loc) = $sth->fetchrow;

    is( ref $loc, "OCILobLocatorPtr", "returned valid locator" );

    is( $dbh->ora_lob_is_init($loc), 1, "returned initialized locator" );
  

    # write string > 32k
    $large_value = 'ABCD' x 10_000;

    $dbh->ora_lob_write( $loc, 1, $large_value );
    is( $dbh->ora_lob_length($loc), length($large_value), "returned length" );
    is( $dbh->ora_lob_read( $loc, 1, length($large_value) ),
        $large_value, "returned written value" );

    ## PL/SQL TESTS
  SKIP: {
    ## test calling PL/SQL with LOB placeholder
        my $plsql_testcount = 4;

        $stmt = "BEGIN ? := DBMS_LOB.GETLENGTH( ? ); END;";
        $sth = $dbh->prepare( $stmt, { ora_auto_lob => 0 } );
        $sth->bind_param_inout( 1, \$len, 16 );
        $sth->bind_param( 2, $loc, { ora_type => ORA_BLOB } );
        $sth->execute;

        # ORA-00600: internal error code
        # ORA-00900: invalid SQL statement
        # ORA-06550: PLS-00201: identifier 'DBMS_LOB.GETLENGTH' must be declared
        # ORA-06553: PLS-00213: package STANDARD not accessible

        if ( $dbh->err && grep { $dbh->err == $_ } ( 600, 900, 6550, 6553 ) ) {
            skip "Your Oracle server doesn't support PL/SQL", $plsql_testcount
              if $dbh->err == 900;
            skip
              "Your Oracle PL/SQL package DBMS_LOB is not properly installed", $plsql_testcount
              if $dbh->err == 6550;
            skip "Your Oracle PL/SQL is not properly installed", $plsql_testcount
              if $dbh->err == 6553 || $dbh->err == 600;
        }

        is( $len, length($large_value), "returned length via PL/SQL" );


        
        $stmt = "
  DECLARE
    --  testing IN, OUT, and IN OUT:
    --  p_out   will be set to LOWER(p_in)
    --  p_inout will be set to p_inout || p_in

    PROCEDURE lower_lob(p_in BLOB, p_out OUT BLOB, p_inout IN OUT BLOB) IS
      pos INT;
      buffer RAW(1024);
    BEGIN
      DBMS_LOB.CREATETEMPORARY(p_out, TRUE);
      pos := 1;
      WHILE pos <= DBMS_LOB.GETLENGTH(p_in)
      LOOP
        buffer := DBMS_LOB.SUBSTR(p_in, 1024, pos);

        DBMS_LOB.WRITEAPPEND(p_out, UTL_RAW.LENGTH(buffer), 
          UTL_RAW.CAST_TO_RAW(LOWER(UTL_RAW.CAST_TO_VARCHAR2(buffer))));

        DBMS_LOB.WRITEAPPEND(p_inout, UTL_RAW.LENGTH(buffer), buffer);

        pos := pos + 1024;
      END LOOP;
    END;
  BEGIN
    lower_lob(:in, :out, :inout);
  END; ";

        my $out;
        my $inout = lc $large_value;

        local $dbh->{LongReadLen} = length($large_value) * 2;

        $sth = $dbh->prepare( $stmt, { ora_auto_lob => 1 } );
        $sth->bind_param( ':in', $large_value, { ora_type => ORA_BLOB });
        $sth->bind_param_inout( ':out', \$out, 100, { ora_type => ORA_BLOB } );
        $sth->bind_param_inout( ':inout', \$inout, 100, { ora_type => ORA_BLOB } );
        $sth->execute;

        skip "Your Oracle PL/SQL installation does not implement temporary LOBS", 3
          if $dbh->err && $dbh->err == 6550;

        is($out, lc($large_value), "returned LOB as string");
        is($inout, lc($large_value).$large_value, "returned IN/OUT LOB as string");

        undef $sth;
        # lobs are freed with statement handle
        skip q{can't check num of temp lobs, no access to v$session}, 1, unless have_v_session();
        is(temp_lob_count($dbh), 0, "no temp lobs left");
    }
}

$dbh->do("DROP TABLE $table");
$dbh->disconnect;

}

1;
