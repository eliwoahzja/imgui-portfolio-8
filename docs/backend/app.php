<?php
// Single entry point for the loader.
// Moved out of /client/ so brute-force on the directory yields nothing.
// All requests: /app.php?op=<encrypted>&a=...&b=...[&c=...]
//
// The real endpoint scripts live inside /client/ which is fully blocked
// by .htaccess (deny from all). Only this gateway can include them.

include_once(__DIR__ . '/client/structure.php');

header('Content-Type: text/plain; charset=UTF-8');
header('Cache-Control: no-store, no-cache, must-revalidate, max-age=0');
header('Pragma: no-cache');

function api_fail_cipher()
{
    global $aes;
    die($aes::encrypt('data_error', $aes::$a, $aes::$b));
}

if (!isset($_GET['op']))
    api_fail_cipher();

$op_raw = $aes::decrypt(
    $helper::fix_request($_GET['op']),
    $aes::$a,
    $aes::$b
);

if (!is_string($op_raw) || $op_raw === '')
    api_fail_cipher();

$allowed = [
    'authentication' => 'authentication.php',
    'version'        => 'version.php',
    'activation'     => 'activation.php',
    'mapper_init'    => 'mapper.php',
    'mapper_chunk'   => 'mapper.php',
    'products'       => 'products.php',
    'profile'        => 'profile.php',
    'cloudcfg_list'  => 'cloudcfg.php',
    'cloudcfg_get'   => 'cloudcfg.php',
    'cloudcfg_put'   => 'cloudcfg.php',
    'cloudcfg_delete'=> 'cloudcfg.php',
    'cloudcfg_rename'=> 'cloudcfg.php',
    'cloudcfg_save'  => 'cloudcfg.php',
    'cloudcfg_load'  => 'cloudcfg.php',
];

if (!isset($allowed[$op_raw]))
    api_fail_cipher();

$target = __DIR__ . '/client/' . $allowed[$op_raw];
if (!file_exists($target))
    api_fail_cipher();

require $target;
?>
