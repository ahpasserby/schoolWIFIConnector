# schoolWIFIConnector

macOS 上的校园网自动认证工具。用 C++ 写的单个二进制，零第三方依赖。

> **解决的问题**：连上校园网后需要在网页上做二次认证，但 macOS 的
> Captive Network Assistant（那个自动弹出的登录小窗）经常弹不出来、
> 弹出来一片空白、或者点了没反应，导致连着 WiFi 却上不了网，只能开流量。

`schoolwifi` 绕过那个小窗：自己探测门户、自己找到真正的登录页、自己完成认证
（普通表单门户和[深澜 Srun](#支持的门户类型) 都支持），并且可以常驻后台，掉线自动重连。

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

## 快速开始

三步，从零到能用。

**1. 编译安装**

```bash
git clone https://github.com/ahpasserby/schoolWIFIConnector.git
cd schoolWIFIConnector
make && sudo make install
```

只需要 Xcode Command Line Tools（`xcode-select --install`）。
不用装 CMake、Homebrew 或任何第三方库。不想装到系统目录就直接用 `./build/schoolwifi`。

**2. 连上校园网 WiFi，然后配置**

```bash
schoolwifi setup
```

会问四个问题：**WiFi 名称、网卡名、学号、密码**。
网卡名直接回车用 `en0` 就行（这一项要的是网卡名，不是 WiFi 名称）。
密码存进 macOS 钥匙串，不会写进配置文件。

**3. 登录**

```bash
schoolwifi login
```

看到 `connected: verified online` 就成了。

到这里就能用了。想让它**开机自启、掉线自动重连**，再加一条：

```bash
schoolwifi install-agent
```

装完就不用管了：开机自动运行，检测到被门户拦截就自动认证。
日志在 `~/Library/Logs/schoolwifi.log`。

### 没连上怎么办

按顺序试这三步，基本能定位：

```bash
schoolwifi diagnose    # 打印探测全过程，多数问题看一眼就知道
schoolwifi -v login    # 打印每个 HTTP 请求和跳转
schoolwifi open        # 保底方案：直接用浏览器打开真正的登录页
```

`schoolwifi open` 单独就能解决「页面弹不出来」——它绕过那个坏掉的系统小窗，
把真正的登录页交给 Safari/Chrome。

常见原因见 [常见问题](#常见问题)；要给自己学校调配置见
[适配你自己学校的校园网](docs/adapting-to-your-campus.md)。

---

下面都是细节，按需查阅：

| 章节 | 内容 |
| --- | --- |
| [命令](#命令) | 每个子命令分别干什么 |
| [配置项说明](#配置项说明) | 配置文件每一项的含义、默认值、什么时候需要改 |
| [支持的门户类型](#支持的门户类型) | 普通表单 / 深澜 Srun / API 式门户 |
| [常见问题](#常见问题) | DNS 解析失败、代理干扰、登录没反应 |
| [它是怎么工作的](#它是怎么工作的) | 探测、找登录页、提交、验证的原理 |
| [开发](#开发) | 编译、单元测试、端到端测试 |
| [兼容性与安全](#兼容性与安全) | 系统要求、密码存储、证书校验 |

---

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

## 配置项说明

### `schoolwifi setup` 会问的四个问题

| 提示 | 对应配置项 | 该填什么 | 例子 |
| --- | --- | --- | --- |
| 校园网 WiFi 名称 | `ssid` | 校园网的 **WiFi 名称**，就是 Mac 右上角 WiFi 菜单里显示的那个名字。直接回车 = 用当前已连接的那个。填 `any` = 不限制网络 | `CAMPUS-WIFI` |
| 无线网卡名 | `interface` | **网卡名，不是 WiFi 名称。** Mac 上几乎永远是 `en0`，直接回车就行 | `en0` |
| 校园网账号 | `username` | 学号 / 校园网账号，就是你在网页认证页面里填的那个 | `20210001` |
| 校园网密码 | （不写入配置） | 网页认证时填的密码。输入时不回显，存进 macOS 登录钥匙串 | |

> **最容易填错的是第 2 项。** `interface` 问的是网卡的系统名字（`en0`），
> 不是 WiFi 的名字。填错的话所有请求都会绑到不存在的网卡上而失败。
> 不确定就直接回车用默认值 —— 填错了向导现在也会挡住并提示你。

### 配置文件

默认路径 `~/.config/schoolwifi/config.ini`，用 `-c/--config` 可以指定别的。
带完整注释的示例见 [`config/config.example.ini`](config/config.example.ini)。

大多数学校只需要这几行，`[portal]` 整段留空即可：

```ini
[network]
ssid = CAMPUS-WIFI

[account]
username = 20210001
```

#### `[network]` —— 在哪个网络上动作

| 配置项 | 说明 | 默认 |
| --- | --- | --- |
| `ssid` | 只在连接这个 WiFi 时才自动登录。留空 = 任何网络都尝试。**建议填上**，否则在家里的路由器上也会把校园网密码发出去 | 空 |
| `interface` | 无线网卡名，Mac 上基本都是 `en0` | `en0` |
| `dns_server` | 系统 DNS 解析不了门户域名时改用哪台 DNS。留空 = 自动用本网络 DHCP 下发的那台（通常就是校园 DNS） | 空 |

#### `[account]` —— 用哪个账号

| 配置项 | 说明 | 默认 |
| --- | --- | --- |
| `username` | 学号 / 校园网账号 | 空 |
| `keychain_service` | 密码在钥匙串里的服务名，一般不用改 | `schoolwifi` |
| `password` | 明文密码。**不推荐**，只在钥匙串不可用时用；填了会在日志里警告 | 空 |

密码的查找顺序是：环境变量 `SCHOOLWIFI_PASSWORD` → 钥匙串 → 配置里的 `password`。
所以想临时换个密码试，不用改配置：

```bash
SCHOOLWIFI_PASSWORD='xxx' schoolwifi -v login
```

#### `[portal]` —— 怎么跟认证门户打交道

**这一整段大部分学校都可以留空**，程序会自动探测登录页、自动识别表单字段。
只有 `schoolwifi login` 失败时才需要填，填什么由 `schoolwifi diagnose` 的输出决定
（详见 [docs/adapting-to-your-campus.md](docs/adapting-to-your-campus.md)）。

| 配置项 | 说明 | 默认 |
| --- | --- | --- |
| `login_method` | `form` = 自动找登录页解析表单提交（也会自动识别深澜 Srun 门户）；`srun` = 强制走深澜认证；`raw` = 直接把 `post_body` 发给 `login_url` | `form` |
| `login_url` | 登录页地址。留空 = 自动探测。自动探测不到时，填 diagnose 输出里 `login page:` 那一行 | 空 |
| `logout_url` | 注销地址，`schoolwifi logout` 用。支持 `{username}` 占位符 | 空 |
| `username_field` | 账号输入框的 `name`。留空 = 自动识别 | 空 |
| `password_field` | 密码输入框的 `name`。留空 = 自动识别 | 空 |
| `field.<名字>` | 额外要提交的字段，比如运营商、域。会覆盖表单里的同名默认值。可以写多行 | 无 |
| `success_contains` | 响应里出现这个字符串就算登录成功 | 空 |
| `failure_contains` | 响应里出现这个字符串就算登录失败 | 空 |
| `probe_urls` | 连通性探测地址，逗号分隔 | Apple / 华为 / 微软三个检测地址 |
| `probe_timeout` | 每个探测地址等多少秒。网络会丢包（而不是明确拒绝）时，这个值决定了命令要等多久 | `5` |
| `user_agent` | 伪装的 UA。有些门户对未知 UA 会返回坏掉的页面 | 内置 Safari UA |
| `http_method` | 仅 `raw` 模式：`POST` 或 `GET` | `POST` |
| `post_body` | 仅 `raw` 模式：请求体模板。占位符 `{username}` `{password}`，URL 编码版 `{username\|url}` `{password\|url}` | 空 |

`success_contains` / `failure_contains` 都留空时，判定方式是**重新探测一次网络是否真的通了** ——
这比信门户自己返回的「登录成功」可靠得多，一般不用改。

#### `[watch]` —— 后台守护进程的行为

| 配置项 | 说明 | 默认 |
| --- | --- | --- |
| `online_interval` | 已在线时，隔多少秒探测一次 | `30` |
| `captive_interval` | 被门户拦截时，隔多少秒重试一次 | `5` |
| `max_retries` | 连续失败几次后进入退避 | `3` |
| `retry_backoff` | 退避多少秒再继续 | `30` |
| `log_file` | 后台日志路径 | `~/Library/Logs/schoolwifi.log` |

### 改完配置之后

`watch` 只在启动时读一次配置，所以改完配置要让后台进程重新加载：

```bash
schoolwifi install-agent    # 会先卸载再重新加载，改完配置跑这个就行
```

没装后台守护的话，改完直接跑 `schoolwifi login` 即可。

## 支持的门户类型

| 类型 | 说明 | 需要配置吗 |
| --- | --- | --- |
| 普通 HTML 表单 | 自动找到登录页、识别账号/密码输入框、保留 hidden 字段后提交 | 一般不用 |
| **深澜 Srun** | 纯 JS 单页应用，**没有 HTML 表单**。登录参数要用服务端下发的 challenge 现算：XXTEA 加密 + 自定义 base64 + HMAC-MD5 + SHA1 签名。已内置实现，自动识别 | 一般不用 |
| API 式门户 | 没有表单、直接调接口的，用 `login_method = raw` 把请求重放一遍 | 要填 `login_url` + `post_body` |

深澜门户的识别是自动的（看页面里的 `Srunsoft` / `srun_bx1` / `srun_portal` 特征）。
识别到之后日志里会有一行：

```
INFO  detected a Srun portal at https://w.example.edu.cn
INFO  srun: challenge obtained, submitting login for 20210001 (ip 10.x.x.x, acid 1)
INFO  connected: srun: login_ok; verified online
```

自动识别没生效的话，可以强制指定：

```ini
[portal]
login_method = srun
login_url = https://w.example.edu.cn/srun_portal_pc?ac_id=1
```

> 已知限制：如果你们学校的深澜门户开了**图形验证码**，目前不支持，
> 只能用 `schoolwifi open` 手动登录。

## 常见问题

### `Could not resolve host: xxx.edu.cn`

门户的域名往往**只存在于校园内网 DNS**。如果你在「系统设置 → 网络 → DNS」里
手工写死了公共 DNS（比如 `223.5.5.5`、`8.8.8.8`），这个设置会跨所有网络生效，
于是在校园网里：

- `captive.apple.com` 这种公网域名 → 解析得了，所以能探测到被拦截
- `w.xxx.edu.cn` 这种内网域名 → 公共 DNS 不认识 → 解析失败，拿不到登录页

`schoolwifi` 会自动处理这种情况：解析失败时，它会改用**本网络 DHCP 下发的
DNS**（也就是校园 DNS）去解析门户域名，然后把结果直接钉给该次连接，
不需要你改系统设置。

用 `schoolwifi diagnose` 可以看到这个冲突：

```
== dns ==
system    : 223.5.5.5
dhcp      : 10.253.0.1
note      : the system resolver ignores this network's DNS (manually pinned
            in System Settings). ...
```

如果自动回退也失败（比如校园 DNS 不在 DHCP 里下发），手工指定：

```ini
[network]
dns_server = 10.253.0.1
```

实在不行，把 Wi-Fi 的手工 DNS 去掉，恢复成用 DHCP 下发的：

```bash
sudo networksetup -setdnsservers Wi-Fi Empty
```

### 命令看起来卡住了，十几秒没反应

多半不是卡死，是在等超时。如果网络把探测请求**静默丢包**（既不回应也不拒绝），
每个探测地址都要等满 `probe_timeout` 秒。现在每次超时都会打一行：

```
INFO  probe http://captive.apple.com/... failed (Connection timed out after 5005 ms); trying the next one
INFO  no check endpoint answered; trying the gateway at http://10.0.0.1/
```

嫌慢就把超时调小：

```ini
[portal]
probe_timeout = 2
```

三个探测地址全都没响应时，工具会再试一次**默认网关**——宿舍和校园网的网关
本身往往就是门户。只有当网关返回的页面确实像登录页（有密码框或跳转）时才会
认定为门户，避免把普通路由器管理页误判成认证页。

### 需要连续过两道认证（比如校园网 + 运营商宽带）

有些宿舍网是**两级认证**：先过校园网门户，再过一个运营商（联通/电信/移动）的
宽带认证。两道门户的账号密码通常是不一样的。

`schoolwifi` **一次只处理一道门户**。如果第一道过了、第二道接管，你会看到：

```
ERROR login failed: srun accepted the login (srun: login_ok) but logged in to
      w.example.edu.cn, but the network is still intercepted - now by
      10.20.30.40. That is a second authentication stage; ...
```

这条信息说明第一道**成功了**，问题在第二道——不是密码错了。目前的办法是给第二道
单独准备一份配置，手动跑一次：

```bash
schoolwifi login                        # 第一道：校园网
schoolwifi -c ~/.config/schoolwifi/stage2.ini login   # 第二道：运营商
```

`stage2.ini` 用 `schoolwifi -c ~/.config/schoolwifi/stage2.ini diagnose` 的输出来填，
账号密码另存钥匙串（在 `[account]` 里把 `keychain_service` 设成别的名字，
比如 `schoolwifi-unicom`，再跑一次 `setup`）。

> 一次命令自动串完两道认证的支持还没做。如果你有这种网络，欢迎提 issue 带上
> （脱敏后的）两道门户的 `diagnose` 输出。

### 门户页面能打开，但登录没反应

先跑 `schoolwifi diagnose` 看 `== forms ==` 段有没有识别到表单，
再按 [docs/adapting-to-your-campus.md](docs/adapting-to-your-campus.md) 调 `[portal]`。

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
