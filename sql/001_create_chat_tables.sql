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


-- 2. 创建好友关系表
-- user_low_id/user_high_id 通过 UNIQUE 约束确保任意两名用户之间最多存在一条好友关系记录。
CREATE TABLE IF NOT EXISTS friendships (
    id BIGINT AUTO_INCREMENT PRIMARY KEY COMMENT '好友关系 ID',
    requester_id BIGINT NOT NULL COMMENT '发起好友申请的用户 ID',
    addressee_id BIGINT NOT NULL COMMENT '接收好友申请的用户 ID',
    user_low_id BIGINT NOT NULL COMMENT '两端用户 ID 较小值，用于无向唯一索引',
    user_high_id BIGINT NOT NULL COMMENT '两端用户 ID 较大值，用于无向唯一索引',
    status VARCHAR(20) NOT NULL DEFAULT 'pending' COMMENT '状态：pending=待同意，accepted=已同意，blocked=已屏蔽',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',

    CONSTRAINT fk_friendships_requester FOREIGN KEY (requester_id) REFERENCES users(id) ON DELETE CASCADE,
    CONSTRAINT fk_friendships_addressee FOREIGN KEY (addressee_id) REFERENCES users(id) ON DELETE CASCADE,
    CONSTRAINT chk_friendships_not_self CHECK (requester_id <> addressee_id),
    CONSTRAINT chk_friendships_pair_order CHECK (user_low_id < user_high_id),
    CONSTRAINT chk_friendships_status CHECK (status IN ('pending', 'accepted', 'blocked')),

    UNIQUE KEY uk_friendships_pair (user_low_id, user_high_id),
    INDEX idx_friendships_requester_status (requester_id, status, created_at),
    INDEX idx_friendships_addressee_status (addressee_id, status, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 3. 创建会话表
-- single_chat_key 字段通过 UNIQUE 约束确保同一对用户在单聊（type='single'）下只有唯一一条会话记录；
-- 对于群聊（type='group'），此字段为 NULL，不触发唯一索引限制。
CREATE TABLE IF NOT EXISTS conversations (
    id VARCHAR(64) PRIMARY KEY COMMENT '会话 ID，单聊格式：conv_{min_id}_{max_id}，群聊格式：uuid',
    type VARCHAR(20) NOT NULL COMMENT '会话类型：single=单聊，group=群聊',
    single_chat_key VARCHAR(64) NULL UNIQUE COMMENT '单聊唯一标识，格式为：{min_id}_{max_id}，用于防止同一对用户重复创建单聊会话',
    last_seq BIGINT NOT NULL DEFAULT 0 COMMENT '会话内最后分配的消息序号',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',

    -- 约束：限制会话类型只能为 single 或 group
    CONSTRAINT chk_conversation_type CHECK (type IN ('single', 'group'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 4. 创建群组表
CREATE TABLE IF NOT EXISTS `groups` (
    id VARCHAR(64) PRIMARY KEY COMMENT '群组 ID',
    name VARCHAR(128) NOT NULL COMMENT '群名称',
    owner_id BIGINT NOT NULL COMMENT '群主用户 ID',
    conversation_id VARCHAR(64) NOT NULL UNIQUE COMMENT '群对应的会话 ID',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',
    CONSTRAINT fk_groups_owner FOREIGN KEY (owner_id) REFERENCES users(id) ON DELETE RESTRICT,
    CONSTRAINT fk_groups_conversation FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE,
    INDEX idx_groups_owner (owner_id, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 5. 创建群成员表
CREATE TABLE IF NOT EXISTS group_members (
    group_id VARCHAR(64) NOT NULL COMMENT '群组 ID',
    user_id BIGINT NOT NULL COMMENT '成员用户 ID',
    role VARCHAR(20) NOT NULL DEFAULT 'member' COMMENT '角色：owner=群主，admin=管理员，member=普通成员',
    joined_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP COMMENT '入群时间',
    PRIMARY KEY (group_id, user_id),
    CONSTRAINT fk_group_members_group FOREIGN KEY (group_id) REFERENCES `groups`(id) ON DELETE CASCADE,
    CONSTRAINT fk_group_members_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
    CONSTRAINT chk_group_member_role CHECK (role IN ('owner', 'admin', 'member')),
    INDEX idx_group_members_user (user_id, joined_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 6. 创建会话成员关联表
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

-- 7. 创建消息表
CREATE TABLE IF NOT EXISTS messages (
    id VARCHAR(64) PRIMARY KEY COMMENT '服务端生成的消息 ID',
    conversation_id VARCHAR(64) NOT NULL COMMENT '关联的会话 ID',
    sequence BIGINT NOT NULL DEFAULT 0 COMMENT '会话内单调递增消息序号',
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

    -- 约束 2：确保同一会话内每个消息序号唯一
    UNIQUE KEY uk_messages_conversation_sequence (conversation_id, sequence),

    -- 索引 3：优化拉取离线消息并支持稳定游标分页（快速找出某个用户未送达的离线消息）
    INDEX idx_to_status_created_id (to_user_id, status, created_at, id),

    -- 索引 4：优化拉取会话历史消息（按会话序号排序）
    INDEX idx_conv_sequence (conversation_id, sequence)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
