# 适配你自己学校的校园网

`schoolwifi login` 默认会自动探测门户、自动识别表单字段。大部分学校到这一步
就能用了。如果失败，这篇文档讲怎么把 `[portal]` 段填对。

整个过程只需要一个命令的输出：

```bash
# 先连上校园网（连上、但还没认证），然后：
schoolwifi diagnose
```

## 读懂 diagnose 的输出

```
== probe ==
url       : http://captive.apple.com/hotspot-detect.html
state     : captive
status    : 302
location  : http://10.0.0.1/portal/index.jsp

== portal discovery ==
  redirect: http://10.0.0.1/portal/index.jsp
  meta refresh: http://10.0.0.1/portal/login.jsp
  login form found at: http://10.0.0.1/portal/login.jsp
login page: http://10.0.0.1/portal/login.jsp
page title: 校园网认证

== forms ==
  form #0  action=/eportal/login.do method=post id=loginForm  <- has password field
      csrfToken                type=hidden     value=a1b2c3
      userName                 type=text       value=
      userPwd                  type=password   value=(hidden)
      domain                   type=select     value=cmcc

== what login would submit ==
  POST http://10.0.0.1/eportal/login.do
  username_field = userName
  password_field = userPwd
```

同时它会把现场存进 `schoolwifi-diagnose-<时间戳>/`：

```
schoolwifi-diagnose-20260922-001231/
├── portal.html          门户页面本身
├── 01-customCommon.js   页面加载的同源脚本，按加载顺序编号
└── 02-index.js
```

`== forms ==` 显示 `(none found)` 时，答案基本都在这些 JS 里 —— 很多门户
（华为 BYOD、深澜等）页面上是空的，登录逻辑全在脚本里。第三方 CDN 上的脚本
不会被抓取。

> 这些文件和页面 URL 里含有你的 IP、MAC、会话 ID。**提 issue 前记得先脱敏。**

## 常见情况

### 1. 字段识别错了

`== forms ==` 里能看到字段，但 `username_field` / `password_field` 选错了
（比如选到了验证码框）。直接指定：

```ini
[portal]
username_field = userName
password_field = userPwd
```

### 2. 少了运营商 / 域之类的字段

有些学校要选中国移动/电信/联通，或者要带一个 `domain`。看 `== forms ==`
里有哪些字段，用 `field.<名字>` 补上（会覆盖表单里的默认值）：

```ini
[portal]
field.domain = cmcc
field.operator = telecom
```

### 3. 找不到登录页 / 跳转链断了

`== portal discovery ==` 停在某一跳，或者 `login form found` 那行没出现。
自己用浏览器打开登录页，把地址栏的地址填进去，跳过自动探测：

```ini
[portal]
login_url = http://10.0.0.1/portal/login.jsp
```

### 4. 门户根本没有表单（深澜 Srun）

`== forms ==` 显示 `(none found)`，`page title` 是 `Srunsoft`，
地址里带 `srun_portal`：

```
login page: https://w.example.edu.cn/srun_portal_pc?ac_id=1&theme=pro
page title: Srunsoft

== forms ==
  (none found)
```

**这种已经内置支持了，不用配置。** 深澜门户是纯 JS 单页应用，页面上的
`<input>` 只有 `id` 没有 `name`，也没有 `<form>` 标签 —— 所以扫不到表单是正常的，
不是 bug。登录参数要用服务端下发的 challenge 现算（XXTEA 加密 + 自定义 base64
+ HMAC-MD5 + SHA1 签名），`schoolwifi` 会自动识别并走这条路径。

如果自动识别没生效，强制指定：

```ini
[portal]
login_method = srun
login_url = https://w.example.edu.cn/srun_portal_pc?ac_id=1
```

`ac_id` 一般能从页面里的 `var CONFIG = { acid : "1", ... }` 读到，读不到默认用 `1`。

> 开了图形验证码的深澜门户目前不支持。

### 4a. 华为 BYOD 门户

`page title` 是 `BYOD`、地址里有 `/byod/`、`== forms ==` 是 `(none found)`：

```
login page: http://10.x.x.x:30004/byod/index.html?usermac=...&userip=...&ssid=E
page title: BYOD
```

**已内置支持，不用配置。** 这个页面是空壳，真正的登录页地址藏在
`/byod/byodrs/init` 接口里，工具会替你发这个请求并跟过去。日志里会看到：

