--TEST--
Bug #8009 (cannot add again same entry to an archive)
--SKIPIF--
<?php
/* $Id: bug8009.phpt,v 1.1.2.1 2007/05/07 18:03:00 tony2001 Exp $ */
if(!extension_loaded('zip')) die('skip');
?>
--FILE--
<?php
$thisdir = dirname(__FILE__);
$src = $thisdir . "/bug8009.zip";
$filename = $thisdir . "/tmp8009.zip";
copy($src, $filename);

$zip = new ZipArchive();

if (!$zip->open($filename)) {
       exit("cannot open $filename\n");
}
$zip->addFromString("2.txt", "=)");
$zip->close();
unlink($filename);
echo "status: " . $zip->status . "\n";
echo "\n";

--EXPECT--
status: 0
