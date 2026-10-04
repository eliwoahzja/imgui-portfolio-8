<?php
// Creates the cloud config table in an existing database.
//
// By default this is the XenForo schema. The normal application user can
// create tables there, but production deployments correctly deny it the global
// CREATE DATABASE privilege. Set CFG_DB_NAME to a separately provisioned
// schema if one is desired.
//
// CLI only, same reasoning as migrate_security.php.
//
// Usage:
//   docker exec -i xenforo-php php /var/www/html/client/migrate_cloudcfg.php
//   php client/migrate_cloudcfg.php --dry-run

if (PHP_SAPI !== 'cli')
{
    http_response_code(403);
    die("migrate_cloudcfg.php is CLI only\n");
}

require_once(__DIR__ . '/settings.php');

$dry_run = in_array('--dry-run', $argv, true);

// PHP 8.1 throws mysqli_sql_exception by default. This migration reports
// concise errors itself, so retain the return-value behavior used by the app.
mysqli_report(MYSQLI_REPORT_OFF);

if (!preg_match('/^[A-Za-z0-9_]+$/', CFG_DB_NAME))
    die("invalid CFG_DB_NAME\n");

$conn = mysqli_connect(CFG_DB_HOST, CFG_DB_USER, CFG_DB_PASS, CFG_DB_NAME);
if (!$conn)
    die("cannot connect to " . CFG_DB_NAME . " on " . CFG_DB_HOST . ": "
        . mysqli_connect_error() . "\n");

mysqli_set_charset($conn, 'utf8mb4');

function say($msg) { echo $msg . "\n"; }

function run($conn, $label, $sql)
{
    global $dry_run;

    if ($dry_run)
    {
        say("  WOULD RUN  $label");
        say("             " . preg_replace('/\s+/', ' ', $sql));
        return;
    }

    if (!mysqli_query($conn, $sql))
        die("FAILED at '$label': " . mysqli_error($conn) . "\n");

    say("  applied    $label");
}

say(($dry_run ? '[dry run] ' : '') . 'ensuring ' . CFG_DB_NAME . '.' . CFG_DB_TABLE);
say('');

// Codes are immutable snapshots: save always creates a new row, so no UPDATE
// path or updated_at column is required.
run($conn, 'table ' . CFG_DB_TABLE,
    "CREATE TABLE IF NOT EXISTS `" . CFG_DB_TABLE . "` (
        id         INT UNSIGNED NOT NULL AUTO_INCREMENT,
        game       VARCHAR(32)  NOT NULL,
        base36     VARCHAR(24)  NOT NULL,
        username   VARCHAR(50)  NOT NULL,
        uid        INT UNSIGNED NOT NULL,
        name       VARCHAR(64)  NOT NULL,
        payload    MEDIUMTEXT   NOT NULL,
        created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
        PRIMARY KEY (id),
        UNIQUE KEY uk_game_code (game, base36),
        KEY idx_game_uid (game, uid)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

run($conn, 'table loader_user_configs',
    "CREATE TABLE IF NOT EXISTS `loader_user_configs` (
        id         INT UNSIGNED NOT NULL AUTO_INCREMENT,
        game       VARCHAR(32)  NOT NULL,
        uid        INT UNSIGNED NOT NULL,
        name       VARCHAR(48)  NOT NULL,
        payload    MEDIUMTEXT   NOT NULL,
        created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP
                   ON UPDATE CURRENT_TIMESTAMP,
        PRIMARY KEY (id),
        UNIQUE KEY uk_game_uid_name (game, uid, name),
        KEY idx_game_uid_updated (game, uid, updated_at)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

say('');
say($dry_run ? 'dry run - nothing was changed' : 'done');
?>
