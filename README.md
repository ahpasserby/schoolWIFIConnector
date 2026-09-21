# schoolWIFIConnector

macOS 上的校园网自动认证工具。用 C++ 写的单个二进制，零第三方依赖。

> **解决的问题**：连上校园网后需要在网页上做二次认证，但 macOS 的
> Captive Network Assistant（那个自动弹出的登录小窗）经常弹不出来、
> 弹出来一片空白、或者点了没反应，导致连着 WiFi 却上不了网，只能开流量。

`schoolwifi` 绕过那个小窗：它自己去探测门户、自己找到真正的登录页、
自己填表提交，并且可以常驻后台，掉线了自动重连。

```
$ schoolwifi status
Interface    en0
SSID         CAMPUS-WIFI
IPv4         10.12.33.7
Portal       captive
Intercept    http://10.0.0.1/portal/index.jsp (HTTP 302)

Run `schoolwifi login` to authenticate, or `schoolwifi open` to do it by hand.

$ schoolwifi login
INFO  submitting login to http://10.0.0.1/eportal/login.do as 20210001
INFO  connected: verified online
```

---

## 安装

需要 macOS 和 Xcode Command Line Tools（`xcode-select --install`）。
不需要 CMake、不需要 Homebrew、不需要任何第三方库 —— libcurl 和
CoreWLAN 都是系统自带的。

```bash
git clone https://github.com/ahpasserby/schoolWIFIConnector.git
cd schoolWIFIConnector
make
sudo make install          # 装到 /usr/local/bin/schoolwifi
```

不想装到系统目录就直接用 `./build/schoolwifi`。

## 快速开始

```bash
# 1. 先连上校园网 WiFi（连上但还没认证的状态）
# 2. 交互式配置：填学号和密码，密码会存进 macOS 钥匙串
schoolwifi setup

# 3. 登录
schoolwifi login

# 4. 成功之后，让它开机自启、常驻后台
schoolwifi install-agent
```

装好 LaunchAgent 之后就不用再管了：开机自动运行，检测到被门户拦截就自动认证，
掉线也会自动重连。日志在 `~/Library/Logs/schoolwifi.log`。

## 命令

| 命令 | 说明 |
| --- | --- |
| `status` | 显示网卡、SSID、以及当前是 online / captive / offline |
| `login` | 立即认证一次 |
| `logout` | 发送注销请求（需要配置 `logout_url`） |
| `open` | **用默认浏览器打开真正的登录页** —— 这就是那个弹不出来的窗口 |
| `watch` | 常驻前台，检测到门户就自动登录（LaunchAgent 跑的就是它） |
| `diagnose` | 打印门户探测的全过程，用来给自己学校写配置 |
| `setup` | 交互式生成配置 + 写入钥匙串 |
| `install-agent` / `uninstall-agent` / `agent-status` | 管理开机自启 |

全局选项：`-c/--config PATH`、`-v/--verbose`（打印每个 HTTP 请求和跳转）、`-q/--quiet`。

> 如果自动登录一时搞不定，`schoolwifi open` 是保底方案：
> 它把真正的登录页地址交给 Safari/Chrome 打开，绕过那个坏掉的系统小窗。
> 单这一条命令就能解决「页面弹不出来」的问题。

## 配置

默认读 `~/.config/schoolwifi/config.ini`，完整带注释的示例见
[`config/config.example.ini`](config/config.example.ini)。

大多数学校只需要：

```ini
[network]
ssid = CAMPUS-WIFI

[account]
username = 20210001
```

密码不写在配置文件里 —— `schoolwifi setup` 会把它存进 macOS 登录钥匙串。
也可以临时用环境变量覆盖：`SCHOOLWIFI_PASSWORD=xxx schoolwifi login`。

如果自动识别失败，看 [docs/adapting-to-your-campus.md](docs/adapting-to-your-campus.md)，
里面讲了怎么用 `schoolwifi diagnose` 的输出把 `[portal]` 那段填对。

## 它是怎么工作的

1. **探测**：请求 `http://captive.apple.com/hotspot-detect.html`（**不跟随跳转**）。
   返回预期内容 = 在线；返回 302 = 被门户拦截，`Location` 头就是门户地址；
   返回 200 但内容不对 = 被透明代理换了页面，同样是被拦截。
2. **找登录页**：从拦截地址出发，逐跳跟随 `<meta http-equiv=refresh>`、
   JS 的 `location.href=` / `top.self.location`、以及 `<iframe src>`，
   直到找到一个含 `type=password` 输入框的页面。校园网门户基本都要跳 2～3 次。
3. **解析表单**：提取整个表单，**保留所有 hidden 字段**（CSRF token、IP、
   会话 ID 这些丢了就会认证失败），按优先级识别账号和密码输入框。
4. **提交**：带上 Referer 和 cookie POST 过去。
5. **验证**：不信门户返回的「登录成功」，而是重新跑一遍第 1 步探测 ——
   只有网络真的通了才算成功。

## 开发

```bash
make            # 编译
make test       # 单元测试：HTML 解析、字段识别、URL 拼接、配置读写
make e2e        # 端到端测试：拿真二进制打一个模拟门户
make check      # 两个都跑
```

`tests/fake_portal.py` 是一个行为跟真门户一致的假门户（302 → splash →
meta refresh → 带 CSRF token 的表单），所以**不在校园网环境也能完整测试**
登录链路。

## 兼容性与安全

- macOS 13+（在 macOS 26 / Apple Silicon 上开发测试）。
- macOS 14 起 CoreWLAN 的 `ssid` 需要定位服务授权才返回值，所以 SSID 改从
  `ipconfig getsummary` 读。读不到也不影响登录，只是 `ssid` 白名单会失效。
- 密码存 macOS 钥匙串，不落盘明文。`diagnose` 的输出里密码会被打码。
- 校园网关普遍用自签名证书，所以 HTTPS 证书校验默认关闭。发出去的只有
  「浏览器本来也要发给同一台网关」的那份凭据。
- 探测和登录强制绕过系统代理（`http_proxy` / VPN）—— 不绕过的话，
  被拦截的网络会被误判成在线。

## License

MIT
