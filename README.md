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

**2. 连上要配置的 WiFi，然后跑配置向导**

```bash
schoolwifi setup
```

会问五个问题：**WiFi 名称、网卡名、账号、密码，以及这个网络要不要过两道认证**。
网卡名直接回车用 `en0`。密码存进 macOS 钥匙串，不会写进配置文件。

第五个问题是给**宿舍宽带**准备的——那种「先过校园网、再过运营商」的网络。
选 `y` 会接着问第二道的账号密码，两道一次配完，`login` 时自动串起来：

```
[5/5] 这个网络需要过两道认证吗？
      宿舍宽带常见：先过校园网，再过运营商（联通/电信/移动），两道账号不一样。
      校园网一般只有一道，直接回车即可。
    > 需要第二道吗 [y/N] y
```

**3. 登录**

```bash
schoolwifi login
```

看到 `connected: verified online` 就成了。

### 在教室和宿舍之间切换

**每个网络各跑一次 `setup` 就行**，之后不用再管用哪份配置：

```bash
# 在教学楼，连上校园网
schoolwifi setup          # 第五问回车（只有一道认证）

# 在宿舍，连上宿舍宽带
schoolwifi setup          # 第五问选 y，接着填运营商账号
```

两份配置会按各自的 SSID 存好。之后**在哪都是同一条命令**：

```bash
schoolwifi login
```

它按当前连的 WiFi 自动选对应的配置——在教学楼走校园网那套，在宿舍自动过完两道。
想看配了哪些网络：

```bash
$ schoolwifi profiles
Profiles in /Users/you/.config/schoolwifi

  * dorm.ini                 DORM-WIFI              campus-acct
    dorm-stage2.ini          DORM-WIFI              isp-acct       [later stage]
    config.ini               CAMPUS-WIFI            student-id

Current SSID: DORM-WIFI
`schoolwifi login` here would use the one marked *
```

