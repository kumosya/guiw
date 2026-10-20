# GUI Window for OS Info

源码与构建独立于 OS 仓库：server/ 为 Ring3 窗口服务，src/ 为图形终端。先安装 InfoIDE，再执行：

```sh
make
# 显式选择 SDK：make OSINFO_SDK=/absolute/path/to/sdk
```

构建入口使用项目根目录的 ./infoide 二进制，已在 .gitignore 中忽略；可用 INFOIDE 指定其他程序，infoide-project.json 决定编译参数及包安装映射。无需相邻 OS 或 InfoIDE 源码。

输出为 build/packages/guiw.inpk，将其放进 OS 的 packages/ 后重新 configure。窗口服务使用 boot-service 目标与 SDK 的 service_start.o，终端使用系统配套的 /lib/ld-osinfo-x86-64.so。
