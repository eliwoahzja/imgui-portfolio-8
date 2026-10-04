<?php
// Cloud config endpoint — share codes backed by their own database.
//
// A share code is no longer the config itself: it is a random base36 key into
// the cloud config table, so the code stays 20 chars while the payload can
// be any size — and the same store serves every game, not just this menu
// (that is why `game` is part of the key). The owner's username and uid are
// resolved server-side from the XenForo account behind the loader token; the
// client never sends its own identity.
//
// Two operations (routed from app.php via $_GET['op']):
//   cloudcfg_save — store the caller's config, answer with a fresh code
//       a=<AES(token)>  b=<AES(game)>  c=<AES(name_b64 . '.' . payload_b64)>
//       reply: ok;<code>
//   cloudcfg_load — fetch a config by its code
//       a=<AES(token)>  b=<AES(game)>  c=<AES(code)>
//       reply: ok;<code>;<uid>;<name_b64>;<username_b64>;<payload_b64>
//
// Codes are immutable snapshots: every save mints a new code and nothing can
// rewrite the payload behind a code that is already shared around.
//
// Error codes: data_error | invalid_credentials | banned | rate_limited |
//              bad_request | not_found | too_large | limit_reached
include_once('structure.php');

if ($loader_cipher === null || $loader_iv === null)
{
    error_log('cloudcfg: data_error at session_keys');
    die($aes::encrypt('data_error', $aes::$a, $aes::$b));
}

$cipher = $loader_cipher;
$iv     = $loader_iv;

function cloudcfg_fail($code, $context = '')
{
    global $aes, $cipher, $iv;
    if ($code === 'data_error')
        error_log('cloudcfg: data_error at ' . ($context !== '' ? $context : 'unknown'));
    die($aes::encrypt($code, $cipher, $iv));
}

// -----------------------------------------------------------------------
// Input contract.
//
// Everything below arrives from the client, so nothing is used before it has
// matched an explicit whitelist. The alphabets are deliberately narrower than
// "whatever the column can store": they describe exactly what the menu is able
// to produce, so anything else is a bug or an attack and is rejected outright
// instead of being escaped and kept. Queries are prepared regardless — the
// patterns are the first wall, not the only one.
// -----------------------------------------------------------------------
const CLOUDCFG_RE_TOKEN = '/^[A-Za-z0-9]{16,64}$/';
const CLOUDCFG_RE_GAME  = '/^[a-z0-9_\-]{1,32}$/';
const CLOUDCFG_RE_CODE  = '/^[a-z0-9]{16,24}$/';
// Mirrors sanitize() in the menu's config_store.cpp: a name that could not have
// come out of the config page is not a name.
const CLOUDCFG_RE_NAME  = '/^[A-Za-z0-9 _\-]{1,48}$/';
const CLOUDCFG_RE_B64   = '#^[A-Za-z0-9+/]+={0,2}$#';

// Ceiling on an encrypted parameter, checked before decryption: an oversized
// `c` would otherwise buy an AES pass over however much was sent.
const CLOUDCFG_MAX_PARAM = 131072;

// Decrypts a GET parameter, or returns null if it is absent, oversized or does
// not decrypt to a non-empty string.
function cloudcfg_param($key)
{
    global $aes, $helper, $cipher, $iv;

    if (!isset($_GET[$key]) || !is_string($_GET[$key]))
        return null;
    if ($_GET[$key] === '' || strlen($_GET[$key]) > CLOUDCFG_MAX_PARAM)
        return null;

    $plain = $aes::decrypt($helper::fix_request($_GET[$key]), $cipher, $iv);
    return is_string($plain) && $plain !== '' ? $plain : null;
}

