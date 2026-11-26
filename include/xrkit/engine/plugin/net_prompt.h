#pragma once
// 插件联网授权钩子（BETA.258）。
//
// 联网开关对插件**始终权威**：`host.http_*` 在开关关闭时一律拒绝，除非外壳装了
// 这个钩子、并且用户当场放行「这一次」。钩子由 Qt 外壳在 UI 线程安装；CLI /
// 无界面环境保持空 ⇒ 一律拒绝（fail closed），不存在"插件自己绕开"的通道。

#include <functional>
#include <string>

namespace xrkit::plugin {

// owner = 插件 id；url = 完整请求地址。返回 true 表示用户放行本次。
using NetworkPrompt = std::function<bool(const std::string& owner, const std::string& url)>;

void set_network_prompt(NetworkPrompt prompt);
NetworkPrompt network_prompt();

}  // namespace xrkit::plugin
