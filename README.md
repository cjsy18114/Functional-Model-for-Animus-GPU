# Functional Model for Animus GPU

Animus GPU 的 C++20 功能模型，用于验证机器码与编译器结果。当前已完成开发计划 **A1–A4、B1–B7、C1–C5、D1–D3** 的实现与本机测试；未进入 E 阶段。

## 已实现范围

- 单 core、4 个 warp 槽位、每 warp 默认 32 lane；寄存器与控制状态独立存储。
- 带 R/W/X 区域检查的字节数组内存、1/2/4 字节小端辅助读写、4 字节对齐取指。
- 装载原始 `.bin`；执行 RV32I 的整数 ALU、立即数、LUI/AUIPC、整数 load/store、6 种条件分支、JAL/JALR，以及 M 的 8 种乘除指令。
- gpu.split / gpu.join：始终压帧、SINGLE/FIRST/SECOND 路径、最多 8 层；当前 EXEC 的 gpu_tmr 只读访问。
- 先求值并预检全部活动 lane，再统一提交寄存器/内存/控制状态；x0 写丢弃、逻辑步预算和 stop_pc。
- JSONL 指令轨迹（含内存写入、EXEC/SP 变化、实验配置标志），带 PC/warp/lane/地址/规格问题编号的错误。

支持宿主四 warp 轮询、测试结束和回收复用；RV32F 的 26 条指令（包括 FLW/FSW、单次舍入 FMA、五种舍入模式、NaN/Inf/异常标志）；六种 CSR 操作、FENCE、ECALL/EBREAK；六种 SFU 参考运算。D 阶段增加 gfx.get/tex/end、人工 VS/FS 上下文与固定输出副本；gpu_tmr 仍只读。没有 wspawn/bar/tmc 管理指令、Command、DRAW/光栅/深度测试或 Vulkan 接入；异常处理器与正式任务派发 ABI 尚未定义。

### 未确认规则如何处理

- 默认 `allow_experimental=false`：未对齐 guest 数据访问（S06.1）和同一指令内重叠 lane store（S07）返回 ModelLimit，不擅自宣称非法 ISA。
- 测试/嵌入者显式设 `allow_experimental=true` 时，暂按自然对齐违规 fault、lane 编号升序提交冲突 store。CLI 保持默认保守配置。暂行结果不作为硬件保证。
- 所有访存整条预检，失败不提交该条任何 lane 的结果（S06.2）；既有成功指令的写入不回滚。
- 默认 shader 代码范围是入口所在可执行映像；可用 LaunchInfo.code_regions 显式声明多段代码（S10.4）。取指检查范围；普通跳转对齐错误在跳转时报错，代码访问错误在目标取指时报错。自定义 SIMT 的后者错误归属暂参照此方式，记录为 S12 待讨论。

## 构建与测试

需要 C++20 与 C11 编译器。标准浮点使用仓库内附的 SoftFloat 3e 精简源码，不需联网下载；来源、校验值和许可见 `third_party/softfloat/`。当前平台配置要求小端主机；模型按单宿主线程使用。安装了 CMake 时，在仓库根目录执行：

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

本机使用 GCC 13.2，可不依赖 CMake，直接构建并运行 A/B/C/D 四套测试（脚本要求 g++.exe 同目录有 gcc.exe）：

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

`build.ps1` 当前运行 A/B/C/D 四套测试，其中 B 为 14 组（包含 240 个小掩码组合）。也可单独执行 `build/animus_model_tests_b.exe`。若有 Python，可额外用独立的整数参考算法验证 810 组边界/固定种子随机向量，每组检查全部 32 lane：

```powershell
python .\tests\differential_integer.py
```

生成的文件均在被忽略的 build 目录。Python 不是构建依赖。完整 C++ 测试另外覆盖指令编码、访存错误、分支分歧、嵌套栈、只读 CSR 和代码边界；测试成功不等于完整 ISA/Vulkan 完备。

## 模块和接口

### C 阶段运行与验证

在仓库根目录先执行上面的构建命令；它会自动运行 A 的 7 组、B 的 14 组、C 的 13 组和 D 的 14 组测试。C 测试最后打印 `C instruction coverage: 41/41`：该阶段新增 41 种操作均执行检查，其中 ECALL/EBREAK 检查预期异常。

```powershell
$env:PATH = 'D:\mingw-19.0\MinGW\bin;' + $env:PATH
.\build\animus_model_tests_c.exe
python .\tests\differential_integer.py
python .\tests\differential_float.py
.\build\animus_model_cli.exe .\cases\fp_arithmetic.bin 6 --experimental-numeric
.\build\animus_model_cli.exe .\cases\sfu_rsqrt.bin 4 --experimental-numeric
```

