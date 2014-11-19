requires "DBI" => "0";
requires "DynaLoader" => "0";
requires "Exporter" => "0";
requires "perl" => "5.006";
requires "strict" => "0";
requires "warnings" => "0";

on 'test' => sub {
  requires "B" => "0";
  requires "Carp" => "0";
  requires "Data::Dumper" => "0";
  requires "Devel::Peek" => "0";
  requires "Encode" => "0";
  requires "Math::BigInt" => "0";
  requires "Test::More" => "0.88";
  requires "Thread::Semaphore" => "0";
  requires "lib" => "0";
  requires "utf8" => "0";
  requires "vars" => "0";
};

on 'configure' => sub {
  requires "DBI" => "1.51";
  requires "ExtUtils::MakeMaker" => "0";
};

on 'develop' => sub {
  requires "version" => "0.9901";
};
