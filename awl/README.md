# AWL 主机逻辑与协议测试

本目录保留 AndroidWayland（AWL）的纯 C 协议逻辑和主机测试环境，用于研究、复用和验证相关机制。它不是替代 KWin/Mutter 的完整 DE，也不包含 Android renderer / Binder 适配层的构建。

## 内容与依赖

- `include/`：AWL 接口。
- `services/waylandbridge/`：surface、dmabuf、输入法、输入、调度等协议逻辑。
- `services/waylandbridge/protocols/`：构建所需的 Wayland 扩展 XML。
- `third_party/wayland/`：AWL 修改过的 libwayland 源码及核心协议，不可作为普通缓存删除。
- `third_party/`：配置、键盘映射和依赖头文件。
- `tests/`：主机协议测试。
- `CMakeLists.txt`：构建静态库 `wayland-server-static`、`awllogic` 和两个测试程序。

顶层 CMake 通过 `ANLAND_BUILD_AWL_TESTS=ON` 加入此目录，默认关闭。WM 公共层中的 `libdisplay_producer/anland_present_awl.c` 是独立的呈现 IPC 适配代码，不等同于本目录；不可因为清理此目录而删除它。

## 构建位置

建议构建到仓库外的缓存目录，而不是在本目录保留产物。例如在仓库根目录运行：

```sh
cmake -S awl -B "$HOME/.cache/anland-awl-host-build"
cmake --build "$HOME/.cache/anland-awl-host-build" --parallel 4
ctest --test-dir "$HOME/.cache/anland-awl-host-build" --output-on-failure
```

需要 C 编译器、CMake、pkg-config、libffi 开发包、wayland-scanner 和 wayland-client 开发包。上述命令是重建说明，不表示清理操作已执行过编译或测试。

## 已清理的材料

2026-09-27 将误随测试树同步进入主仓库的以下内容移到 `~/.cache/anland-review/awl-cleanup-20260927-171206/`：

- `build/`：旧二进制、静态库、生成代码及 CMake 缓存。
- `wayland-protocols-1.49/`：下载后解包的完整协议源码，含 Debian/Quilt 材料。
- `wayland-protocols_1.49*`：下载的源码包、描述文件和签名。

当前 CMake 使用本目录内已保留的协议 XML，不引用上述下载/解包材料。清理时核对了保留的 63 个文件哈希和 `consumers/` 内容，均未改变。归档用于恢复；移动旧 build 缓存不代表其在新路径可直接复用，应按上面的命令重新配置。

本目录应保留源码、测试、所需协议及第三方许可信息；下载包和构建产物不要再次同步入主仓库。
