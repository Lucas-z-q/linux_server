# Master Bugfix Report

## Scope

本次修复基于 `master` 上一次扫描确认的 3 个问题。
改动保持最小范围，不做重构和无关清理。

## Fixes

### PacketCodec

问题：多个小包在同一 TCP chunk 中总长度超过 `kMaxPacketSize`
时，被误判为超大半包。

证据：`src/codec/packet_codec.cc` 原先在切包前检查
`buffer_.size()`。`include/codec/packet_codec.h` 注释说明限制对象
是半包大小。

修复：`PacketCodec::feed()` 改为先按换行切完整包，并分别校验
单个完整包和残留半包大小。

### JsonCodec

问题：`token` 字段类型错误时被静默当作空 token 接受。

证据：`README.md` 协议字段表定义 `token` 类型为 string。
`src/codec/json_codec.cc` 原逻辑把非 string token 置空后返回成功。

修复：`JsonCodec::decodeMessage()` 在 `token` 存在但不是 string
时返回协议错误。

### ChatService

问题：`ChatService::sendMessage()` 直接调用路径缺少
`to_user_id <= 0` 兜底校验。

证据：`src/codec/json_codec.cc` 协议层拒绝非正数 `to_user_id`，
但 service 层原先会继续查库。

修复：`ChatService::sendMessage()` 增加 `to_user_id` 正数校验，
非法值返回 `INVALID_PARAM`。

## Regression Tests

- `tests/packet_codec_test.cc`
  新增多小包超总长和完整超大包两个回归用例。
- `tests/json_codec_test.cc`
  新增 `TestRejectInvalidTokenType`。
- `tests/chat_service_test.cc`
  新增 `TestSendMessageRejectsInvalidTargetUserId`。

## Verification

- `cmake --build build`
- Focused CTest for packet, JSON, and chat service tests
- `ctest --test-dir build --output-on-failure`
- `git diff --check`
- `npx markdownlint-cli2@0.17.2 docs/master_bugfix_report.md`

以上命令已通过。

Markdown 工具链按项目 skill 优先尝试 `mise run fix-md`。
当前环境未安装 `mise`。
因此改用可用的 Markdown lint 命令验证。
