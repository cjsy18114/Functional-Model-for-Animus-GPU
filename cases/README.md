# A 阶段机器码案例

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
