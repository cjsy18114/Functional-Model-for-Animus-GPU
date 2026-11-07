# A–D 阶段机器码案例

`addi_add.bin` 是小端机器码，不是文本汇编。包含两条独立核对的指令：

| 偏移 | 机器字 | 汇编 | 文件字节 |
| --- | --- | --- | --- |
| 0 | 0x00500093 | addi x1, x0, 5 | 93 00 50 00 |
| 4 | 0x00108133 | add x2, x1, x1 | 33 81 10 00 |

默认 32 个 lane 均活动。两步后每个 lane 的 x1=5、x2=10、x0=0，warp PC=8；仅使用 warp 槽位 0。另三个槽位存在但不运行。

停止由 CLI 参数 `2` 控制，这是宿主暂停条件；不是 shader 已执行 gfx.end，也不是 GPU 用了两个硬件时钟周期。

## B 阶段循环示例

`countdown.bin`：0x00300093（addi x1,x0,3）、0xFFF08093（addi x1,x1,-1）、0xFE009EE3（bne x1,x0,-4）。

使用 `animus_model_cli cases/countdown.bin --stop-pc 12` 运行。32 个 lane 的判断一致，循环减至 x1=0，在 PC=12 暂停；共执行 7 条 warp 指令。stop_pc 是宿主断点地址，可以等于代码末尾，不要求在那里再放一条退出指令。

## C 阶段浮点与 SFU 示例

`fp_arithmetic.bin`：每 lane 计算 1.5+2.25=3.75，再把结果位模式放入 x3。

| 偏移 | 机器字 | 操作 |
| --- | --- | --- |
| 0 | 0x3FC000B7 | lui x1,0x3fc00 |
| 4 | 0xF00080D3 | fmv.w.x f1,x1 |
| 8 | 0x40100137 | lui x2,0x40100 |
| 12 | 0xF0010153 | fmv.w.x f2,x2 |
| 16 | 0x002081D3 | fadd.s f3,f1,f2,rne |
| 20 | 0xE00181D3 | fmv.x.w x3,f3 |

`sfu_rsqrt.bin`：0x408000B7（lui x1，设置 4.0 位模式）、0xF00080D3（fmv.w.x f1,x1）、0x4200818B（gpu.rsqrt.s f3,f1）、0xE00181D3（fmv.x.w x3,f3），预期 x3=0x3F000000，即 0.5。

分别以 `animus_model_cli cases/fp_arithmetic.bin 6 --experimental-numeric` 和 `animus_model_cli cases/sfu_rsqrt.bin 4 --experimental-numeric` 运行。该选项显式开启待确认的逐 lane 浮点状态与 HostMathV1 SFU 参考 profile；省略时分别在 FADD 或 SFU 指令处报 ModelLimit。示例只验证功能，不体现物理 SFU 数量和性能。

## D 阶段：人工 FS 纹理原色输出

`fs_texture.bin` 必须配合 `animus_graphics_demo` 的人工 FS 上下文运行，不直接交给默认 Test 阶段的 CLI。

| 偏移 | 机器字 | 操作 |
| --- | --- | --- |
| 0 | 0x010001AB | gfx.get x3,PIXEL_X（人工输入中为 lane 索引） |
| 4 | 0x00319193 | slli x3,x3,3（每对 UV 占 8 字节） |
| 8 | 0x0020022B | gfx.get x4,UNIFORM_BASE |
| 12 | 0x00320233 | add x4,x4,x3 |
| 16 | 0x00022083 | lw x1,0(x4)，读取 u 的 FP32 位模式 |
| 20 | 0x00422103 | lw x2,4(x4)，读取 v 的 FP32 位模式 |
| 24 | 0x0020A52B | gfx.tex x10,x1,x2 |
| 28 | 0x0000302B | gfx.end，复制 x10 与原片元身份 |

运行 `animus_graphics_demo cases/fs_texture.bin`，默认 4 个 lane 的 UV 为 (0,0)、(1,0)、(0,1)、(1,1)。绑定 3×2 纹理，stride=16：第一行红/绿/蓝，第二行白/黑/品红，每行尾部 4 字节 padding。8 步后正常 Ended，颜色依次为 0xFF0000FF、0xFFFF0000、0xFFFFFFFF、0xFFFF00FF。输出仅是片元记录，没有 framebuffer 写入。

可额外指定 UV 文件：`animus_graphics_demo cases/fs_texture.bin build/texture_oracle_uv.bin`；文件包含 1–32 对小端 FP32 u/v 位模式。独立参考脚本会生成并传入该文件。不需要数值实验选项。
