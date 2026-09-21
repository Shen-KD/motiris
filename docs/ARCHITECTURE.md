# motiris 架构总览与实现路径

> 2026-09-21。覆盖:本轮已落地的 REPL/会话回顾/Skill 编辑改造,以及
> "skill 级 schema 声明"两个候选方案(方案 B/C)的实现路径与取舍。

## 1. 总览

motiris = 一个 ~150KB 的 C11 二进制,零运行时依赖(libcurl.so.4 除外)。
本次改造后,系统由四层组成:

- 入口层:CLI(`main.c`)、REPL(`repl.c`)、HTTP 网关(`gateway.c`)、定时任务(`cron.c`)
- 核心层:agent 循环(`agent.c`)、配置(`config.c`)、传输(`transport.c`)、
  会话日志(`sessions.c`,新增)
- 工具层:注册表(最多 128 个工具),内置/文件/网络/记忆/技能/子代理/
  浏览器/MCP/插件
- 存储层:`~/.local/share/motiris/` 下的 sessions/、gateway/、cron/、
  memory.json、plugins/,以及用户指定的 skill 目录

新增能力(本轮):REPL 启动横幅显示模型/SKILL/TOOLS、自动会话日志、
`/sessions` `/resume` `/skills` 命令、模型可编辑技能的 skill_patch/
skill_write 工具。

## 2. 架构图

```
                     +-----------------------------------------------+
                     |              motiris (~150KB 单二进制)          |
                     +-----------------------------------------------+
  入口        +-------+--------+       +---------+      +-----------+
              | CLI (main.c)   |       | gateway |      | cron      |
              | -p/--sessions  |       | (HTTP)  |      | (定时)    |
              +-------+--------+       +----+----+      +-----+-----+
                      |                      |                 |
                      v                      v                 v
  交互        +----------------+      +-----+-----------+    |
              | REPL (repl.c)  |      | sessions.c      |<---+
              | banner 信息     |----->| (共享 U/A/T 日志)|
              | /sessions      | 复   +-----------------+
              | /resume        | 用
              | /skills        |
              | /tools         |
              +-------+--------+
                      |
                      v
  核心        +-----------------------------------------------+
              | agent.c 循环: model -> tool_calls -> execute  |
              |  请求装配(cJSON)  · 多 provider 故障转移      |
              +-------+-------------------+-------------------+
                      |                   |
                      v                   v
              +---------------+   +----------------------------+
              | transport.c   |   | tools 注册表 (Max 128)     |
              | libcurl/spawn |   | shell/time  · file/web    |
              | curl/echo     |   | memory · skill_list/load  |
              +-------+-------+   | skill_patch/write · sub   |
                      |           | agent · browser · MCP     |
                      v           | <server>:<tool> · 插件.so |
              +----------------+  +-------+-------------------+
              | LLM API       |          |
              | (OpenAI 兼容)  |          v
              +----------------+  +---------------------------+
                                  | 存储层 ~/.local/share/     |
                                  | motiris/                   |
                                  | sessions/ gateway/ cron/  |
                                  | memory.json plugins/       |
                                  +---------------------------+
```

## 3. 核心数据流

**Agent 循环**(`agent.c`):装配请求(系统提示 + 历史 + tools 数组)→
按 provider 顺序尝试 → libcurl 发送 → 解析响应 → 有 tool_calls 就执行
工具、把结果作为 tool 消息追加 → 回到循环;finish_reason=stop 输出最终
答案。token 计数从每次响应的 usage 累加。

**会话日志(U/A/T 行)**:所有入口统一走 `sessions.c`。
- REPL(tty 下):每轮写 `U<用户行>`,`T<工具名>(<参数摘要>) -> <结果摘要>`
  (工具钩子),回合结束写 `A<完整回复>`
- 网关:`gateway/<chat_id>.log`,启动时按 U 行重放历史
- `--sessions` / `/sessions [TERM]`:`motiris_session_list("gateway"|"sessions", term)`
- `/resume FILE`:读取日志的 U 行注入当前上下文