两个示例分别得到每 lane 的 x3=0x40700000（浮点 3.75 的位模式）和 x3=0x3F000000（0.5 的位模式），终止 PC 分别为 24、16；详细机器字见 `cases/README.md`。第二个参数是成功执行的逻辑指令数，仍是宿主暂停条件。

`--experimental-numeric` 显式同时选择 `PerLaneExperimental` 和 `HostMathV1`，会在标准错误中提示，轨迹分别记录 `fp_state_policy`、`sfu_profile`。它不启用 B 的冲突访存实验策略。不传该参数时，FLW/FSW、位搬运、符号和分类可执行；涉及浮点舍入/异常状态的运算与 CSR 在 S04 处暂停，SFU 在 S05 处暂停。这样避免把待讨论规则当作正式 ISA。

- C++ 测试检查四 warp 独立状态与共享内存、槽位复用、完整 F/CSR/SFU 指令分组、掩码、错误原子性、浮点特殊数和轨迹。f0 可写，x0 不可写。
- `differential_float.py` 用 Python Fraction 精确有理数和整数平方根独立核对 900 组浮点结果及异常标志，覆盖五种舍入模式和 32 lane；另用高精度 Decimal 参考检查 42 组 SFU 有限输入。4 ULP 只是这些 SFU 用例的验收条件，不保证所有输入或未来硬件误差。
- `differential_integer.py` 保留 B 的 810 组独立整数参考检查。Python 仅是额外测试依赖。脚本生成文件写入被忽略的 build 目录。

浮点标准运算由软件逐位计算，不依赖宿主 float 的默认舍入；SFU 的 HostMathV1 单独采用 double 数学库，弧度制、RNE 转 FP32、规范 NaN、不更新 guest fflags。未确认事项集中在上层 `ISA改进思考记录日志.md` 的 C 阶段独立章节。通过这些测试不等于 Vulkan 1.3 完备性认证。

### 四 warp 宿主接口

先装载各段代码、用 `create_warp` 创建上下文，再用 `initialize_x` / `initialize_f` 设置每 lane 初态。`initialize_f` 的 value 是 FP32 原始 32 位，不是宿主浮点值。

`run_warps_until_stops(machine, stops, trace)` 的 stops 为 1–4 个唯一的 `{warp, stop_pc}`，只轮询所选 Ready warp，每次执行一条；其他 warp 不运行。在各自 stop_pc 前暂停后，逐个 `finish_test_warp(machine, id, stop_pc)`，要求空重汇合栈且 EXEC 恢复初始掩码；随后 `release_warp(machine, id)` 返回最终状态快照并清空槽位。故障 warp 也可释放；仍在运行、等待或仅暂停的 warp 不能直接释放。新调用有新的 `invocation_id`，可区分同槽位的前后任务。

这套接口供测试使用，不是 guest 机器指令或正式派发 ABI；不会模拟硬件时钟、两个 SFU 的争用或 barrier。没有 Ready warp 而仍有未完成者时返回 SuspectedDeadlock 诊断。

### D 阶段：人工 VS/FS 图形接口

D 仅实现 shader 图形接口，不运行完整 DRAW：

- `gfx.get` 按阶段读取人工提供的 VS/FS selector；浮点输入以原始位模式写入整数寄存器，不代算插值。
- `gfx.tex` 在 VS/FS 中均可用，RGBA8、nearest、normalized UV、LOD0、clamp-to-edge；NaN/-Inf→0、+Inf→1；支持行间 padding，检查地址、布局和读取权限。
- `gfx.end` 检查重汇合并复制固定输出：VS 为 f0–f3 和 N 个 varying，FS 为 x10 及原始片元身份/深度。只采集 INIT_MASK lane；提交后 Ended，不再执行后续指令。输出副本、状态和身份同时写入轨迹。

在仓库根目录执行完整构建后，可单独复验：

```powershell
$env:PATH = 'D:\mingw-19.0\MinGW\bin;' + $env:PATH
.\build\animus_model_tests_d.exe
.\build\animus_graphics_demo.exe .\cases\fs_texture.bin > .\build\d_fs_trace.jsonl
python .\tests\differential_texture.py
```

预期 D 的 **14 组 PASS**、`D instruction coverage: 3/3`；示例提示 `Completed 4 fragment outputs via gfx.end in 8 logical instructions.`；独立参考打印 `PASS texture Fraction oracle: 1024 UV pairs, machine-code FS, packed color and output identity`。整次构建共运行 **48 组 C++ 测试**。

