-- 数据库表结构初始化脚本
-- 用于支持单聊/群聊会话管理、消息持久化和离线消息。

-- 1. 创建用户表（若不存在）
CREATE TABLE IF NOT EXISTS users (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(64) NOT NULL UNIQUE COMMENT '用户名，唯一',
    password_hash VARCHAR(128) NOT NULL COMMENT '密码哈希值',
    nickname VARCHAR(64) NOT NULL COMMENT '用户昵称',
    status TINYINT NOT NULL DEFAULT 1 COMMENT '状态：1=启用，0=禁用',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 2. 创建会话表
-- single_chat_key 字段通过 UNIQUE 约束确保同一对用户在单聊（type='single'）下只有唯一一条会话记录；
-- 对于群聊（type='group'），此字段为 NULL，不触发唯一索引限制。
CREATE TABLE IF NOT EXISTS conversations (
    id VARCHAR(64) PRIMARY KEY COMMENT '会话 ID，单聊格式：conv_{min_id}_{max_id}，群聊格式：uuid',
    type VARCHAR(20) NOT NULL COMMENT '会话类型：single=单聊，group=群聊',
    single_chat_key VARCHAR(64) NULL UNIQUE COMMENT '单聊唯一标识，格式为：{min_id}_{max_id}，用于防止同一对用户重复创建单聊会话',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',

    -- 约束：限制会话类型只能为 single 或 group
    CONSTRAINT chk_conversation_type CHECK (type IN ('single', 'group'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 3. 创建会话成员关联表
CREATE TABLE IF NOT EXISTS conversation_members (
    conversation_id VARCHAR(64) NOT NULL COMMENT '关联会话 ID',
    user_id BIGINT NOT NULL COMMENT '关联用户 ID',
    role VARCHAR(20) NOT NULL DEFAULT 'member' COMMENT '角色：owner=群主，admin=管理员，member=普通成员',
    joined_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP COMMENT '加入时间',
    PRIMARY KEY (conversation_id, user_id),
    CONSTRAINT fk_members_conversation FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE,
    CONSTRAINT fk_members_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,

    -- 索引：优化查询某个用户的会话列表
    INDEX idx_members_user (user_id, joined_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 4. 创建消息表
CREATE TABLE IF NOT EXISTS messages (
    id VARCHAR(64) PRIMARY KEY COMMENT '服务端生成的消息 ID',
    conversation_id VARCHAR(64) NOT NULL COMMENT '关联的会话 ID',
    client_msg_id VARCHAR(64) NOT NULL COMMENT '客户端生成的消息唯一标识，用于去重',
    from_user_id BIGINT NOT NULL COMMENT '发送方用户 ID',
    to_user_id BIGINT NOT NULL COMMENT '接收方用户 ID',
    content VARCHAR(4096) NOT NULL COMMENT '消息文本内容',
    status TINYINT NOT NULL DEFAULT 0 COMMENT '状态：0=stored(已存储/未投递)，1=delivered(已投递)，2=read(已读)',
    created_at BIGINT NOT NULL COMMENT '消息创建时间戳，Unix 秒级',
    delivered_at BIGINT NULL DEFAULT NULL COMMENT '投递时间戳，Unix 秒级',
    read_at BIGINT NULL DEFAULT NULL COMMENT '已读时间戳，Unix 秒级',

    -- 约束外键（防止用户物理删除导致聊天记录被自动级联删除，使用 RESTRICT/NO ACTION 保护聊天记录）
    CONSTRAINT fk_messages_conversation FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE,
    CONSTRAINT fk_messages_from_user FOREIGN KEY (from_user_id) REFERENCES users(id),
    CONSTRAINT fk_messages_to_user FOREIGN KEY (to_user_id) REFERENCES users(id),

    -- 约束：限制消息状态取值范围
    CONSTRAINT chk_message_status CHECK (status IN (0, 1, 2)),

    -- 约束 1：防止同一个用户发送重复的 client_msg_id（幂等性）
    UNIQUE KEY uk_from_client_msg (from_user_id, client_msg_id),

    -- 索引 2：优化拉取离线消息并支持稳定游标分页（快速找出某个用户未送达的离线消息）
    INDEX idx_to_status_created_id (to_user_id, status, created_at, id),

    -- 索引 3：优化拉取会话历史消息（按会话时间排序）
    INDEX idx_conv_created (conversation_id, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