**Skill 编辑**:skilltools.c 启动时扫描 `--skill-dir` 的 *.md,解析
frontmatter(name/description)建立索引(含文件名);skill_patch /
skill_write 按索引构造路径(`skill_dir` + `file`,不经用户输入,天然
限制在技能目录内)读写文件,并同步内存索引;skill_load 返回最新正文。

## 4. 状态目录

```
~/.local/share/motiris/
├── sessions/        repl 会话日志 repl-YYYYMMDD-HHMMSS-<pid>.log
├── gateway/         <chat_id>.log(网关会话,可被 /resume 复用)
├── cron/            <job_id>.log
├── memory.json      持久记忆
└── plugins/         插件 .so
```

配置目录:`~/.motiris/`(env、config.json,受 MOTIRIS_HOME 覆盖);
状态目录一律走 HOME(与网关/既有逻辑一致)。

## 5. 方案 A(已落地):REPL 增强 + 会话回顾 + Skill 编辑

实现路径(commit 顺序,均在本地 main 上):

| # | 改动 | 涉及文件 | commit |
|---|------|---------|--------|
| A1 | banner 分隔线/对齐/配色;状态脚注(耗时+model);首启提示 | src/harness/repl.c | b3cb537 |
| B1 | 共享会话模块 sessions.c;--sessions 改走共享层 | src/harness/sessions.c(新), motiris.h, main.c | 67c1f10 |
| B2/B3 | REPL 自动会话日志(tty);/sessions /resume 命令 | repl.c | 89d0d81 |
| C1-C3 | skill 索引 file 字段 + desc 访问器;skill_patch/skill_write; /skills | skilltools.c, repl.c, motiris.h | 9606819 |
| D1 | smoke #19-#23(skills/sessions/resume + 两个 mock 工具用例) | tests/smoke.sh | 132c45a |
| D2 | README/CHANGELOG/DEVELOPING 同步(23 checks) | *.md | a4764a8 |
| D3 | 访问器实现补提交 + .hermes 忽略 | agent.c, .gitignore | 9e43675 |

验证:make test 23 项通过;make test-docker(干净容器,CI 同款)23 项
通过;二进制 149752B < 163840B 上限。

## 6. 方案 B(新方向,待做):每个 tool 独立 schema 文件

> 决策(2026-09-21):否决"skill 级 schema"(config.json 集中声明 /
> frontmatter + skill_run),改为工具/插件维度:每个 tool 的参数 schema
> 独立成文件,与 C 代码解耦,注册时可被外部文件覆盖。

**目标**:schema 调整不重编译;插件可随 .so 外部分发 schema 文件
(不改 MotirisPluginApi ABI);内置工具与插件工具统一走同一机制。

**设计形态**(tools 与 plugins 统一按此目录规范):

```
<plugin_dir>/                       # MOTIRIS_PLUGIN_DIR(默认 ~/.motiris/plugins)
  hello/
    hello.so                        # 链接库:动态库(运行时 dlopen,导出 motiris_plugin_init)
    hello.a                         # 静态库(可选:编译期集成用,不参与运行时加载)
    hello.h                         # 头文件(工具/扩展 API 声明,供宿主对接)
    hello.json                      # 该工具的参数 schema(JSON Schema)
  skillhub/
    skillhub.so
    skillhub.h
    skillhub.json
```

- 内置工具的同类覆盖目录:`<tools_dir>/<tool_name>/`(MOTIRIS_TOOLS_DIR,
  默认 ~/.motiris/tools)—— 放同名 json 即覆盖内嵌 schema;
  放 .so 可注册新工具,内置/外部统一语义。
- 工具名 = 目录名;目录下文件同名前缀(可选,json 也可叫 schema.json,
  见实现时定)。
- 一个"工具"= 一个目录:库(运行时能力)+ 头(编译期契约)+ json(模型
  可见的参数契约)。

**语义**:注册工具时按 name 查找外部 schema;找到 → 覆盖
MotirisTool.parameters(加载器持有副本,注册时拷贝进 agent 存储);
未找到 → 使用 C 内嵌默认 schema(零破坏,渐进式迁移)。

**实现路径**(任务粒度):

