# Offline Message Pull Pagination Design

## Overview

Add a focused TCP integration regression test for offline message pagination. Alice sends five messages while Bob is offline. Bob logs in later and pulls with `limit=2`, ACKing each page before continuing, until all stored messages have moved out of the offline pull result set.

This is a test-only change. The production protocol, repository behavior, service behavior, and database schema remain unchanged.

## Goals

- Cover the complete offline补拉 pagination path over the existing TCP integration harness.
- Verify five stored messages are returned in send order as `2 + 2 + 1`.
- Verify `has_more` is `true` for the first two pages and `false` for the final non-empty page.
- Verify each subsequent page uses the previous page's last `message_id` as `since_message_id`.
- Verify ACK affects exactly the messages returned on the current page.
- Verify the final pull after all ACKs returns an empty `messages` array with `has_more=false`.

## Non Goals

- Do not change `pull_offline_messages` protocol fields.
- Do not add support for a literal numeric `since_message_id=0` sentinel.
- Do not change ACK, delivered, or read status semantics.
- Do not add Redis, cross-server, or stress-test coverage in this spec.
- Do not refactor the integration test harness beyond small local helpers needed for readability.

## Selected Approach

Add one test case to `tests/auth_integration_test.cc`, for example `TestOfflineMessagePullPaginationOverTcp`.

The first pull will represent `since_message_id=0` from the requested scenario by omitting `since_message_id` in the JSON request. This matches the current protocol: `since_message_id` is a string cursor, an empty cursor means "from the beginning", and a literal `"0"` would be interpreted as a real message id.

The test will reuse the existing `auth_test_server` process, users, socket helpers, packet framing, and JSON envelope assertions. It will not introduce a new binary or new CMake target.

## Test Flow

```text
Alice connects and logs in.
Alice sends five send_message requests to offline Bob.
The test records each send response message_id and expected content.
Alice disconnects.

Bob connects and logs in.

Page 1:
  Bob sends pull_offline_messages with limit=2 and no since_message_id.
  Response contains message 1 and message 2 in order.
  has_more is true.
  Bob ACKs both message ids.
  affected_rows is 2.

Page 2:
  Bob sends pull_offline_messages with limit=2 and since_message_id=<page 1 last message_id>.
  Response contains message 3 and message 4 in order.
  has_more is true.
  Bob ACKs both message ids.
  affected_rows is 2.

Page 3:
  Bob sends pull_offline_messages with limit=2 and since_message_id=<page 2 last message_id>.
  Response contains message 5.
  has_more is false.
  Bob ACKs the message id.
  affected_rows is 1.

Final pull:
  Bob sends pull_offline_messages with limit=2 and since_message_id=<page 3 last message_id>.
  Response contains an empty messages array.
  has_more is false.
```

## Assertions

For every send response:

- The response envelope is `send_message_resp` with `ErrorCode::OK`.
- `message_id` is non-empty.
- The returned status is stored.

For every pull response:

- The response envelope is `pull_offline_messages_resp` with `ErrorCode::OK`.
- `messages` is an array of the expected size.
- `has_more` matches the page expectation.
- Each item has the expected `message_id`, `from_user_id`, `to_user_id`, and `content`.
- Message order matches Alice's send order.

For every ACK response:

- The response envelope is `message_ack_resp` with `ErrorCode::OK`.
- `affected_rows` equals the page size.
- `message_ids` echoes the ACKed ids.

## Error Handling

The test should fail fast with existing `assert` style if any socket send, receive, JSON parse, envelope check, pagination result, or ACK result is unexpected. No new retry behavior is needed beyond the existing server readiness wait.

## Test Isolation

Use unique `client_msg_id` values and message contents for this scenario, such as a pagination-specific prefix plus the message index. This avoids colliding with existing auth integration cases that use the same fixed Alice and Bob test users.

The auth integration server uses its in-memory repositories for one test-binary run. Existing scenarios ACK the Bob messages they create, and this scenario ACKs each returned page before moving forward, so the final pull should observe an empty offline queue.

## Implementation Notes

Small local helpers in `tests/auth_integration_test.cc` are acceptable if they keep the test readable, such as a helper to send one message, collect pull results, or ACK a vector of message ids. Helpers should remain scoped to the test file and follow the current assert-based style.

The new test should be called from the existing `main()` in `tests/auth_integration_test.cc`, and the existing `auth_integration_test` CTest target should remain the only target involved.
