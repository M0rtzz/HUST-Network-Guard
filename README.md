# HUST-Network-Guard

HUST-Network-Guard 用于持续监控校园网连接，在断网时自动重新认证，并通过 Windows 托盘显示状态通知。

Windows 下运行 `build.bat` 即可使用仓库内置的 cURL 完成编译，无需单独安装或配置 cURL。

---

## 使用指南

### 1. 准备工作

* 确保已将 64 位 **MinGW-w64 g++** 加入 `PATH`
* 连接网线

---

### 2. 获取加密密码和设备静态校验码（以Edge浏览器为例）

1. 如果电脑已登录校园网，请先在校园网自助服务系统 [http://myself.hust.edu.cn:8080/selfservice/](http://myself.hust.edu.cn:8080/selfservice/) 将此设备“下线”。
2. 打开浏览器，任意访问一个网页，会自动跳转到校园网认证界面。
3. 按 **F12** 打开检查窗口，切换到 **网络 (Network)**。
4. 输入校园网帐号密码，点击“连接”登录校园网。
5. 在请求中找到 `InterFace.do?method=login` → **负载 (Payload)** → **表单数据 (Form Data)**，即可获取：

   * `password`（加密密码）
   * `queryString`（设备校验码）

示例截图：

![HUST校园网认证界面](https://github.com/user-attachments/assets/d40d7396-ea97-425e-9200-e917c9ada3cc)
![获取表单数据](https://github.com/user-attachments/assets/65930177-1e38-47df-b364-43e64fbaf314)

---

### 3. 配置认证信息

复制配置模板：

```bat
copy .env.example .env
```

编辑 `.env` 并填写从登录请求中获取的认证信息：

```dotenv
HUST_USER_ID=学号
HUST_PASSWORD=加密密码
HUST_QUERY_STRING=设备校验码
HUST_LOGIN_URL=http://172.18.18.60:8080/eportal/InterFace.do?method=login
HUST_CHECK_INTERVAL_SECONDS=30
HUST_FAILURE_THRESHOLD=3
```

`HUST_LOGIN_URL` 必须与抓包中登录请求的目标地址一致；不同网络环境使用的认证服务器可能不同。`HUST_CHECK_INTERVAL_SECONDS` 是联网正常时的检测周期，默认 30 秒，可设置为 5～3600 秒。`HUST_FAILURE_THRESHOLD` 是确认断网前所需的连续失败次数，默认 3 次，可设置为 1～10 次。第一次失败后会每 5 秒复检，不再等待正常的 30 秒周期。

`.env` 包含敏感信息且已被 Git 忽略，不要将其提交或分享。程序从 `HUST-Network-Guard.exe` 所在目录读取该文件，修改后需要重启程序。

---

### 4. 编译并封装为可执行文件（以 Windows 为例）

#### 使用 C++ 编译

1. 安装 64 位 **MinGW-w64 g++**，无需另外安装 cURL。推荐在 MSYS2 UCRT64 终端运行：

   ```bash
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc
   ```

   `build.bat` 会自动查找 `C:\msys64\ucrt64\bin`。目标为 `mingw32` 或 `i686-w64-mingw32` 的 32 位编译器不能链接仓库内置的 x64 cURL。
2. 双击 `build.bat`，或在项目目录运行：

   ```bat
   .\build.bat
   ```
   重新编译前需要先右键托盘图标并选择“退出”，否则 Windows 会锁定正在运行的 EXE。
3. 编译完成后，`outs` 目录中会生成 `HUST-Network-Guard.exe`、`libcurl-x64.dll` 和本地 `.env`。运行或移动程序时，必须保持这些文件位于同一目录。

手动编译的等效命令为：

```bat
if not exist ".\outs" mkdir ".\outs"
windres -I".\resources" -I"." ".\resources\HUST-Network-Guard.rc" -O coff -o ".\outs\HUST-Network-Guard-resource.o"
g++ -std=c++17 -O2 -Wall -Wextra -pthread HUST-Network-Guard.cc ".\outs\HUST-Network-Guard-resource.o" -o ".\outs\HUST-Network-Guard.exe" -mwindows -I".\3rdparty\curl\include" -L".\3rdparty\curl\lib" -static-libgcc -static-libstdc++ -lcurl -lshell32
del /Q ".\outs\HUST-Network-Guard-resource.o"
copy /Y ".\3rdparty\curl\bin\libcurl-x64.dll" ".\outs\libcurl-x64.dll"
copy /Y ".\.env" ".\outs\.env"
```

---

### 5. 运行可执行文件

* **方案A.**：双击 `outs\HUST-Network-Guard.exe`，或为其创建桌面快捷方式。不要单独移动 EXE。
* **方案B.(推荐)**：设置登录时自启
  1. 按 `Win + R`，输入 `shell:startup` 并回车。
  2. 该命令会打开 `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup`，其中 `%APPDATA%` 等价于 `C:\Users\<用户名>\AppData\Roaming`。
  3. 将 `outs` 中的 `HUST-Network-Guard.exe`、`.env` 和 `libcurl-x64.dll` 一起复制到该目录。

右键托盘图标可使用以下命令：

* **立即检测**：立即检测实际互联网连接，失败会计入连续失败次数。
* **测试登录配置**：立即向校园网认证服务器发送一次登录请求，并在通知和日志中显示服务器是否接受，以及 `message` 返回的具体原因。HTTP 200 本身不再视为认证成功。
* **打开日志**：打开 `HUST-Network-Guard.log`。

严格验证密码时，应先在校园网管理系统将当前设备下线，再选择“测试登录配置”。已经联网时，认证服务器可能只返回“已在线”，不能据此证明密码正确。


## 🎉大功告成！每次开机或唤醒将自动连接校园以太网！