1. **工具目录扫描器**(新建 src/tools/toolscan.c):启动时扫描
   `tools_dir/*/` 与 `plugin_dir/*/` 子目录,读取 `<name>/<name>.json`
   → cJSON_Parse 校验合法 → 存入 name→schema 注册表;非法 json
   打印警告并跳过该文件的 schema(不影响工具注册)。访问器:
   `motiris_set_tools_dir()` / `const char *motiris_tool_schema(const char *name)`。
2. **注册合并钩子**:motiris_register_tool 末尾增加"查外部 schema,
   有则替换 parameters"(注意生命周期:外部字符串 strdup 进 agent
   自己的存储,寄存器本身 shallow copy 语义不变)。
3. **插件按目录加载**:plugin.c 由"扫单层 *.so"改为逐子目录:
   对每个 `<plugin_dir>/<name>/` 目录 dlopen `<name>.so`(导出
   motiris_plugin_init 则加载),并读取同名 `<name>.json` 注册 schema;
   `.a`/`.h` 不参与运行时加载(供编译期集成与文档)。ABI 不变,
   老插件目录形态(扁平的 .so)保留兼容:缺目录时仍按旧方式加载。
4. **渐进迁移**:仓库分发示范 `tools/shell/shell.json`(与内嵌一致),
   README 说明"想改 schema 就建目录放 json;想加工具就放 .so"。
5. **文档与测试**:smoke 新增:建 shell 工具目录(json 覆盖)→ mock
   模型断言请求体 tools[].function.parameters 变为文件内容;非法
   json 跳过不影响注册;插件子目录 .so + json 生效用例(沿用
   hello_plugin.so)。README/DEVELOPING 同步。

**收益**:schema 即文件,可 diff 可审查可热改;插件分发 schema 不需
改 ABI;与 skill 解耦(方案 C 已否决,skill 仍是纯文本目录)。

**代价**:多一层目录查找;同名文件的"覆盖"语义需文档化;二进制略增。

## 7. 对比:内嵌硬编码 vs 每工具 schema 文件

| 维度 | 内嵌硬编码(现状) | 每工具 schema 文件(方案 B) |
|------|------------------|------------------------------|
| 改 schema | 改 C 源码重编译 | 改文件生效(下次装配) |
| 插件分发 | schema 只能随 .so 代码 | 可外带 schemas/ 目录,ABI 不变 |
| 一致性 | 编译期保证 | 运行时 cJSON 校验,非法跳过 |
| 审查 | 混在注册代码里 | 独立文件,可 diff/可 review |
| 复杂度 | 无 | 加载器 + 注册合并钩子 |
| 二进制 | 最小 | 略增(加载器 + 注册表) |

## 8. 实现状态与顺序

✅ 已落地(commit d2011f5 + 后续):
1. 工具目录扫描器(toolscan.c):`<name>/<name>.json` 解析 + 注册表 +
   `motiris_tool_scan(plugin_dir)` / `motiris_tool_schema()` 访问器
2. `motiris_register_tool` 合并钩子(外部 schema 覆盖 parameters)
3. 插件按目录加载:plugin.c 递归子目录(dlopen `<name>.so` + 读
   `<name>.json`),保留旧扁平 .so 兼容
4. 仓库分发示范 `examples/shell/shell.json` + README 说明
5. smoke #24(schema 覆盖)/ #25(非法 json 跳过)/ #26(插件子目录加载)

⚠ 待做:
- 内置工具默认目录也默认是 `~/.motiris/tools`(env MOTIRIS_TOOLS_DIR),
  当前只扫描不创建,文档已声明
- 全量 smoke 26 项已过;若后续加工具,记得补对应 schema 文件用例

## 9. 下一步建议(按价值排序)

1. 方案 B(schema 文件化)落地,约 1 天工作量,从上面的顺序开工
2. `/jobs` 命令:cron 日志接入 motiris_session_list("cron", term),
   一行命令
3. 会话日志轮转/清理策略(目前每会话一文件,不删)
4. 网关会话目录统一读 MOTIRIS_HOME(现状:config 读 MOTIRIS_HOME,
   状态目录读 HOME,属历史遗留不一致)

