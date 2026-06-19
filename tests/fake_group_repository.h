#ifndef LINUX_SERVER_TESTS_FAKE_GROUP_REPOSITORY_H_
#define LINUX_SERVER_TESTS_FAKE_GROUP_REPOSITORY_H_

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "db/group_repository.h"

class FakeGroupRepository : public chat::IGroupRepository {
   public:
    chat::RepositoryStatus create_status = chat::RepositoryStatus::kOk;
    chat::RepositoryStatus find_status = chat::RepositoryStatus::kOk;
    chat::RepositoryStatus add_member_status = chat::RepositoryStatus::kOk;
    chat::RepositoryStatus list_members_status = chat::RepositoryStatus::kOk;
    std::unordered_map<std::string, chat::GroupRecord> groups;
    std::unordered_map<std::string, std::vector<chat::GroupMemberRecord>> members;
    int create_calls = 0;
    int add_member_calls = 0;
    int list_members_calls = 0;

    chat::GroupRepositoryCreateResult createGroup(
        const chat::GroupRecord& group, const std::vector<chat::GroupMemberRecord>& initial_members) override {
        ++create_calls;
        if (create_status != chat::RepositoryStatus::kOk) {
            return {.status = create_status};
        }
        if (groups.find(group.id) != groups.end()) {
            return {.status = chat::RepositoryStatus::kDuplicate};
        }
        groups[group.id] = group;
        members[group.id] = initial_members;
        return {.status = chat::RepositoryStatus::kOk, .group = group, .created = true};
    }

    chat::GroupRepositoryFindResult findGroup(const std::string& group_id) override {
        if (find_status != chat::RepositoryStatus::kOk) {
            return {.status = find_status};
        }
        const auto it = groups.find(group_id);
        if (it == groups.end()) {
            return {.status = chat::RepositoryStatus::kNotFound};
        }
        return {.status = chat::RepositoryStatus::kOk, .group = it->second};
    }

    chat::GroupRepositoryAddMemberResult addMember(const chat::GroupMemberRecord& member) override {
        ++add_member_calls;
        if (add_member_status != chat::RepositoryStatus::kOk) {
            return {.status = add_member_status};
        }
        auto& group_members = members[member.group_id];
        const auto it = std::find_if(group_members.begin(), group_members.end(),
                                     [&](const auto& existing) { return existing.user_id == member.user_id; });
        if (it != group_members.end()) {
            return {.status = chat::RepositoryStatus::kDuplicate};
        }
        group_members.push_back(member);
        return {.status = chat::RepositoryStatus::kOk, .member = member, .created = true};
    }

    chat::GroupRepositoryListMembersResult listMembers(const std::string& group_id) override {
        ++list_members_calls;
        if (list_members_status != chat::RepositoryStatus::kOk) {
            return {.status = list_members_status};
        }
        const auto it = members.find(group_id);
        if (it == members.end()) {
            return {.status = chat::RepositoryStatus::kNotFound};
        }
        return {.status = chat::RepositoryStatus::kOk, .members = it->second};
    }
};

#endif  // LINUX_SERVER_TESTS_FAKE_GROUP_REPOSITORY_H_