// The store may use the XenForo schema or a separately provisioned one.
function cloudcfg_connect()
{
    static $cfg = null;
    if ($cfg === null)
    {
        $cfg = @mysqli_connect(CFG_DB_HOST, CFG_DB_USER, CFG_DB_PASS, CFG_DB_NAME);
        if (!$cfg)
        {
            error_log('cloudcfg: cannot connect to ' . CFG_DB_NAME . ': ' . mysqli_connect_error());
            $cfg = false;
        }
        else
        {
            mysqli_set_charset($cfg, 'utf8mb4');
        }
    }
    return $cfg;
}

function cloudcfg_generate_code($len = 20)
{
    $alphabet = 'abcdefghijklmnopqrstuvwxyz0123456789';
    $out = '';
    while (strlen($out) < $len)
    {
        foreach (str_split(random_bytes(16)) as $byte)
        {
            $v = ord($byte);
            // 252 = 7 * 36: reject the tail so the modulo stays uniform.
            if ($v >= 252)
                continue;
            $out .= $alphabet[$v % 36];
            if (strlen($out) === $len)
                break;
        }
    }
    return $out;
}

$conn = $helper::connect();
if (!$conn)
    cloudcfg_fail('data_error', 'xf_connect');

$client_ip = get_client_ip();
if (check_rate_limit($conn, $client_ip, 30, 300))
    cloudcfg_fail('rate_limited');

// app.php routes both ops here and leaves the decrypted name in $op_raw.
$cloudcfg_op = isset($op_raw) && is_string($op_raw) ? $op_raw : '';
$cloudcfg_ops = [
    'cloudcfg_list', 'cloudcfg_get', 'cloudcfg_put',
    'cloudcfg_delete', 'cloudcfg_rename',
    'cloudcfg_save', 'cloudcfg_load'
];
if (!in_array($cloudcfg_op, $cloudcfg_ops, true))
    cloudcfg_fail('bad_request');

// -----------------------------------------------------------------------
// Who is calling. Same two paths as profile.php: a loader token, or an
// already-activated sid session. Loads too — anonymous code scraping would
// otherwise let anyone enumerate the keyspace for free.
// -----------------------------------------------------------------------
$user_id = 0;

if (isset($_GET['a']))
{
    $token = cloudcfg_param('a');
    if ($token === null)
        cloudcfg_fail('data_error', 'token_decrypt');

    // Tokens are 64 hex chars from panel/index.php. Anything that cannot be one
    // never reaches the database, so a crafted `a` fails here rather than on a
    // lookup that returns nothing.
    if (!preg_match(CLOUDCFG_RE_TOKEN, $token))
    {
        record_login_attempt($conn, $client_ip);
        cloudcfg_fail('invalid_credentials');
    }

    $st = mysqli_prepare($conn,
        "SELECT id, user_id, revoked FROM loader_tokens WHERE token = ? LIMIT 1");
    if (!$st)
        cloudcfg_fail('data_error', 'token_prepare');

    mysqli_stmt_bind_param($st, 's', $token);
    if (!@mysqli_stmt_execute($st))
    {
        mysqli_stmt_close($st);
        cloudcfg_fail('data_error', 'token_execute');
    }

    mysqli_stmt_bind_result($st, $token_id, $token_user_id, $token_revoked);
    $have_token = mysqli_stmt_fetch($st);
    mysqli_stmt_close($st);

    if (!$have_token || (int)$token_revoked === 1)
    {
        record_login_attempt($conn, $client_ip);
        cloudcfg_fail('invalid_credentials');
    }

    $user_id = (int)$token_user_id;

    $st = mysqli_prepare($conn,
        "UPDATE loader_tokens SET last_used_at = NOW(), last_ip = ? WHERE id = ?");
    if ($st)
    {
        mysqli_stmt_bind_param($st, 'si', $client_ip, $token_id);
        @mysqli_stmt_execute($st);
        mysqli_stmt_close($st);
    }
}
else if (loader_is_activated())
{
    $user_id = loader_user_id();
}

if ($user_id <= 0)
    cloudcfg_fail('invalid_credentials');

