# 构建与性能排障记录

## Redis / MySQL 协议扫描

- 短 RESP 回复和密集参数 SQL 不能为每个片段都进入外部向量函数。
  本次基准确认短片段的调用成本会抵消收益；短片段先就地扫描，长片段再批量处理。
  采用 `msvc-benchmark-user` 的正式 CTest 基准验证，具体数据和命令见 README。
- SaltsBase 0.1.5 的 `simd_reduce(..., SALTS_SIMD_REDUCE_BITMASK, ...)`
  使用数值 lane 描述符（此处 `simd_u8x16`），不能传 `simd_b8x16`；后者返回失败。
  失败直接向上传播，不切换备用实现。
- `tstr_new_len(NULL, n)` 会初始化存储。Redis 命令用 `tstr_new` +
  `tstr_reserve`，直接写入容量后再 `tstr_set_len_checked`，避免额外初始化大 payload。
- SQL 参数索引缩短后，后续输入和输出可能重叠。批量输出使用 `memmove`，
  保留原地改写；正式测试覆盖先删除索引、再复制长字符串与注释的场景。
- Windows 测量中出现过单批次数十毫秒的调度停顿，固定 CPU 也可能放大干扰。
  本次结果采用默认调度、三轮交替基线与优化版，每轮 200 × 1,024 次操作；
  原始日志与汇总保留在 `build/performance-{baseline,optimized}-*.log`
  和 `build/performance-comparison.json`。不把协议 CPU 基准写成数据库端到端吞吐。
