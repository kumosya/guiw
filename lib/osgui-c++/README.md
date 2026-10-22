# libosgui-c++

独立构建仅需要本目录、已安装 InfoIDE/inpkg 和基础 OSInfo SDK，不读取相邻 info 源码。

```sh
make INFOIDE=/absolute/path/to/infoide OSINFO_SDK=/absolute/path/to/base-sdk
```

产物为 `build/packages/libosgui-c++.inpk`，运行库 SONAME 是 `libosgui-c++.so`。安装映射包括 `/usr/lib/libosgui-c++.so`、`/usr/include/osgui/`、`/usr/share/fonts/osinfo/` 和许可证；编译链接使用 `-losgui`。

`osgui::Window` 保留基本窗口 API，`osgui::Canvas` 提供经能力协商的有界批量绘制，`osgui::Clipboard` 提供最多 3072 字节的跨进程 UTF-8 剪贴板。`session.h` 是只依赖 OS IPC 和同源协议的内联会话适配器，console 不链接 GUI 动态库。

阻塞事件循环使用 `Window::getNextWindowEvent`，需要定期绘制或处理后台结果的应用使用 `Window::pollWindowEvent`。两个接口都会建立事件订阅；应用不要自行发送未订阅的 `GetEvent` 请求。

TTF 使用固定版本 stb_truetype v1.26，来源提交 `f0569113c93ad095470c54bf34a17b36646bbbb5`。解析前校验随包 DejaVu 字体的 SHA-256，拒绝任意外部字体与损坏文件；只支持静态 TrueType 轮廓，不提供复杂文字塑形和系统 IME。字形缓存最多 256 项、512 KiB，按最久未使用项淘汰；缓存位图与颜色无关。

基础 SDK 无法直接使用宿主 FreeType 动态库：宿主 ABI、加载器和 C 运行库与 OSInfo 不同。本实现采用受限后端，已使用 OSInfo 的 freestanding 头文件、目标 libc 数学和内存接口编译链接，不引入宿主 `.so`。字体资源与解析状态在进程内复用。增加字体需审核资源、许可证及覆盖范围，并同步更新 `src/font-digests.h`。

独立验证客户端位于项目根目录的 `build/smoke/`，不进入库包。使用完整 SDK 执行 `infoide build --sdk /absolute/path/to/sdk --project build/smoke`。