$st = mysqli_prepare($conn,
    "SELECT username, user_state, is_banned FROM xf_user WHERE user_id = ? LIMIT 1");
if (!$st)
    cloudcfg_fail('data_error', 'user_prepare');

mysqli_stmt_bind_param($st, 'i', $user_id);
if (!@mysqli_stmt_execute($st))
{
    mysqli_stmt_close($st);
    cloudcfg_fail('data_error', 'user_execute');
}

mysqli_stmt_bind_result($st, $username, $user_state, $user_banned);
$have_user = mysqli_stmt_fetch($st);
mysqli_stmt_close($st);

if (!$have_user)
    cloudcfg_fail('invalid_credentials');

if ((int)$user_banned === 1 || $user_state !== 'valid')
    cloudcfg_fail('banned');

// -----------------------------------------------------------------------
// Common parameter: which game's config store to hit.
// -----------------------------------------------------------------------
$game = cloudcfg_param('b');
if ($game === null || !preg_match(CLOUDCFG_RE_GAME, $game))
    cloudcfg_fail('bad_request');

$cfg = cloudcfg_connect();
if (!$cfg)
    cloudcfg_fail('data_error', 'config_connect');

function cloudcfg_decode_b64($encoded)
{
    if (!is_string($encoded) || !preg_match(CLOUDCFG_RE_B64, $encoded))
        return false;
    return base64_decode($encoded, true);
}

function cloudcfg_decode_name($encoded)
{
    $name = cloudcfg_decode_b64($encoded);
    if ($name === false)
        return false;
    $name = trim($name);
    return preg_match(CLOUDCFG_RE_NAME, $name) ? $name : false;
}

function cloudcfg_validate_payload($payload_b64)
{
    if (!is_string($payload_b64) || !preg_match(CLOUDCFG_RE_B64, $payload_b64))
        return false;
    if (strlen($payload_b64) > (int)(CLOUDCFG_MAX_PAYLOAD * 4 / 3) + 4)
        return false;

    $payload = base64_decode($payload_b64, true);
    if ($payload === false || $payload === '' || strlen($payload) > CLOUDCFG_MAX_PAYLOAD)
        return false;
    if (!preg_match('/^[\x20-\x7E\r\n]+$/', $payload))
        return false;
    return true;
}

