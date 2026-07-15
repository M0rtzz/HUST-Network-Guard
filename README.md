# HUST-Network-Guard

HUST-Network-Guard 用于持续监控校园网连接，在断网时自动重新认证，并通过 Windows 托盘显示状态通知。

## 使用指南

### 1. 下载程序

从项目的 **GitHub Releases** 页面下载 `HUST-Network-Guard-<版本>-windows-x64.zip` 并解压。

无需安装编译器。运行时请保持解压目录中的文件位于同一目录，不要单独移动 `HUST-Network-Guard.exe`。

---

### 2. 配置认证信息

复制配置模板：

```bat
copy .env.example .env
```

编辑 `.env`，填写校园网账号和明文密码：

```dotenv
HUST_USER_ID=学号
HUST_PLAIN_PASSWORD=明文密码
HUST_CHECK_INTERVAL_SECONDS=5
HUST_FAILURE_THRESHOLD=5
```

程序在断网时访问 `https://www.baidu.com` 获取校园网返回的认证页面，动态提取认证服务器、MAC 和 `queryString`，再使用明文密码生成登录所需的 256 位加密密码。因此不需要抓包，也不需要配置固定的认证地址或设备校验码。

`HUST_CHECK_INTERVAL_SECONDS` 是联网正常时的检测周期。`HUST_FAILURE_THRESHOLD` 是确认断网前所需的连续失败次数，必须至少为 1。第一次失败后会每 5 秒复检，不再等待正常检测周期。

`.env` 包含明文密码，不要截图或分享。程序从 `HUST-Network-Guard.exe` 所在目录读取该文件，修改后需要重启程序。

---

### 3. 运行和自启动

* **方案A.**：双击 `HUST-Network-Guard.exe`，或为其创建桌面快捷方式。
* **方案B.(推荐)**：设置登录时自启
  1. 按 `Win + R`，输入 `shell:startup` 并回车。
  2. 该命令会打开 `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup`，其中 `%APPDATA%` 等价于 `C:\Users\<用户名>\AppData\Roaming`。
  3. 在解压目录中右键 `HUST-Network-Guard.exe`，选择“创建快捷方式”。
  4. 将创建的 `"HUST-Network-Guard.exe.lnk"` 复制到刚打开的 `Startup` 目录。

不要移动或删除原始解压目录；快捷方式需要从该目录启动程序，以便读取同目录的 `.env` 和 `libcurl-x64.dll`。

右键托盘图标可使用以下命令：

* **立即检测**：立即检测实际互联网连接，失败会计入连续失败次数。
* **测试登录配置**：立即向校园网认证服务器发送一次登录请求，并在通知和日志中显示服务器是否接受，以及 `message` 返回的具体原因。HTTP 200 本身不再视为认证成功。
* **打开日志**：打开 `HUST-Network-Guard.log`。

严格验证密码时，应先在校园网管理系统将当前设备下线，再选择“测试登录配置”。已经联网时无法取得校园网认证页面，因此不会发送测试登录请求。

动态认证流程参考 [https://github.com/black-binary/hust-network-login](https://github.com/black-binary/hust-network-login)，密码加密仍在本地完成，不会写入日志。
