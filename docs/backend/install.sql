-- Run once after XenForo is installed.
-- Creates the loader-specific tables inside the XenForo database.

-- Loader sessions, keyed by the opaque `sid` handed out during the handshake.
--   sid        — 32 hex chars, echoed by the client as `s=` on every request.
--                NULL only on rows written by a pre-sid loader.
--   state      — HANDSHAKE until activation.php binds a user, then ACTIVE
--   meta       — JSON blob: dll_path, cheat_name, one-shot dll_authorized, ...
--   expires_at — sliding TTL (LOADER_SESSION_TTL); cron_cleanup.php prunes past it
--
-- There is deliberately no index on `ip`. Sessions used to be looked up by
-- client IP when the PHPSESSID cookie went missing, which behind Cloudflare
-- matched on a shared edge address and handed out other users' keys.
CREATE TABLE IF NOT EXISTS aes_keys (
    id         INT UNSIGNED NOT NULL AUTO_INCREMENT,
    sid        CHAR(32)     NULL DEFAULT NULL,
    cipher     VARCHAR(128) NOT NULL,
    iv         VARCHAR(64)  NOT NULL,
    date       DATETIME     NOT NULL,
    ip         VARCHAR(45)  NOT NULL,
    user_id    INT UNSIGNED NOT NULL DEFAULT 0,
    username   VARCHAR(50)  NULL DEFAULT NULL,
    state      VARCHAR(16)  NOT NULL DEFAULT 'HANDSHAKE',
    meta       TEXT         NULL DEFAULT NULL,
    last_seen  DATETIME     NULL DEFAULT NULL,
    expires_at DATETIME     NULL DEFAULT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uk_sid (sid),
    KEY idx_expires (expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- HWID → username binding
CREATE TABLE IF NOT EXISTS xf_hwid (
    id            INT UNSIGNED NOT NULL AUTO_INCREMENT,
    username      VARCHAR(50)  NOT NULL,
    hwid          VARCHAR(255) NOT NULL,
    registered_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Loader version / status (filled via admin panel or manually)
CREATE TABLE IF NOT EXISTS loader_settings (
    id      INT UNSIGNED NOT NULL AUTO_INCREMENT,
    version VARCHAR(20)  NOT NULL,
    status  VARCHAR(50)  NOT NULL DEFAULT 'Enabled',
    PRIMARY KEY (id),
    UNIQUE KEY uk_version (version)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Insert the current loader version matching globals.hpp LOADER_VERSION
INSERT IGNORE INTO loader_settings (version, status) VALUES ('0.0.1', 'Enabled');

-- Cheats table — one row per project (Deadlock, CS2, ...).
--   name        — display name in the loader UI
--   secure      — detection status: "Undetected" / "Detected" / "On update"
--   group_name  — must match xf_user_group.title exactly (e.g. "Premium - Deadlock")
--   dll_path    — absolute path to the DLL inside the php container (/var/secure/*.dll)
--   process     — target game process name for injection
CREATE TABLE IF NOT EXISTS cheats (
    id         INT UNSIGNED NOT NULL AUTO_INCREMENT,
    name       VARCHAR(64)  NOT NULL,
    secure     VARCHAR(32)  NOT NULL DEFAULT 'Undetected',
    group_name VARCHAR(64)  NOT NULL DEFAULT '',
    dll_path   VARCHAR(255) NOT NULL DEFAULT '/var/secure/Dll.dll',
    process    VARCHAR(64)  NOT NULL DEFAULT 'deadlock.exe',
    chunk_size INT UNSIGNED NOT NULL DEFAULT 4096,
    PRIMARY KEY (id),
    UNIQUE KEY uk_group (group_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Sample rows: one cheat per XenForo subscriber group.
-- group_name must match xf_user_group.title exactly.
-- Placeholder rows for CS2/Rust/ABI are pre-created but their DLLs are missing —
-- upload them via the admin panel when ready.
INSERT IGNORE INTO cheats (name, secure, group_name, dll_path, process)
VALUES
    ('Deadlock', 'Undetected', 'Premium - Deadlock', '/var/secure/deadlock.dll', 'deadlock.exe'),
    ('CS2',      'On update',  'Premium - CS2',      '/var/secure/cs2.dll',      'cs2.exe'),
    ('Rust',     'On update',  'Premium - Rust',     '/var/secure/rust.dll',     'RustClient.exe'),
    ('ABI',      'On update',  'Premium - ABI',      '/var/secure/abi.dll',      'Client-Win64-Shipping.exe');

-- Launch log. idx_launched exists so cron_cleanup.php can prune by age
-- without full-scanning a table that only ever grows.
CREATE TABLE IF NOT EXISTS launch_log (
    id          INT UNSIGNED NOT NULL AUTO_INCREMENT,
    license_key VARCHAR(128) NOT NULL,
    ip          VARCHAR(45)  NOT NULL,
    launched_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_launched (launched_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- HWID reset audit log (populated by client/admin/index.php)
CREATE TABLE IF NOT EXISTS hwid_reset_log (
    id              INT UNSIGNED NOT NULL AUTO_INCREMENT,
    admin_username  VARCHAR(50)  NOT NULL,
    target_username VARCHAR(50)  NOT NULL,
    reset_at        DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_target (target_username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Manual subscription grants (issued by admins from the panel)
-- Overrides / supplements xf_user_upgrade_active for users without a paid upgrade.
--   cheat_id NULL → MasterPack (grants access to every cheat in `cheats`)
--   cheat_id set  → grant applies only to that specific cheat
CREATE TABLE IF NOT EXISTS manual_upgrades (
    id              INT UNSIGNED NOT NULL AUTO_INCREMENT,
    user_id         INT UNSIGNED NOT NULL,
    cheat_id        INT UNSIGNED NULL DEFAULT NULL,
    granted_by      VARCHAR(50)  NOT NULL,
    days            INT          NOT NULL,
    end_date        INT UNSIGNED NOT NULL, -- unix timestamp
    granted_at      DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_user (user_id, end_date),
    KEY idx_cheat (cheat_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Personal loader tokens.
-- Each XenForo user can have one active token; token is embedded in the
-- personalized loader binary they download from /client/download.php. The
-- token replaces manual username/password entry — the loader reads its own
-- overlay, decrypts the token, and passes it to activation.php as `t=`.
CREATE TABLE IF NOT EXISTS loader_tokens (
    id            INT UNSIGNED NOT NULL AUTO_INCREMENT,
    user_id       INT UNSIGNED NOT NULL,
    token         VARCHAR(64)  NOT NULL,
    revoked       TINYINT(1)   NOT NULL DEFAULT 0,
    issued_at     DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_used_at  DATETIME     NULL,
    last_ip       VARCHAR(45)  NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uk_token (token),
    KEY idx_user (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Immutable cloud config snapshots. The game key allows the same loader
-- backend to serve multiple products without sharing their code namespace.
CREATE TABLE IF NOT EXISTS loader_cloud_configs (
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
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Mutable, private named configs. These power the config library and autosave;
-- share codes remain immutable snapshots in loader_cloud_configs.
CREATE TABLE IF NOT EXISTS loader_user_configs (
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
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Rate limiting for login attempts
CREATE TABLE IF NOT EXISTS login_attempts (
    id           INT UNSIGNED NOT NULL AUTO_INCREMENT,
    ip           VARCHAR(45)  NOT NULL,
    attempted_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_ip_time (ip, attempted_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Mapper sessions — chunked DLL delivery with per-session encryption
CREATE TABLE IF NOT EXISTS mapper_sessions (
    id             INT UNSIGNED NOT NULL AUTO_INCREMENT,
    user_id        INT UNSIGNED NOT NULL,
    session_id     VARCHAR(64)  NOT NULL,
    xor_key        BLOB         NOT NULL,
    chunk_size     INT UNSIGNED NOT NULL DEFAULT 4096,
    chunk_count    INT UNSIGNED NOT NULL,
    image_size     INT UNSIGNED NOT NULL,
    entry_rva      INT UNSIGNED NOT NULL DEFAULT 0,
    sections_json  TEXT         NOT NULL,
    branch_data    BLOB         NOT NULL,
    chunks_data    LONGBLOB     NOT NULL,
    delivered_mask TEXT         NOT NULL DEFAULT '',
    created_at     DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at     DATETIME     NOT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uk_session (session_id),
    KEY idx_user (user_id),
    KEY idx_expires (expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