示例由专用宿主工具装载人工输入与 3×2 小纹理（stride=16），真正的 UV 读取、采样和结束由 `.bin` 内的 8 条机器指令完成。最后一行 JSON 的 `outputs.fragments` 中，lane 0–3 颜色依次为 `FF0000FF`（红）、`FFFF0000`（蓝）、`FFFFFFFF`（白）、`FFFF00FF`（品红）；R 位于最低字节。可这样查看：

```powershell
$records = @(Get-Content .\build\d_fs_trace.jsonl | ForEach-Object { $_ | ConvertFrom-Json })
$records[-1].outputs.fragments | Select-Object lane, pixel_x, pixel_y, @{Name='RGBA8';Expression={'{0:X8}' -f $_.rgba8}}
```

`animus_graphics_demo` 是验证用宿主工具，普通 `animus_model_cli` 默认仍创建 Test 阶段，不能只给它一个图形 `.bin` 就获得 VS/FS 输入。示例不需要 `--experimental-numeric`：没有使用依赖浮点 CSR 的运算或 SFU。独立纹理脚本用 Fraction 精确取 texel，另检查 packed 原色与输出身份；这不代替完整光栅或 Vulkan 验证。

使用 C++ 接入自己的 shader 时，在 `create_warp` 前填写 `LaunchInfo.dispatch`：`stage=Vertex/Fragment`、`varying_count=0..8`、逐 lane `inputs` 和纹理状态 `graphics`，并映射输入/代码资源。结构字段及人工例子见 `tools/graphics_demo.cpp`。调用 `run_shader_batch(machine, ids, trace)`，它只轮询所选图形 warp，直到均通过 gfx.end 返回 Completed；共享整台 Machine 的指令预算。输出位于每个 warp 的 `completed_outputs`，或由 `release_warp` 返回快照保存；释放后可复用槽位。没有自动清槽，不执行Framebuffer写回。`finish_test_warp` 仅供 Test 阶段，不能用来绕过图形 shader 的 gfx.end。

D 阶段的采样边界精度、end 后调试 PC 和宿主资源交接细节，单列在父目录改进日志的“D 阶段实践记录”；B/C 历史记录不改写。

### VS Code 标准库头文件提示

请用“文件 → 打开文件夹”打开本仓库根目录（包含 CMakeLists.txt 的目录）。若提示无法打开 string、array、span 等标准库头文件，执行 `C/C++: Edit Configurations (UI)`，设置 Compiler path 为本机 C++ 编译器路径，C++ standard 为 C++20，IntelliSense mode 为 windows-gcc-x64，Include path 加入 `${workspaceFolder}/include`。

本机已配置 `.vscode/c_cpp_properties.json`，编译器为 `D:/mingw-19.0/MinGW/bin/g++.exe`；该文件含本机路径，已加入忽略规则，同学需按自己的安装位置配置。标准库头文件由编译器提供，不应复制进仓库或逐个添加系统 include 路径。修改后可执行 `Developer: Reload Window` 刷新；若仍异常，使用 `C/C++: Log Diagnostics` 检查实际生效配置。

### 文件职责

文件按下面四类阅读；这是职责分类，不移动文件。**理解机器码如何执行，先读第一类；核对结果是否正确，重点读第三类。** 宿主启动/停止接口是模型运行支撑，不能当作 GPU 已支持的机器指令。

| 类别 | 通俗含义 | 与其他类别的关系 |
| --- | --- | --- |
| 1. 架构功能与指令执行 | 说明模拟 GPU 如何识别机器码、计算、访问寄存器/内存和改变状态 | 由第 2 类启动运行，结果交给第 3 类检查 |
| 2. 宿主装载、上下文与运行入口 | 在电脑上准备代码/输入、创建 warp、控制运行与回收 | 调用第 1 类；目前的轮询是宿主执行顺序，不是真实硬件调度性能模型 |
| 3. 辅助验证、诊断与案例 | 提供测试输入、独立参考答案、轨迹和错误定位，检查模型算得是否正确 | 通过第 2 类运行第 1 类，比较结果；不应替机器码完成 shader 计算 |
| 4. 构建与工程说明 | 将源码编译成程序，组织测试并说明使用方法 | 为前三类提供构建和执行入口 |

一次典型验证的关系是：**第 4 类编译 → 第 2 类装载并启动 → 第 1 类执行机器码 → 第 3 类核对结果**。实际测试程序也会调用宿主接口，这四类是主要职责分类，不表示函数彼此隔离。

#### 1. 架构功能与指令执行