```
INFO  byod: asking http://10.x.x.x:30004/byod/byodrs/init where the login page is
INFO  byod: portal says the login page is http://...
INFO  portal hop (byod init): http://...
```

如果失败，`schoolwifi-diagnose-*/byod-init.json` 里是接口的原始返回，
提 issue 时带上它（记得脱敏 IP/MAC）。

### 4b. 其它 API 式门户

不是深澜、但同样没有表单、直接调接口的，用 `raw` 模式把请求原样重放：

```ini
[portal]
login_method = raw
http_method = POST
login_url = http://10.0.0.1:801/eportal/
post_body = user_account={username|url}&user_password={password|url}&wlan_user_ip=10.12.33.7
```

占位符：`{username}`、`{password}`，以及 URL 编码版的
`{username|url}`、`{password|url}`。

要拿到真实的请求格式，在 **Safari 或 Chrome 里手动登录一次**，
开开发者工具的 Network 面板，找到登录那个请求，看它的 URL 和 Request Payload，
照着填 `login_url` 和 `post_body` 即可。

> 注意：如果这个 API 的参数里有 JS 现算的哈希/加密串，`raw` 模式重放固定值是不行的。
> 欢迎提 issue 带上（脱敏后的）请求样本。

### 4c. 页面只有隐藏字段、加载就自动提交

```
== forms ==
  form #0  action=http://portal.example.com:80/index.do method=post
      basPushUrl               type=hidden     value=
      testmacauth              type=hidden     value=false
```

配合页面里的 `<body onload="...">` + `document.forms[0].submit()`，
这不是登录页，而是**用 POST 做的一次跳转**——提交之后返回的才是真正的登录页。
运营商门户（如广东联通 `portal.gd165.com`）常用这种写法。

**已内置支持**，日志里会看到：

```
INFO  portal hop (form the page submits itself): POST http://portal.example.com:80/index.do
```

名字里含 `url` 且值为空的隐藏字段会被填上当前页面地址，
和页面自己的脚本（`value = window.parent.location.href`）做的事一致。

### 5. 登录了但判定成失败

默认的成功判定是「重新探测一次，网络真通了才算成功」，通常最可靠。
如果你的网关认证后需要较长时间才放行，可以改成直接看响应内容：

```ini
[portal]
success_contains = 登录成功
failure_contains = 密码错误
```

### 6. 密码里有特殊字符

表单模式下会自动做 URL 编码，不用管。`raw` 模式下**必须**用
`{password|url}` 而不是 `{password}`。

### 7. `Could not resolve host`（门户域名解析不了）

```
== portal discovery ==
  javascript redirect: https://w.bnbu.edu.cn/index_1.html
  note: fetch failed: Could not resolve host: w.bnbu.edu.cn
```

门户域名通常只存在于校园内网 DNS。如果你在系统设置里写死了公共 DNS，
它会跨所有网络生效，公网域名解析得了、内网域名解析不了。

先看 `schoolwifi diagnose` 的 `== dns ==` 段：

```
== dns ==
system    : 223.5.5.5        <- 系统实际在用的
dhcp      : 10.253.0.1       <- 本网络下发的（校园 DNS）
note      : the system resolver ignores this network's DNS ...
```

两行不一致就是这个问题。`schoolwifi` 会自动改用 `dhcp` 那台去解析门户域名，
一般不用你做什么。自动回退也失败时，手工指定校园 DNS：

```ini
[network]
dns_server = 10.253.0.1
```

校园 DNS 的地址从 `dhcp` 那一行抄；那行是空的话，用网关地址试试
（`netstat -rn -f inet | awk '$1=="default"{print $2}'`）。

## 调试技巧

```bash
# 打印每一个 HTTP 请求、状态码和跳转
schoolwifi -v login

# 用环境变量临时试密码，不动钥匙串
SCHOOLWIFI_PASSWORD='xxx' schoolwifi -v login

# 不确定配置有没有被读到？status 最后一行会打印配置文件路径
schoolwifi status
```

## 贡献你学校的配置

调通之后欢迎提 PR，把能用的 `[portal]` 段加到本文档下面这个列表里
（**记得删掉 username、IP、MAC 等个人信息**）。

### 已知可用的配置

<!-- 按学校/认证系统追加，格式：
#### 某某大学（锐捷 / Dr.COM / ePortal / ...）
```ini
[portal]
...
```
-->

*(还是空的 —— 欢迎成为第一个。)*
