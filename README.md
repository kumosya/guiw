# GUI Window for OS Info

`lib/osgui-c++/` 是独立 GUI 客户端与 TTF 运行库，包名为 `libosgui-c++`。`server/` 是窗口服务；`app/tools/` 包含 terminal、calc、taskmgr，`app/editor/` 是文本编辑器。

需要 Linux 或 WSL、gcc/g++、已安装的 InfoIDE 和 inpkg。构建只需两步：

```sh
# 库修改后先独立构建，再把头文件和运行库安装到 InfoIDE SDK
make -C lib/osgui-c++
sudo sh tools/install-sdk.sh
# 调用 InfoIDE 构建、打包
make
```

头文件默认安装到 `/usr/local/lib/osinfo/sdk/include/osgui-c++/osgui/`。`OSINFO_SDK` 可指定 SDK 根目录，`INFOIDE_HOME` 可指定 InfoIDE 资源目录；两个脚本使用相同规则。SDK 需已有配套运行库。`INFOIDE=/path/to/infoide` 可指定构建工具。

发布目录 `build/release/` 包含 `libosgui-c++.inpk`、`guiw.inpk`、`guiw-app.zip`。ZIP 由 `infoide bundle` 生成，包含 `guiw-app-tools.inpk` 和 `guiw-editor.inpk`。安装时先库、再服务器、最后应用；ZIP 需先解开。

先执行 `make` 构建各包，再运行 `infoide run`。`infoide-debug.json` 的 `dependencies` 已列出配套的 `libosgui-c++` 和 `guiw-app-tools`，新版 InfoIDE 会先安装它们，再安装服务器。tools 包提供 `/usr/bin/terminal`；镜像缺少该包会导致 init 启动 GUI 终端时报告 `executable stat failed`。

项目已有的 `osinfo.img` 会继续使用，不会随 InfoIDE 模板自动更新。旧镜像还需同步配套 OS 运行库和 `/etc/sessions`：运行库过旧会报告缺少 `os::Task::query`，缺少会话配置会报告 `[TERMINAL] invalid /etc/sessions`。

升级 OS 启动服务或协议后，应从新版 `make export-images` 产物创建新的项目镜像。只升级 `/usr/lib` 和应用包无法更新旧镜像的内核、`/boot` 服务和 GRUB 模块。例如在项目根目录执行：

```sh
./infoide install --disk /path/to/new/osinfo.img --output osinfo-current.img
./infoide run
```

当前调试配置使用 `osinfo-current.img`；原 `osinfo.img` 保留。创建时使用尚不存在的输出文件名，已有项目镜像会继续增量更新。

库可使用基础 SDK 独立构建：

```sh
make -C lib/osgui-c++ OSINFO_SDK=/path/to/base-sdk
```

宿主验证入口为 `sh tests/run.sh`（需要 Perl 标准模块），包验证为 `sh tests/packages.sh`（需要 unzip）。冒烟文件只放在 `build/`。
