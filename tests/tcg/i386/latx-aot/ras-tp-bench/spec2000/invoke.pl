use strict;
use warnings;
use JSON::PP;
use File::Find;
use Cwd 'abs_path';

my ($object, $directory, $size, $exe) = @ARGV;
$object = abs_path($object);
chdir $directory or die "$directory: $!";
our ($reltol, $abstol, $obiwan, $compress_whitespace);
sub read_file {
    open my $file, '<', $_[0] or die "$!: $_[0]";
    return <$file>;
}
sub Log { die join(' ', @_); }
{
    package Bench;
    sub exe_file { $_[0]->{exe} }
    sub size { $_[0]->{size} }
    sub endian { '12345678' }
    sub name { $_[0]->{exe} }
    sub benchmark { $_[0]->{exe} }
    sub OS { 'unix' }
    sub input_files_base { @{$_[0]->{files}} }
}
my @files;
find({no_chdir => 1, wanted => sub {
    push @files, substr($File::Find::name, 2) if -f $_;
}}, '.');
@files = sort @files;
# The licensed object file declares package globals without strict vars.
{ no strict 'vars'; do $object or die "$object: $@ $!"; }
my $me = bless {exe => $exe, size => $size, files => \@files}, 'Bench';
no strict 'refs';
my @commands = main::invoke($me);
print encode_json({commands => \@commands, reltol => $reltol,
    abstol => $abstol, obiwan => $obiwan,
    compress_whitespace => $compress_whitespace});
