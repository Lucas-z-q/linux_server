#ifndef LINUX_SERVER_INCLUDE_DB_GROUP_REPOSITORY_H_
#define LINUX_SERVER_INCLUDE_DB_GROUP_REPOSITORY_H_

#include <optional>
#include <vector>

#include "db/user_repository.h"
#include "model/group_record.h"

namespace chat {

struct GroupRepositoryCreateResult {
    RepositoryStatus status = RepositoryStatus::kInsertFailed;
    std::optional<GroupRecord> group;
    bool created = false;
};

struct GroupRepositoryFindResult {
    RepositoryStatus status = RepositoryStatus::kNotFound;
    std::optional<GroupRecord> group;
};

struct GroupRepositoryAddMemberResult {
    RepositoryStatus status = RepositoryStatus::kInsertFailed;
    std::optional<GroupMemberRecord> member;
    bool created = false;
};

struct GroupRepositoryListMembersResult {
    RepositoryStatus status = RepositoryStatus::kQueryFailed;
    std::vector<GroupMemberRecord> members;
};

class IGroupRepository {
   public:
    virtual ~IGroupRepository() = default;

    virtual GroupRepositoryCreateResult createGroup(const GroupRecord& group,
                                                    const std::vector<GroupMemberRecord>& initial_members) = 0;
    virtual GroupRepositoryFindResult findGroup(const std::string& group_id) = 0;
    virtual GroupRepositoryAddMemberResult addMember(const GroupMemberRecord& member) = 0;
    virtual GroupRepositoryListMembersResult listMembers(const std::string& group_id) = 0;
};

class DbPool;

class GroupRepository : public IGroupRepository {
   public:
    explicit GroupRepository(DbPool* pool);

    GroupRepositoryCreateResult createGroup(const GroupRecord& group,
                                            const std::vector<GroupMemberRecord>& initial_members) override;
    GroupRepositoryFindResult findGroup(const std::string& group_id) override;
    GroupRepositoryAddMemberResult addMember(const GroupMemberRecord& member) override;
    GroupRepositoryListMembersResult listMembers(const std::string& group_id) override;

   private:
    DbPool* pool_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_DB_GROUP_REPOSITORY_H_