| 文件 | 职责 |
| --- | --- |
| include/model/model.hpp | 公共类型、函数声明、状态与结果契约 |
| include/model/execution.hpp | 执行模块内部共用接口 |
| src/memory.cpp | 内存、权限、区域映射与取指 |
| src/decode.cpp | A–D 指令编码匹配、格式检查和立即数扩展 |
| src/engine.cpp | 分类分派、源值预检、集中提交和单步；同文件也提供宿主 stop_pc 运行接口 |
| src/exec_integer.cpp | ALU、M、分支与跳转 |
| src/exec_memory.cpp | guest 整数与浮点 load/store、对齐/冲突规则 |
| src/exec_simt.cpp | split/join 与只读 gpu_tmr |
| src/exec_fp.cpp | 每活动 lane 的浮点/SFU 执行与结果暂存 |
| src/fp_backend.cpp | SoftFloat 适配、舍入/异常；SFU 使用显式实验参考 profile |
| src/exec_csr.cpp | 浮点 CSR 六种操作、FENCE 和环境异常 |
| src/exec_graphics.cpp | gfx.get/tex/end、输出收集与精确 texel 定位；同文件也提供宿主派发数据预检 |
| third_party/softfloat/ | 标准浮点执行依赖，参与模型计算；附来源、校验值和许可证 |

SFU 的参考实现用于执行 guest 指令，归入本类；用于判断它结果是否正确的独立参考算法归入第三类。公共头文件还声明宿主接口，不能只凭文件所在类别判断某个函数是否属于 ISA。

#### 2. 宿主装载、上下文与运行入口

| 文件 | 职责 |
| --- | --- |
| src/common.cpp | 配置检查、模型创建、宿主 warp 启动/初态、测试结束与槽位回收 |
| src/loader.cpp | 文件检查与原始机器码装载 |
| src/scheduler.cpp | 宿主多 warp 轮询与各自停止条件；不模拟硬件调度延迟 |
| src/main.cpp | CLI 参数处理、创建单 warp、运行机器码和返回退出码 |

#### 3. 辅助验证、诊断与案例

| 文件 | 职责 |
| --- | --- |
| src/trace.cpp | 输出状态变化轨迹和错误位置，供人工或脚本核对 |
| tests/model_tests.cpp | A 阶段回归（已实现的 SUB/移位改为正确识别断言） |
| tests/stage_b_tests.cpp | B1–B7 功能、边界、非法输入与掩码组合 |
| tests/stage_c_tests.cpp | C1–C5 的 13 组测试及新增操作覆盖检查 |
| tests/stage_d_tests.cpp | D1–D3 的 14 组测试及图形操作覆盖检查 |
| tests/differential_integer.py | 独立整数数学参考算法 |
| tests/differential_float.py | 精确有理数浮点参考与 Decimal SFU 参考 |
| tests/differential_texture.py | 1024 组独立精确 UV/texel、packed 颜色及片元身份检查 |
| tools/graphics_demo.cpp | 人工 FS 验证工具，只装载上下文/纹理，让机器码完成计算与输出 |
| cases/*.bin | A/B/C/D 示例机器码，图形示例需配合人工派发工具 |
| cases/README.md | 示例机器字与预期结果说明 |

#### 4. 构建与工程说明

| 文件 | 职责 |
| --- | --- |
| CMakeLists.txt | C/C++ 目标、依赖和自动测试注册 |
| build.ps1 | 本机 GCC 构建与 A/B/C/D 测试执行入口 |
| .gitignore | 排除构建产物、临时文件与本机配置 |
| README.md | 本仓库的构建、运行和职责索引 |

`evaluate` 只生成 StepEffect；内存变化也先暂存，所有源值来自提交前状态。冲突实验中，memory_writes 的 before 统一表示指令执行前快照，after 按 lane 顺序提交；`commit_effect` 是内部提交边界，只能提交当前状态上成功求值得到的效果。一般调用者使用 `step_shader`，不要手工伪造 StepEffect。宿主初态通过 `initialize_x` 设置；未赋值的普通整数寄存器读取默认产生调试诊断。

内存辅助函数按字节工作，不额外要求数据对齐；guest LW/SW/FLW/FSW 的未对齐策略另行检查（S06 尚待确认）。装载映像创建只读/可执行区，区域不允许重叠，一次访问须位于一个区域内。未知指令不会静默跳过；A–D 范围内检查固定编码，范围外尚未完整译码的指令报告 ModelLimit，不能据此认定它是非法 ISA 程序。

完整设计和讨论日志目前位于本仓库的父目录，包括 [C阶段设计与验证](../C阶段设计与验证.md)、[D阶段设计与验证](../D阶段设计与验证.md)；README 保留了独立构建和运行所需说明。当前工作止于 D 阶段，后续按开发计划推进，不把物理硬件延迟加入逻辑步计数。
