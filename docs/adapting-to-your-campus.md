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

同时它会把门户原始 HTML 存成 `schoolwifi-portal-<时间戳>.html`，
自动识别搞不定的时候可以直接翻这个文件。

> 这个 HTML 里可能含有你的 IP、MAC、会话 ID。**提 issue 前记得先脱敏。**

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

### 4. 门户根本没有表单，是个 JS 调的 API

有些认证系统（比如深澜 Srun）不走表单，而是前端用 JS 算好参数直接打 API。
这种情况用 `raw` 模式，把请求原样重放：

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

> 注意：如果这个 API 的参数里有 JS 现算的哈希/加密串（Srun 的 `chksum`、
> `info` 就是），`raw` 模式重放固定值是不行的。这类门户目前不支持，
> 欢迎提 issue 带上（脱敏后的）请求样本。

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
