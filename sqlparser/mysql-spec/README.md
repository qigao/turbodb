# MySQL 8.4 SQL 语料

当前收录 **385 条独立 SQL 输入**，从 MySQL 官方 11 个 `.test` 和 3 个 `.inc`
文件中按已审阅的行范围提取。每个 `.sql` 对应一条语句，保留原始字节、空白、
引号和末尾分号；不保证离开上游 schema / 会话后可以独立执行。

来源固定为 [mysql-8.4.0](https://github.com/mysql/mysql-server/tree/mysql-8.4.0)，
commit 为 `dc86e412f18b36ce271f791026714e8caa0ec919`。
这是固定的 8.4.0 语法测试样本，不代表最新版 8.4，也不是官方完整测试套件。

## 来源、提取与许可

- `upstream/mysql-test/`：所选官方来源文件的完整、未修改副本。
- `upstream/LICENSE`、`upstream/README`：上游许可和版权原文。
- `cases/<主题>/<上游起始行>.sql`：只复制所选语句，不做变量替换或 SQL 改写。
- `manifest.json`：版本、commit、来源 SHA-256、语句行范围/字节范围、官方预期、
  会话模式说明，以及本地解析器基线和已知缺口。它是提取范围和分类的事实源。

上游材料及其 SQL 片段保留 GPLv2 和上游附加许可，见
[根目录第三方说明](../../THIRD_PARTY_NOTICES.md)及 [LICENSE](upstream/LICENSE)。
语料仅被测试读取，不编译、链接、安装进解析库；没有增加 MySQL 服务端或客户端库依赖。

按 [mysqltest 官方语言说明](https://dev.mysql.com/doc/dev/mysql-server/latest/PAGE_MYSQLTEST_LANGUAGE_REFERENCE.html)，
`.test` 混合 SQL、测试指令、变量和流程控制。因此本次只提取明确选定的完整 SQL 行范围，
不自动扫描全部来源，也不把 `connection`、`source`、`eval`、`let`、条件块或循环当 SQL。
不展开 include；直接从所需 `.inc` 文件选取确定语句。动态拼接、禁用分支、存储程序自定义
分隔符等未纳入本轮。原始文件保留，未选中部分不计入通过率或覆盖率。

`sql_mode` 记录上游语句所在场景：`empty`、`NO_ENGINE_SUBSTITUTION` 或 `default`。
本轮未收录依赖 `ANSI_QUOTES`、`IGNORE_SPACE`、`PIPES_AS_CONCAT` 等不同词法模式的语句。
测试逐条调用固定 MySQL 方言解析入口，不执行 SET，也不模拟会话状态。

## 预期与结果分类

官方执行预期和本地解析器行为分开保存：

| manifest 字段 | 含义 |
| --- | --- |
| `mysql_expectation=success` | 上游选定场景预期执行成功，共 328 条 |
| `mysql_expectation=syntax_error` | 上游明确标注 ER_PARSE_ERROR / 1064，共 20 条 |
| `mysql_expectation=server_error` | 上游标注名称解析、约束或其他执行错误，共 37 条；不能据此要求语法拒绝 |
| `mysql_error` | 保留上游原始错误名称/编号，成功时为空 |
| `parser_expectation` | 当前经审阅的 accept / reject 回归基线，不覆盖官方预期 |
| `gap` | 与官方语法接受/拒绝不同的具体原因；正常匹配时为空 |

测试先核对 SQL 与来源字节、行号，再检查 AST 切片和语句链、失败原子性、错误位置，
最后比较当前行为与显式基线。所有用例均进入 TSV，即使发现行为变化也先输出完整报告。
新增、修复或回退造成的基线差异会失败，维护者须先审查原因再更新 manifest，不能用
当前解析器输出自动刷新预期。SQL 片段自身不携带、也不执行 mysqltest 指令。

**事实：本轮结果**为接受 365、拒绝 20。官方语法应接受的输入有 365 条，全部
接受、0 条拒绝；官方预期语法错误的 20 条全部拒绝，误接收为 0。
因此回归目标通过不等于 385 条全部兼容，更不证明 AST 含义或数据库执行语义等价。

**已修复 HIGH：原有 10 条误接收。** BIT_AND、BIT_OR、BIT_XOR、COUNT、CURDATE、CURTIME、
DATE_ADD、DATE_SUB、EXTRACT、GROUP_CONCAT 紧接左括号作未限定表名时现在拒绝。
空格、注释、引用和限定名称的正反例，以及函数调用 AST，另由 `dialect_test.c` 验证。
CAST 作普通标识符的两条合法语料现在接受。规则依据为
[官方函数名解析说明](https://dev.mysql.com/doc/refman/8.4/en/function-resolution.html)
及本地未修改的 `upstream/mysql-test/t/parser.test`。

**已修复 HIGH：SQL_CALC_FOUND_ROWS 静默错析。** 该修饰符现在由 SELECT 的
calc_found_rows 标志保存，列及别名不会错位；原先明确拒绝的两条语料现已接受。
正式测试覆盖标志、列数、DISTINCT、括号查询、引用/限定名称和 SQLite 同名标识符。

已支持 CREATE TABLE … [AS] SELECT、基础 EXPLAIN / DESCRIBE / DESC、单表 ANALYZE TABLE
和基础 CREATE / DROP VIEW，消除语料中的 8 条缺口。正式 `dialect_test.c` 同时检查 AST
字段、子句归属、非法组合和 SQLite 隔离；没有改变原始 SQL 或官方预期。
EXPLAIN FORMAT 及视图 CHECK OPTION 也已在本轮扩展中覆盖。

数字开头名称及其限定引用现在通过 `cases/create/0078.sql` 至 `0081.sql` 的全部 4 条语料。
`dialect_test.c` 另覆盖指数数字与别名边界、限定关键字、MySQL 二进制/十六进制字面量，
以及截断输入的错误位置和 AST 切片界限；这些不额外计入 385 条官方语料数量。

括号查询新增 QUERY_GROUP AST 节点，保留各层 ORDER BY / LIMIT；13 条原括号查询缺口
中的 12 条先前已接受；本轮支持 SQL_CALC_FOUND_ROWS 后，第 13 条也已接受。
正式测试覆盖嵌套查询、标量与 IN 查询区别、UNION 子句归属、写入/建表查询、资源边界和
分配失败清理。公开 AST 新节点的消费方适配要求见 [模块说明](../README.md#接口与-ast)。

DEFAULT 写入值和 SET 系统变量默认值消除了 7 条缺口：insert/0051、0054、0055、0058，
replace/0033、0036，show_variables/0052。裸 DEFAULT 使用独立 DEFAULT_VALUE 节点，
DEFAULT(列名) 使用 CALL；正式测试另覆盖 UPDATE、REPLACE SET、字符集默认值、作用域、
不合法的 DEFAULT 表达式、用户变量拒绝、SQLite 隔离及资源失败。

本轮按缺口条数优先实现以下组别，原来的 45 条缺口全部转为接受：

| 类别 | 语料条数 |
| --- | ---: |
| SHOW（含 CREATE TABLE） | 10 |
| 事务隔离、CHAIN、读写模式 | 8 |
| 基础 DDL、ZEROFILL、ON UPDATE | 6 |
| SQL 预处理语句 | 5 |
| XOR、用户变量赋值表达式 | 4 |
| LOW_PRIORITY、多表 DELETE | 4 |
| LOCK / UNLOCK TABLES | 2 |
| SQL_CALC_FOUND_ROWS | 2 |
| 基础窗口调用 | 2 |
| 视图 CHECK OPTION | 1 |
| EXPLAIN FORMAT | 1 |

每组先检查接受变化及 AST / 非法组合测试，再更新对应的本地预期；原始 SQL、来源字节、
上游预期和错误标注均未修改。37 条官方服务端错误语料仍作为语法合法输入接受，
不把缺表、类型、递归执行限制等服务端错误误当成语法错误。

**MED：覆盖范围。** 当前 385 条语料的 missing_syntax / over_accept 均为 0，
只证明这些输入的接受/拒绝匹配，不等于完整 MySQL 兼容。窗口帧、WINDOW 声明、
其他 ALTER 动作、存储程序、SQL 模式等仍未覆盖，详见[模块支持范围](../README.md)。

## 运行与维护

在 VS 开发者命令行中使用仓库 presets：

```text
cmake --build --preset win-release-user --target sqlparser_mysql_corpus_test
ctest --preset win-release-user -R "^sqlparser_mysql_corpus_test$" --output-on-failure
```

报告为 `<build>/sqlparser/tests/mysql-spec-results.tsv`，逐条包含官方预期/错误、模式、
本地基线、实际结果、`matches_syntax` / `missing_syntax` / `over_accept` 分类、来源行号、
解析错误位置和缺口原因。它与用 MySQL 模式解析 SQLite 语料的 `corpus-results.tsv` 独立。
本轮没有运行 MySQL 服务端，官方预期来自固定来源中的测试及错误标注。

需要重建原文字节片段时使用 Python 3.9+：

```text
python sqlparser/tests/rebuild_mysql_corpus.py
```

该维护工具校验全部来源 SHA-256、范围和输出路径后重新生成 manifest 列出的 SQL 文件，
不下载、不解释 mysqltest、不更新预期、不运行测试。`.gitattributes` 禁止 Git 自动转换
上游及片段的换行，避免破坏哈希与偏移。添加语句时先审阅上游上下文和错误分类，补齐
manifest 行/字节范围，再重建片段并运行正式测试。生成的 C 清单只写入 build tree。