标着 `[later stage]` 的是第二道认证的配置，由第一道自动调用，不用手动选。

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
| [在教室和宿舍之间切换](#在教室和宿舍之间切换) | 多个网络各配一次，之后一条命令通用 |
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
| `setup` | 交互式生成配置 + 写入钥匙串（可一次配完两道认证） |
| `profiles` | 列出配好的网络，并显示当前这个网络会用哪一份 |
| `install-agent` / `uninstall-agent` / `agent-status` | 管理开机自启 |

全局选项：`-v/--verbose`（打印每个 HTTP 请求和跳转）、`-q/--quiet`。

`-c/--config PATH` 用来指定配置文件。**平时不需要**——不加的话会按当前 WiFi 的
SSID 自动选择对应的配置（见 [`profiles`](#在教室和宿舍之间切换)）。只有在同一个
SSID 配了多份、或者想临时跑某一份时才用得上。

## 配置项说明

### `schoolwifi setup` 会问的五个问题

| 提示 | 对应配置项 | 该填什么 | 例子 |
| --- | --- | --- | --- |
| 校园网 WiFi 名称 | `ssid` | 校园网的 **WiFi 名称**，就是 Mac 右上角 WiFi 菜单里显示的那个名字。直接回车 = 用当前已连接的那个。填 `any` = 不限制网络 | `CAMPUS-WIFI` |
| 无线网卡名 | `interface` | **网卡名，不是 WiFi 名称。** Mac 上几乎永远是 `en0`，直接回车就行 | `en0` |
| 校园网账号 | `username` | 学号 / 校园网账号，就是你在网页认证页面里填的那个 | `20210001` |
| 校园网密码 | （不写入配置） | 网页认证时填的密码。输入时不回显，存进 macOS 登录钥匙串 | |
| 需要第二道吗 | `next_stage` | 宿舍宽带那种「先校园网、再运营商」的网络选 `y`，接着填第二道的账号密码；校园网直接回车 | `N` |

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
| `service_suffix_id` | 仅华为 BYOD：账号属于哪个"服务"（通常是运营商）。留空用门户默认值。报 `E63018` 时多半要改这个 | 空 |
| `next_stage` | 两级认证网络：这一层过了之后接着跑的配置文件路径。见[需要连续过两道认证](#需要连续过两道认证比如校园网--运营商宽带) | 空 |
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
| **华为 BYOD** | 两层都是接口：空壳页 `/byod/index.html` 靠 `/byod/byodrs/init` 给出登录页地址；登录页上的三个输入框全是 `hidden`，表单根本不会被提交，真正的登录是 POST JSON 到 `/byod/byodrs/login/defaultLogin`（密码 base64）。两层都已内置 | 一般不用 |
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

#### 华为 BYOD 报 `E63018: 用户不存在或者用户没有申请该服务`

这个错误码**同时**覆盖两种情况：账号在这一层不存在，或者账号没订阅所选的「服务」。
工具会把门户提供的服务列表打出来：

```
INFO  byod: portal offers services: 7=校园网, 9=中国联通
INFO  byod: using serviceSuffixId 7 (the portal's default)
ERROR ... set service_suffix_id under [portal] to try another
```

按需指定：

```ini
[portal]
service_suffix_id = 9
```

`schoolwifi diagnose` 的 `== byod login policy ==` 段也会列出全部选项。

> 已知限制：如果门户开了**图形验证码**，目前不支持，只能用 `schoolwifi open` 手动登录。

## 常见问题

### `Could not resolve host: xxx.edu.cn`

门户的域名往往**只存在于校园内网 DNS**。如果你在「系统设置 → 网络 → DNS」里
手工写死了公共 DNS（比如 `223.5.5.5`、`8.8.8.8`），这个设置会跨所有网络生效，
于是在校园网里：

- `captive.apple.com` 这种公网域名 → 解析得了，所以能探测到被拦截
- `w.xxx.edu.cn` 这种内网域名 → 公共 DNS 不认识 → 解析失败，拿不到登录页

`schoolwifi` 会自动处理这种情况：解析失败时，它会改用**本网络 DHCP 下发的
DNS**（也就是校园 DNS）去解析，然后把结果直接钉给该次连接，不需要你改系统设置。
连通性探测本身也走这条路径 —— 有些网络（比如宿舍网）认证前会把到公共 DNS
的流量整个挡掉，这时候**所有域名都解析不了**，探测地址也不例外：

```
transport error: Resolving timed out after 6005 milliseconds
```

看到 `Resolving timed out`（而不是 `Connection timed out`）就是这个情况。

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

### `no network path at all` / 所有域名都解析不了

先看 `schoolwifi status` 有没有 IPv4 地址：

```
IPv4         (none)
Link         no IPv4 address on en0 and no default route -- the network has not
             been joined yet (DHCP may still be running)
```

出现 `Link` 这一行说明**机器压根还没连上网**（Wi-Fi 没关联，或刚切换网络、
DHCP 还没完成），不是门户或 DNS 的问题。等几秒、看到 IP 地址再试。

刚切换 Wi-Fi 之后马上跑命令很容易撞上这个。

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
宽带认证，两道的账号密码通常不一样。

**已支持自动串联，而且 `setup` 会主动问你。** 跑 `schoolwifi setup` 时第五个问题
选 `y`，两道的账号密码一次填完，配置文件和钥匙串条目都会自动建好——
下面的手写配置只是说明它生成了什么，正常用不着自己写：

```ini
# ~/.config/schoolwifi/dorm.ini —— 第一层（校园网）
[account]
username = <校园网账号>
keychain_service = schoolwifi

[portal]
next_stage = ~/.config/schoolwifi/dorm-isp.ini
```

```ini
# ~/.config/schoolwifi/dorm-isp.ini —— 第二层（运营商）
[account]
username = <运营商账号>
```

之后在这个网络下直接 `schoolwifi login` 即可，不用加 `-c`——
它按 SSID 自动选到第一层，再自动接上第二层。

日志里能看到交接：

```
INFO  byod: submitting login for <校园网账号>
INFO  stage 1 done; 10.20.30.40 now wants authentication too -- continuing with .../dorm-isp.ini
INFO  submitting login to http://10.20.30.40/... as <运营商账号>
INFO  connected: verified online
```

最多串 4 层，超过会报错而不是无限循环。

> `SCHOOLWIFI_PASSWORD` 只对你直接调用的那一层生效，不会穿透到后续阶段
> ——后面的阶段在别的地方认证，用的是自己的账号。

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