// -----------------------------------------------------------------------
// Private named config library.
// -----------------------------------------------------------------------
if ($cloudcfg_op === 'cloudcfg_list')
{
    $st = mysqli_prepare($cfg,
        "SELECT name, UNIX_TIMESTAMP(updated_at)
         FROM loader_user_configs
         WHERE game = ? AND uid = ?
         ORDER BY updated_at DESC, id DESC
         LIMIT " . (int)CLOUDCFG_MAX_NAMED_PER_USER);
    if (!$st)
        cloudcfg_fail('data_error', 'named_list_prepare');

    mysqli_stmt_bind_param($st, 'si', $game, $user_id);
    if (!@mysqli_stmt_execute($st))
    {
        mysqli_stmt_close($st);
        cloudcfg_fail('data_error', 'named_list_execute');
    }

    mysqli_stmt_bind_result($st, $row_name, $row_updated);
    $rows = [];
    while (mysqli_stmt_fetch($st))
        $rows[] = [base64_encode($row_name), (int)$row_updated];
    mysqli_stmt_close($st);

    $reply = 'ok;' . count($rows);
    foreach ($rows as $row)
        $reply .= ';' . $row[0] . ';' . $row[1];
    die($aes::encrypt($reply, $cipher, $iv));
}

if ($cloudcfg_op === 'cloudcfg_get')
{
    $name_b64 = cloudcfg_param('c');
    $name = cloudcfg_decode_name($name_b64);
    if ($name === false)
        cloudcfg_fail('bad_request');

    $st = mysqli_prepare($cfg,
        "SELECT name, payload, UNIX_TIMESTAMP(updated_at)
         FROM loader_user_configs
         WHERE game = ? AND uid = ? AND name = ? LIMIT 1");
    if (!$st)
        cloudcfg_fail('data_error', 'named_get_prepare');

    mysqli_stmt_bind_param($st, 'sis', $game, $user_id, $name);
    if (!@mysqli_stmt_execute($st))
    {
        mysqli_stmt_close($st);
        cloudcfg_fail('data_error', 'named_get_execute');
    }

    mysqli_stmt_bind_result($st, $row_name, $row_payload, $row_updated);
    $found = mysqli_stmt_fetch($st);
    mysqli_stmt_close($st);
    if (!$found)
        cloudcfg_fail('not_found');

    die($aes::encrypt('ok;' . base64_encode($row_name) . ';'
        . (int)$row_updated . ';' . $row_payload, $cipher, $iv));
}

if ($cloudcfg_op === 'cloudcfg_put')
{
    $blob = cloudcfg_param('c');
    $dot = is_string($blob) ? strpos($blob, '.') : false;
    if ($dot === false)
        cloudcfg_fail('bad_request');

    $name_b64 = substr($blob, 0, $dot);
    $payload_b64 = substr($blob, $dot + 1);
    $name = cloudcfg_decode_name($name_b64);
    if ($name === false || !cloudcfg_validate_payload($payload_b64))
        cloudcfg_fail('bad_request');

    $st = mysqli_prepare($cfg,
        "SELECT COUNT(*) FROM loader_user_configs WHERE game = ? AND uid = ?");
    if (!$st)
        cloudcfg_fail('data_error', 'named_count_prepare');
    mysqli_stmt_bind_param($st, 'si', $game, $user_id);
    if (!@mysqli_stmt_execute($st))
    {
        mysqli_stmt_close($st);
        cloudcfg_fail('data_error', 'named_count_execute');
    }
    mysqli_stmt_bind_result($st, $named_count);
    mysqli_stmt_fetch($st);
    mysqli_stmt_close($st);

    $st = mysqli_prepare($cfg,
        "SELECT id FROM loader_user_configs
         WHERE game = ? AND uid = ? AND name = ? LIMIT 1");
    if (!$st)
        cloudcfg_fail('data_error', 'named_exists_prepare');
    mysqli_stmt_bind_param($st, 'sis', $game, $user_id, $name);
    if (!@mysqli_stmt_execute($st))
    {
        mysqli_stmt_close($st);
        cloudcfg_fail('data_error', 'named_exists_execute');
    }
    mysqli_stmt_store_result($st);
    $exists = mysqli_stmt_num_rows($st) > 0;
    mysqli_stmt_close($st);

    if (!$exists && (int)$named_count >= CLOUDCFG_MAX_NAMED_PER_USER)
        cloudcfg_fail('limit_reached');

    $st = mysqli_prepare($cfg,
        "INSERT INTO loader_user_configs (game, uid, name, payload)
         VALUES (?, ?, ?, ?)
         ON DUPLICATE KEY UPDATE payload = VALUES(payload), updated_at = NOW()");
    if (!$st)
        cloudcfg_fail('data_error', 'named_put_prepare');
    mysqli_stmt_bind_param($st, 'siss', $game, $user_id, $name, $payload_b64);
    if (!@mysqli_stmt_execute($st))
    {
        error_log('cloudcfg: named put failed: ' . mysqli_error($cfg));
        mysqli_stmt_close($st);
        cloudcfg_fail('data_error', 'named_put_execute');
    }
    mysqli_stmt_close($st);
    die($aes::encrypt('ok;' . time(), $cipher, $iv));
}

if ($cloudcfg_op === 'cloudcfg_delete')
{
    $name = cloudcfg_decode_name(cloudcfg_param('c'));
    if ($name === false)
        cloudcfg_fail('bad_request');

    $st = mysqli_prepare($cfg,
        "DELETE FROM loader_user_configs WHERE game = ? AND uid = ? AND name = ?");
    if (!$st)
        cloudcfg_fail('data_error', 'named_delete_prepare');
    mysqli_stmt_bind_param($st, 'sis', $game, $user_id, $name);
    if (!@mysqli_stmt_execute($st))
    {
        mysqli_stmt_close($st);
        cloudcfg_fail('data_error', 'named_delete_execute');
    }
    $deleted = mysqli_stmt_affected_rows($st);
    mysqli_stmt_close($st);
    if ($deleted < 1)
        cloudcfg_fail('not_found');
    die($aes::encrypt('ok', $cipher, $iv));
}

if ($cloudcfg_op === 'cloudcfg_rename')
{
    $blob = cloudcfg_param('c');
    $dot = is_string($blob) ? strpos($blob, '.') : false;
    if ($dot === false)
        cloudcfg_fail('bad_request');
    $old_name = cloudcfg_decode_name(substr($blob, 0, $dot));
    $new_name = cloudcfg_decode_name(substr($blob, $dot + 1));
    if ($old_name === false || $new_name === false)
        cloudcfg_fail('bad_request');

    $st = mysqli_prepare($cfg,
        "UPDATE loader_user_configs SET name = ?, updated_at = NOW()
         WHERE game = ? AND uid = ? AND name = ?");
    if (!$st)
        cloudcfg_fail('data_error', 'named_rename_prepare');
    mysqli_stmt_bind_param($st, 'ssis', $new_name, $game, $user_id, $old_name);
    if (!@mysqli_stmt_execute($st))
    {
        $duplicate = mysqli_errno($cfg) === 1062;
        mysqli_stmt_close($st);
        cloudcfg_fail($duplicate ? 'already_exists' : 'data_error',
            $duplicate ? '' : 'named_rename_execute');
    }
    $renamed = mysqli_stmt_affected_rows($st);
    mysqli_stmt_close($st);
    if ($renamed < 1)
        cloudcfg_fail('not_found');
    die($aes::encrypt('ok;' . time(), $cipher, $iv));
}

// -----------------------------------------------------------------------
// cloudcfg_load — code in, config out.
// -----------------------------------------------------------------------
if ($cloudcfg_op === 'cloudcfg_load')
{
    $code = cloudcfg_param('c');
    $code = $code === null ? '' : strtolower(trim($code));
    if (!preg_match(CLOUDCFG_RE_CODE, $code))
        cloudcfg_fail('bad_request');

    $st = mysqli_prepare($cfg,
        "SELECT base36, uid, username, name, payload
         FROM `" . CFG_DB_TABLE . "` WHERE game = ? AND base36 = ? LIMIT 1");
    if (!$st)
        cloudcfg_fail('data_error', 'load_prepare');

    mysqli_stmt_bind_param($st, 'ss', $game, $code);
    if (!@mysqli_stmt_execute($st))
    {
        mysqli_stmt_close($st);
        cloudcfg_fail('data_error', 'load_execute');
    }

    mysqli_stmt_bind_result($st, $row_code, $row_uid, $row_username, $row_name, $row_payload);
    $found = mysqli_stmt_fetch($st);
    mysqli_stmt_close($st);

    if (!$found)
        cloudcfg_fail('not_found');

    // name and username travel base64 so the fixed `;` arity of six survives
    // any characters XenForo allows in a display name.
    die($aes::encrypt(
        'ok;' . $row_code
        . ';' . (int)$row_uid
        . ';' . base64_encode($row_name)
        . ';' . base64_encode($row_username)
        . ';' . $row_payload,
        $cipher, $iv
    ));
}

// -----------------------------------------------------------------------
// cloudcfg_save — config in, fresh code out.
// -----------------------------------------------------------------------
$blob = cloudcfg_param('c');
if ($blob === null)
    cloudcfg_fail('bad_request');

// Both halves are base64, so '.' can never occur inside either of them.
$dot = strpos($blob, '.');
if ($dot === false)
    cloudcfg_fail('bad_request');

$name_b64    = substr($blob, 0, $dot);
$payload_b64 = substr($blob, $dot + 1);

// Reject the encoding before decoding it: base64_decode() in strict mode
// already refuses stray bytes, but matching the alphabet first means a payload
// full of quotes never becomes a string this script holds at all.
if (!preg_match(CLOUDCFG_RE_B64, $name_b64) || !preg_match(CLOUDCFG_RE_B64, $payload_b64))
    cloudcfg_fail('bad_request');

$name = base64_decode($name_b64, true);
if ($name === false)
    cloudcfg_fail('bad_request');
$name = trim($name);
if (!preg_match(CLOUDCFG_RE_NAME, $name))
    cloudcfg_fail('bad_request');

// The cap is on the encoded form so the check happens before the decode.
if (strlen($payload_b64) > (int)(CLOUDCFG_MAX_PAYLOAD * 4 / 3) + 4)
    cloudcfg_fail('too_large');

$payload = base64_decode($payload_b64, true);
if ($payload === false || $payload === '')
    cloudcfg_fail('bad_request');

// The payload is config_store's serialize() output: `key=value` lines of
// printable ASCII. It is stored opaquely and never interpreted here, but a body
// that cannot be a config has no business in the table either.
if (strlen($payload) > CLOUDCFG_MAX_PAYLOAD)
    cloudcfg_fail('too_large');
if (!preg_match('/^[\x20-\x7E\r\n]+$/', $payload))
    cloudcfg_fail('bad_request');

// Hard cap per (game, user). Old rows are NOT silently evicted: a code that
// has been passed around must keep working, so the user hits an explicit
// limit instead of quietly breaking a friend's import.
$st = mysqli_prepare($cfg,
    "SELECT COUNT(*) FROM `" . CFG_DB_TABLE . "` WHERE game = ? AND uid = ?");
if (!$st)
    cloudcfg_fail('data_error', 'count_prepare');
mysqli_stmt_bind_param($st, 'si', $game, $user_id);
if (!@mysqli_stmt_execute($st))
{
    mysqli_stmt_close($st);
    cloudcfg_fail('data_error', 'count_execute');
}
mysqli_stmt_bind_result($st, $owned);
mysqli_stmt_fetch($st);
mysqli_stmt_close($st);

if ((int)$owned >= CLOUDCFG_MAX_PER_USER)
    cloudcfg_fail('limit_reached');

// 36^20 makes a collision an RNG failure, not a probability — but the UNIQUE
// key is authoritative, so retry a couple of times before giving up.
for ($try = 0; $try < 3; $try++)
{
    $code = cloudcfg_generate_code();

    $st = mysqli_prepare($cfg,
        "INSERT INTO `" . CFG_DB_TABLE . "` (game, base36, username, uid, name, payload)
         VALUES (?, ?, ?, ?, ?, ?)");
    if (!$st)
        cloudcfg_fail('data_error', 'insert_prepare');

    mysqli_stmt_bind_param($st, 'sssiss',
        $game, $code, $username, $user_id, $name, $payload_b64);

    if (@mysqli_stmt_execute($st))
    {
        mysqli_stmt_close($st);
        die($aes::encrypt('ok;' . $code, $cipher, $iv));
    }

    $dup = mysqli_errno($cfg) === 1062;
    mysqli_stmt_close($st);

    if (!$dup)
    {
        error_log('cloudcfg: insert failed: ' . mysqli_error($cfg));
        cloudcfg_fail('data_error', 'insert_execute');
    }
}

error_log('cloudcfg: three code collisions in a row — check random_bytes');
cloudcfg_fail('data_error', 'code_collisions');
?>
