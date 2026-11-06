# Functional Model for Animus GPU

Animus GPU 的 C++20 功能模型，用于验证机器码与编译器结果。当前已完成开发计划 **A1–A4、B1–B7**，未进入 C 阶段。

## 已实现范围

- 单 core、4 个 warp 槽位、每 warp 默认 32 lane；寄存器与控制状态独立存储。
- 带 R/W/X 区域检查的字节数组内存、1/2/4 字节小端辅助读写、4 字节对齐取指。
- 装载原始 `.bin`；执行 RV32I 的整数 ALU、立即数、LUI/AUIPC、整数 load/store、6 种条件分支、JAL/JALR，以及 M 的 8 种乘除指令。
- gpu.split / gpu.join：始终压帧、SINGLE/FIRST/SECOND 路径、最多 8 层；当前 EXEC 的 gpu_tmr 只读访问。
- 先求值并预检全部活动 lane，再统一提交寄存器/内存/控制状态；x0 写丢弃、逻辑步预算和 stop_pc。
- JSONL 指令轨迹（含内存写入、EXEC/SP 变化、实验配置标志），带 PC/warp/lane/地址/规格问题编号的错误。

尚无自动 warp 调度/退出/释放、wspawn/bar/tmc 管理指令、浮点/SFU、Graphics、Command 或 Vulkan 接入。gpu_tmr 支持 CSRRS/CSRRC/CSRRSI/CSRRCI 的零源读取；实际写请求拒绝，其他 CSR 留至 C4。预留的浮点寄存器不是已实现的浮点能力。

### 未确认规则如何处理

- 默认 `allow_experimental=false`：未对齐 guest 数据访问（S06.1）和同一指令内重叠 lane store（S07）返回 ModelLimit，不擅自宣称非法 ISA。
- 测试/嵌入者显式设 `allow_experimental=true` 时，暂按自然对齐违规 fault、lane 编号升序提交冲突 store。CLI 保持默认保守配置。暂行结果不作为硬件保证。
- 所有访存整条预检，失败不提交该条任何 lane 的结果（S06.2）；既有成功指令的写入不回滚。
- 默认 shader 代码范围是入口所在可执行映像；可用 LaunchInfo.code_regions 显式声明多段代码（S10.4）。取指检查范围；普通跳转对齐错误在跳转时报错，代码访问错误在目标取指时报错。自定义 SIMT 的后者错误归属暂参照此方式，记录为 S12 待讨论。

## 构建与测试

需要支持 C++20 的编译器。安装了 CMake 时，在仓库根目录执行：

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

本机已发现 GCC 13.2，可不依赖 CMake，直接构建并运行 A/B 两套测试：

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

## B 阶段运行与验证

原来的两条指令示例继续有效。新增循环案例：

```powershell
.\build\animus_model_cli.exe .\cases\countdown.bin --stop-pc 12
```

预期 7 条成功指令，最终 PC=12、32 lane 的 x1=0。`--stop-pc` 支持十进制或 `0x` 十六进制，达到地址时在取指前暂停；不可达时由总步数预算停止，不将超时当作正常结束。

`build.ps1` 运行原有 7 组回归及 14 组 B 阶段测试（包含 240 个小掩码组合）。也可单独执行 `build/animus_model_tests_b.exe`。若有 Python，可额外用独立的整数参考算法验证 810 组边界/固定种子随机向量，每组检查全部 32 lane：

```powershell
python .\tests\differential_integer.py
```

生成的文件均在被忽略的 build 目录。Python 不是构建依赖。完整 C++ 测试另外覆盖指令编码、访存错误、分支分歧、嵌套栈、只读 CSR 和代码边界；测试成功不等于完整 ISA/Vulkan 完备。

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
| src/decode.cpp | B 阶段指令编码匹配、格式检查和立即数扩展 |
| src/engine.cpp | 分类分派、源值预检、集中提交、单步与 stop_pc |
| include/model/execution.hpp | 执行模块内部共用接口 |
| src/exec_integer.cpp | ALU、M、分支与跳转 |
| src/exec_memory.cpp | guest 整数 load/store、对齐/冲突规则 |
| src/exec_simt.cpp | split/join 与只读 gpu_tmr |
| src/trace.cpp | 轨迹和错误格式化 |
| src/main.cpp | 原始 bin 的最小运行入口 |
| tests/model_tests.cpp | A 阶段回归（已实现的 SUB/移位改为正确识别断言） |
| tests/stage_b_tests.cpp | B1–B7 功能、边界、非法输入与掩码组合 |
| tests/differential_integer.py | 独立整数数学参考算法 |

`evaluate` 只生成 StepEffect；内存变化也先暂存，所有源值来自提交前状态。冲突实验中，memory_writes 的 before 统一表示指令执行前快照，after 按 lane 顺序提交；`commit_effect` 是内部提交边界，只能提交当前状态上成功求值得到的效果。一般调用者使用 `step_shader`，不要手工伪造 StepEffect。宿主初态通过 `initialize_x` 设置；未赋值的普通整数寄存器读取默认产生调试诊断。

内存辅助函数按字节工作，不额外要求数据对齐；这不决定未来 LW/SW 的未对齐语义（S06 尚待确认）。装载映像创建只读/可执行区，区域不允许重叠，一次访问须位于一个区域内。未知指令不会静默跳过；B 范围内检查固定编码，范围外尚未完整译码的指令报告 ModelLimit，不能据此认定它是非法 ISA 程序。

完整设计和讨论日志目前位于本仓库的父目录；README 保留了独立构建和运行所需说明。后续按 C 阶段逐项扩展，不把物理硬件延迟加入逻辑步计数。
