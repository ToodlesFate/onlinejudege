-- =============================================================================
--  OnlineJudge — 001_init.sql
--  SPEC: §4.2 表结构 / §4.3 幂等 (CREATE TABLE IF NOT EXISTS)
--  用途: 首次启动 MySQL 时由 /docker-entrypoint-initdb.d 自动执行;
--        同时被 backend 的 `db_init` 命令读取以保持一致
--  字符集: utf8mb4 / utf8mb4_unicode_ci (SPEC §7.2 docker-compose)
--  引擎:   InnoDB (事务 + 外键)
-- =============================================================================

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- -----------------------------------------------------------------------------
--  1. users
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `users` (
  `id`            BIGINT       NOT NULL AUTO_INCREMENT,
  `username`      VARCHAR(20)  NOT NULL,
  `email`         VARCHAR(100) NOT NULL,
  `password_hash` VARCHAR(255) NOT NULL,
  `is_admin`      TINYINT(1)   NOT NULL DEFAULT 0,
  `created_at`    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_users_username` (`username`),
  UNIQUE KEY `uk_users_email`    (`email`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='用户表';

-- -----------------------------------------------------------------------------
--  2. tags (预置 8 个, 由 002_seed.sql 灌入)
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `tags` (
  `id`   INT         NOT NULL,
  `name` VARCHAR(20) NOT NULL,
  `slug` VARCHAR(20) NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_tags_name` (`name`),
  UNIQUE KEY `uk_tags_slug` (`slug`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='标签表 (预置 8 个, 不开放后台管理)';

-- -----------------------------------------------------------------------------
--  3. problems
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `problems` (
  `id`              BIGINT        NOT NULL AUTO_INCREMENT,
  `title`           VARCHAR(100)  NOT NULL,
  `content_md`      MEDIUMTEXT    NOT NULL,
  `difficulty`      ENUM('easy','medium','hard') NOT NULL,
  `time_limit_ms`   INT           NOT NULL DEFAULT 2000,
  `memory_limit_mb` INT           NOT NULL DEFAULT 256,
  `output_limit_mb` INT           NOT NULL DEFAULT 64,
  `is_published`    TINYINT(1)    NOT NULL DEFAULT 0,
  `created_by`      BIGINT        NOT NULL,
  `created_at`      DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `idx_problems_difficulty`  (`difficulty`),
  KEY `idx_problems_published`   (`is_published`),
  KEY `idx_problems_created_at`  (`created_at`),
  CONSTRAINT `fk_problems_created_by`
    FOREIGN KEY (`created_by`) REFERENCES `users` (`id`)
    ON DELETE RESTRICT ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='题目表';

-- -----------------------------------------------------------------------------
--  4. problem_tags (problems <-> tags 多对多)
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `problem_tags` (
  `problem_id` BIGINT NOT NULL,
  `tag_id`     INT    NOT NULL,
  PRIMARY KEY (`problem_id`, `tag_id`),
  KEY `idx_problem_tags_tag_id` (`tag_id`),
  CONSTRAINT `fk_problem_tags_problem_id`
    FOREIGN KEY (`problem_id`) REFERENCES `problems` (`id`)
    ON DELETE CASCADE ON UPDATE CASCADE,
  CONSTRAINT `fk_problem_tags_tag_id`
    FOREIGN KEY (`tag_id`) REFERENCES `tags` (`id`)
    ON DELETE RESTRICT ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='题目-标签关联表';

-- -----------------------------------------------------------------------------
--  5. testcases
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `testcases` (
  `id`              BIGINT     NOT NULL AUTO_INCREMENT,
  `problem_id`      BIGINT     NOT NULL,
  `case_index`      INT        NOT NULL,
  `input`           LONGTEXT   NOT NULL,
  `expected_output` LONGTEXT   NOT NULL,
  `is_sample`       TINYINT(1) NOT NULL DEFAULT 0,
  `score`           INT        NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_testcases_problem_index` (`problem_id`, `case_index`),
  KEY `idx_testcases_problem_id` (`problem_id`),
  CONSTRAINT `fk_testcases_problem_id`
    FOREIGN KEY (`problem_id`) REFERENCES `problems` (`id`)
    ON DELETE CASCADE ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='题目测试点';

-- -----------------------------------------------------------------------------
--  6. submissions
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `submissions` (
  `id`              BIGINT       NOT NULL AUTO_INCREMENT,
  `user_id`         BIGINT       NOT NULL,
  `problem_id`      BIGINT       NOT NULL,
  `language`        ENUM('c','cpp','java','python','go') NOT NULL,
  `code`            MEDIUMTEXT   NOT NULL,
  `status`          ENUM('queued','compiling','running','finished') NOT NULL DEFAULT 'queued',
  `result`          ENUM('AC','WA','TLE','MLE','OLE','RE','CE','SE') DEFAULT NULL,
  `total_score`     INT          NOT NULL DEFAULT 0,
  `time_used_ms`    INT          NOT NULL DEFAULT 0,
  `memory_used_kb`  INT          NOT NULL DEFAULT 0,
  `compile_output`  MEDIUMTEXT,
  `judge_message`   VARCHAR(500),
  `created_at`      DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `finished_at`     DATETIME     DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_submissions_status_created` (`status`, `created_at`),
  KEY `idx_submissions_user_created`   (`user_id`, `created_at`),
  KEY `idx_submissions_problem_id`     (`problem_id`),
  KEY `idx_submissions_result`         (`result`),
  CONSTRAINT `fk_submissions_user_id`
    FOREIGN KEY (`user_id`) REFERENCES `users` (`id`)
    ON DELETE RESTRICT ON UPDATE CASCADE,
  CONSTRAINT `fk_submissions_problem_id`
    FOREIGN KEY (`problem_id`) REFERENCES `problems` (`id`)
    ON DELETE RESTRICT ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='提交记录';

-- -----------------------------------------------------------------------------
--  7. submission_cases
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `submission_cases` (
  `id`             BIGINT       NOT NULL AUTO_INCREMENT,
  `submission_id`  BIGINT       NOT NULL,
  `case_index`     INT          NOT NULL,
  `status`         ENUM('AC','WA','TLE','MLE','OLE','RE') NOT NULL,
  `time_used_ms`   INT          NOT NULL DEFAULT 0,
  `memory_used_kb` INT          NOT NULL DEFAULT 0,
  `score`          INT          NOT NULL DEFAULT 0,
  `is_sample`      TINYINT(1)   NOT NULL DEFAULT 0,
  `user_output`    LONGTEXT     DEFAULT NULL COMMENT '仅 is_sample=1 时存储',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_submission_cases_sub_idx` (`submission_id`, `case_index`),
  KEY `idx_submission_cases_submission_id` (`submission_id`),
  CONSTRAINT `fk_submission_cases_submission_id`
    FOREIGN KEY (`submission_id`) REFERENCES `submissions` (`id`)
    ON DELETE CASCADE ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='提交逐点结果';

SET FOREIGN_KEY_CHECKS = 1;
