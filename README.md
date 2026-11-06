# Functional Model for Animus GPU

Animus GPU 的 C++20 功能模型，用于验证机器码与编译器结果。当前仅完成开发计划 **A1–A4**。

## 已实现范围

- 单 core、4 个 warp 槽位、每 warp 默认 32 lane；寄存器与控制状态独立存储。
- 带 R/W/X 区域检查的字节数组内存、1/2/4 字节小端辅助读写、4 字节对齐取指。
- 装载原始 `.bin` 代码，严格译码和执行 **ADDI、ADD**。
- 单条 warp 指令的“预检/求值 → 集中提交”，x0 写丢弃、活动掩码、未初始化整数源检查、逻辑步预算。
- JSONL 成功指令轨迹、带 PC/warp/lane 上下文的错误和自动化回归。

尚无其他指令执行、split/join、warp 调度/退出/释放、SFU、图形、Command 或 Vulkan 接入。预留的浮点寄存器和栈只是状态结构，不表示相应指令已实现。物理 SFU 数量不进入本功能阶段。

## 构建与测试

需要支持 C++20 的编译器。安装了 CMake 时，在仓库根目录执行：

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

本机已发现 GCC 13.2，可不依赖 CMake，直接构建并运行测试：

```powershell
.\build.ps1 -Compiler 'D:\mingw-19.0\MinGW\bin\g++.exe'
```

若 PowerShell 阻止脚本运行，可仅为这次脚本启动新的进程：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Compiler 'D:\mingw-19.0\MinGW\bin\g++.exe'
```

## 运行两条示例机器指令

使用上面的本机构建方式时：

```powershell
$env:PATH = 'D:\mingw-19.0\MinGW\bin;' + $env:PATH
.\build\animus_model_cli.exe .\cases\addi_add.bin 2
```

CMake/Visual Studio 构建可能将程序放在 `build/Debug/`。示例每 lane 执行 `x1=5; x2=x1+x1`，输出两行 JSON。停止计数是宿主调试条件，不是 shader 结束。错误返回非零进程退出码，说明写入标准错误；成功轨迹写入标准输出，可重定向到文件。

## 模块和接口

### VS Code 标准库头文件提示

请用“文件 → 打开文件夹”打开本仓库根目录（包含 CMakeLists.txt 的目录）。若提示无法打开 string、array、span 等标准库头文件，执行 `C/C++: Edit Configurations (UI)`，设置 Compiler path 为本机 C++ 编译器路径，C++ standard 为 C++20，IntelliSense mode 为 windows-gcc-x64，Include path 加入 `${workspaceFolder}/include`。

本机已配置 `.vscode/c_cpp_properties.json`，编译器为 `D:/mingw-19.0/MinGW/bin/g++.exe`；该文件含本机路径，已加入忽略规则，同学需按自己的安装位置配置。标准库头文件由编译器提供，不应复制进仓库或逐个添加系统 include 路径。修改后可执行 `Developer: Reload Window` 刷新；若仍异常，使用 `C/C++: Log Diagnostics` 检查实际生效配置。

### 文件职责

| 文件 | 职责 |
| --- | --- |
| include/model/model.hpp | 公共类型、函数声明、状态与结果契约 |
| src/common.cpp | 配置、模型创建、宿主 warp 启动与初态设置 |
| src/memory.cpp | 内存、权限、区域映射与取指 |
| src/loader.cpp | 文件检查与原始机器码装载 |
| src/decode.cpp | ADDI/ADD 编码匹配、立即数扩展 |
| src/engine.cpp | 只读求值、集中提交、单步与预算 |
| src/trace.cpp | 轨迹和错误格式化 |
| src/main.cpp | 原始 bin 的最小运行入口 |
| tests/model_tests.cpp | 不依赖第三方测试框架的断言测试 |

`evaluate` 只生成 StepEffect；`commit_effect` 是内部提交边界，只能提交当前状态上成功求值得到的效果。一般调用者使用 `step_shader`，不要手工伪造 StepEffect。宿主初态通过 `initialize_x` 设置；未赋值的普通整数寄存器读取默认产生调试诊断。

内存辅助函数按字节工作，不额外要求数据对齐；这不决定未来 LW/SW 的未对齐语义（S06 尚待确认）。装载映像创建只读/可执行区，区域不允许重叠，一次访问须位于一个区域内。未知指令不会静默跳过；A 阶段未完整译码的其他编码报告 ModelLimit，不能据此认定它是非法 ISA 程序。

完整设计和讨论日志目前位于本仓库的父目录；README 保留了独立构建和运行所需说明。后续按 B 阶段逐项扩展，不把物理硬件延迟加入逻辑步计数。
