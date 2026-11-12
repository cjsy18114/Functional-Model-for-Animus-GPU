# Warp管理指令：设计与验证

实现日期：2026-11-12。范围：单core、4个warp槽、默认32 lane；新增现有ISA的`gpu.wspawn`、`gpu.tmc`、`gpu.bar`，不修改指令编码或ISA原文。不是完整计算派发ABI，也不包含函数栈、spill空间自动分配或Vulkan API。

## 参考与适配

参数和主要行为参考Vortex提交`48fc0069d24899fab280b8a2e388955e4296d46c`，固定提交便于复查：

- [wctl_unit.cpp](https://github.com/vortexgpgpu/vortex/blob/48fc0069d24899fab280b8a2e388955e4296d46c/sim/simx/wctl_unit.cpp)：从最高编号的活动lane读取标量参数；TMC的掩码和零掩码退出。
- [scheduler.cpp](https://github.com/vortexgpgpu/vortex/blob/48fc0069d24899fab280b8a2e388955e4296d46c/sim/simx/scheduler.cpp)：WSPAWN限制数量、等待旧warp退出、新warp从lane 0启动。
- [barrier_unit.cpp](https://github.com/vortexgpgpu/vortex/blob/48fc0069d24899fab280b8a2e388955e4296d46c/sim/simx/barrier_unit.cpp)：阻塞式局部屏障按warp计数，到齐释放并进入下一轮。
- [types.h](https://github.com/vortexgpgpu/vortex/blob/48fc0069d24899fab280b8a2e388955e4296d46c/sim/simx/types.h)：屏障标识`group | (barrier_number << 8)`，bit31为全局标志。
- [VX_scheduler.sv](https://github.com/vortexgpgpu/vortex/blob/48fc0069d24899fab280b8a2e388955e4296d46c/hw/rtl/core/VX_scheduler.sv)：启动时保留调用者；模型也支持非零槽调用者，不局限于simx常见的warp 0启动路径。

Vortex的指令编码、split/join、系统CSR、参数传递和图形ABI不直接移植。Animus现有编码继续使用funct7=0/1/4、funct3=0、rd=0、opcode=0x0B；TMC的rs2必须为0。

本轮用户已授权参考Vortex实现三条指令。以下是模型运行约定；正式RTL仍需核对容量、寄存器初态和内存范围，不能仅凭模型通过便宣称硬件语义已全部定稿。

## 三条指令的通俗含义

三条指令的标量参数均取**当前最高编号活动lane**的对应整数寄存器。例如EXEC=0b0101时取lane 2，不读lane 0的参数，也不要求其他lane的参数已初始化。这与普通逐lane算术不同。

| 指令 | 参数 | 做什么 |
| --- | --- | --- |
| `gpu.wspawn rs1, rs2` | rs1为数量范围N，rs2为入口字节地址 | N限制在0..4，启动编号小于N且不是调用者的槽位；调用者继续原PC+4。常见warp 0调用N=4时，新启动warp 1、2、3，而不是另启动4个。 |
| `gpu.tmc rs1` | rs1的低W位为活动掩码 | 每个bit决定对应lane是否执行后续指令；不是线程数量。掩码为0则结束管理warp，不再取指。 |
| `gpu.bar rs1, rs2` | rs1为屏障标识，rs2为参加数量 | 每个warp贡献一次到达；数量以warp为单位，不是lane数，也不是数量减一。未到齐等待，到齐一起继续。 |

### WSPAWN的等待与初始化

若还有其他Ready/Waiting warp，调用者先在PC+4等待；等它们结束后再启动新一轮。`wspawn 1`由warp 0调用时可等待其他旧warp结束而不新启动warp；N=0不表示结束调用者。非零调用者按“启动[0,N)内除自身以外的槽”解释，因此N不一定等于最终活动warp总数。

新warp：

- PC为rs2，EXEC只开lane 0，SIMT分支栈为空。
- 管理上下文的INIT_MASK为配置宽度的全掩码，表示允许启用的lane集合；启动代码可用TMC扩展到全宽。管理代码没有VS/FS invocation，不能将这个管理包络解释成重写图形invocation的INIT_MASK。
- x0为0；其他整数和浮点寄存器未定义，fcsr初值0；不复制父warp寄存器，不自动提供参数、栈或线程编号CSR。
- 与父warp共享内存，继承允许执行的代码区间，获得新的invocation_id。未对齐入口立即报错，代码范围/权限在实际取指时检查。
- 已结束的Test管理槽可重新初始化并复用；故障槽和带图形输出的槽必须先由宿主收集并释放，不能覆盖。旧管理寄存器快照需要保留时，宿主应在后续WSPAWN之前复制。

模型禁止在延迟WSPAWN期间用宿主create_warp注入任务，或用finish_test_warp结束旧参与者；应继续调度旧管理warp，让它们执行TMC零退出。这样启动只发生一次，不靠反复执行WSPAWN轮询。

### TMC的合法范围

继续执行原ISA限制：只供没有在途shader的管理代码使用；VS/FS中禁止，即使写零也不允许；SIMT栈SP必须为0。模型采用现有`ShaderStage::Test`宿主上下文承载管理代码，不能在BUSY绘制提交或尚未释放的VS/FS上下文旁边执行TMC/WSPAWN。

TMC忽略W位之外的高位，不能启用宿主INIT_MASK包络外的lane。新spawn子warp包络为全宽；人工创建的窄包络管理warp若要扩展，宿主应一开始就提供足够的INIT_MASK。新启用lane的普通寄存器不会被自动广播或初始化。

### BAR的范围与重复使用

rs1低8位为本地group标识，bits[30:8]为组内屏障编号。例：0为group 0的barrier 0；`0x100`为group 0的barrier 1；1为group 1的barrier 0。程序自行保证同一批参与者使用同一标识，当前未提供自动工作组派发。

模型容量为group 0..3，每组barrier 0..7；超出报告ModelLimit，不截断或偷偷共用另一个屏障。bit31请求跨core全局屏障时报告ModelLimit，单core模型不伪造其他core已经到达。

每轮rs2必须为1..4且各参与者一致；首个到达者记录数量。BAR仅允许SP=0且EXEC=INIT_MASK。到达时该条指令已退休、PC+4，warp变Waiting；最后一个到达时释放所有参与者、清空本轮计数并增加generation。下一轮重新计数，不重放上一轮BAR。count=1立即继续。

同一共享数组中的先前写入都已经提交，因此本地参与者经过屏障后能够读取其他参与者先前写入的数据。不模拟缓存、排空时钟、跨core或CPU缓存一致性；这套较强的顺序参考行为不能证明未来弱内存硬件已经正确。join仍不是内存屏障。

数量无效、同轮数量不一致等报告带名称的错误；等待者不够时，运行器报告SuspectedDeadlock并保留现场，不能假装完成。不会自动把不参加的warp凑入屏障；程序和宿主负责隔离不同任务的标识与参与者。

## 模块与运行接口

- `src/exec_management.cpp`：三条指令的检查、纯求值、延迟启动、屏障轮次与唤醒。
- `model.hpp`：等待原因、屏障和待启动状态、管理指令副作用及轨迹事件。
- `engine.cpp`：只检查被选作标量源的lane；统一提交跨warp变化。求值失败不产生部分到达或部分启动。
- `scheduler.cpp::run_management`：动态轮询所有Test管理warp，包括执行过程中spawn的新warp；全部TMC退出才Completed。预算不足保留状态，提高max_steps后再次调用可继续。
- `trace.cpp`：JSONL的`warp_events`记录受影响的warp身份、PC、掩码、ready/waiting/ended状态和等待原因。槽位复用后以新invocation_id区分。
- `main.cpp`：新增`animus_model_cli code.bin --run`；原来的指定条数/stop_pc仍为选定warp的调试入口，不自动运行所有新warp。单warp入口遇到等待会暂停；完整管理程序请用--run。

run_management不用于图形提交。VS/FS若使用合法的局部BAR，应由现有图形批次运行器调度参与者；E当前串行批次没有自动提供额外屏障参与者，count>实际参与者不会自动满足。

## 验证与运行步骤

在仓库根目录运行一次完整构建，自动执行A–E原63组测试和管理指令13组测试：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Compiler 'D:\mingw-19.0\MinGW\bin\g++.exe'
```

新增测试末行应为：

```text
PASS management: 13 test groups; instruction coverage 3/3
```

13组覆盖：编码及保留字段、TMC选取最高活动lane/精确掩码/退出、VS/FS及分支保护、启动初态与求值无副作用、N=0/1/2/4/超容量、非零调用者、延迟启动及槽位复用、代码范围、重复屏障、独立标识、错误原子性、33种到达顺序各两轮、缺参与者的暂停恢复，以及真实生产者/消费者机器码。

生产者/消费者测试的预期由程序直接推出：父warp单lane将地址1024写成100，4个warp在同一BAR集合，3个子warp各32 lane随后读取该地址，均应得到100；总共父14条、每个子8条，即38条成功指令。测试在第12条预算暂停后继续，检查总数不增加、没有重放副作用。

只复验新增C++测试：

```powershell
$env:PATH = 'D:\mingw-19.0\MinGW\bin;' + $env:PATH
.\build\animus_management_tests.exe
```

Python可选，用固定机器字从CLI外部再验证启动/退出和JSONL：

```powershell
python .\tests\verify_management.py
```

预期`PASS management CLI: 12 instructions, 3 fresh children, lane-0 writes and 4 TMC exits; JSONL parsed`。脚本生成`build/management.bin`和`build/management_trace.jsonl`；父6条、每个子2条，所以总数12，三个子warp只有lane 0写x3=7，最后四个warp都通过TMC零退出。这一检查不依赖C++测试的机器码构造函数。

生成后也可直接运行：

```powershell
.\build\animus_model_cli.exe .\build\management.bin --run
```

以上验证不能关闭栈/spill ABI、通用计算工作组接口、SFU误差和完整Vulkan能力等其他待办。
