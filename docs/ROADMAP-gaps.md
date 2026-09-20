# motiris 功能缺口盘点(对照 Hermes Agent 功能面)

现状:motiris 已有 agent loop(工具调用)、工具注册/运行时插拔、dlopen 插件、
REPL 交互、SSE 流式、会话 log/resume、配置中心(~/.motiris)、CI/分支保护。

## 对照表

| # | Hermes 功能面 | motiris 现状 | 缺口 | 优先级 |
|---|--------------|-------------|------|--------|
| 1 | Provider/多模型 fallback | 单 OpenAI 兼容 provider | 多供应商+fallback+重试 | P0 |
| 2 | Gateway 消息平台 | 无(CLI only) | 平台接入+会话路由 | P0 |
| 3 | 文件工具集 | 仅 shell/time | read/write/patch/search | P0 |
| 4 | Web 搜索 | 无 | 搜索+提取工具 | P1 |
| 5 | Memory 持久记忆 | 无 | 键值记忆+画像 | P1 |
| 6 | Session 检索 | 简化 U/A/T log | FTS 检索+列表+多会话 | P1 |
| 7 | Skills 系统 | --skill-dir *.md | SKILL.md 规范+热加载 | P1 |
| 8 | Subagent 委派 | 无 | 子代理+并行批次 | P1 |
| 9 | Cron 调度 | 无 | 定时任务+投递 | P2 |
| 10 | MCP 接入 | 无 | MCP 客户端+远端工具 | P2 |
| 11 | 浏览器自动化 | 无 | browser 工具 | P2 |
| 12 | 视觉/图像 | 无 | vision 工具 | P2 |
| 13 | TTS 语音 | 无 | 语音输出(平台) | P2 |
| 14 | 权限审批 | shell 直跑(文档警示) | 命令 allowlist/确认钩子 | P1 |
| 15 | 桌面控制 | 无 | computer-use | P3 |

## Issue 草稿(12 条,待批准后批量创建)

1. provider: 多模型/多供应商抽象与 fallback 链
   body: 单 OpenAI 兼容 provider → 多供应商按优先级 fallback、模型级参数/重试。
   验收: 配置 provider 列表+顺序;主 provider 失败自动切换。
2. gateway: 消息平台接入(HTTP/Webhook 起步)
   body: CLI only → 常驻 gateway:HTTP API 会话路由(chat_id→session),
   后续 Feishu/WeCom/Telegram 适配器插件化(MotirisPluginApi 扩展)。
   验收: POST /v1/chat 多会话;GET /health;会话持久化。
3. tools: 文件工具集(read/write/patch/search)
   body: 仅 shell/time → read_file/write_file/patch/search_files,
   C 实现,权限路径约束。
   验收: 模型可读写仓库文件并搜索。
4. tools: web 搜索与页面提取
   body: 无网络工具 → web_search/web_extract 工具(JSON over curl)。
   验收: 模型可搜索并读页面要点。
5. memory: 持久记忆系统
   body: 无记忆 → 每会话/全局键值记忆文件,可被工具读写;
   用户画像/偏好条目。
   验收: 跨会话 recall 用户设置。
6. sessions: 会话检索与多会话管理
   body: 单 U/A/T log → 会话目录+全文搜索(FTS)+列表/恢复。
   验收: 按关键词检索历史会话。
7. skills: SKILL.md 规范与热加载
   body: --skill-dir 全注入 → 按需加载 SKILL.md(名称/描述索引),
   工具触发加载,支持子技能。
   验收: 模型按描述发现并加载技能。
8. agent: 子代理与并行任务
   body: 单线程 loop → subagent 委派(同进程 fork 或独立 agent),
   并行批次,结果回收。
   验收: 主代理可委派并拿到子结果。
9. scheduling: cron 定时任务
   body: 无 → 定时触发 agent 任务+结果投递(本地文件/HTTP)。
   验收: 定期任务可配置并执行留痕。
10. mcp: MCP 客户端接入
    body: 无 → MCP stdio/HTTP 客户端,远端工具注册为本机工具。
    验收: 接入一个 MCP server 后工具可用。
11. security: shell 工具权限审批
    body: 文档警示 → 命令 allowlist/denylist 配置+交互确认钩子。
    验收: 敏感/未放行命令被拦截或需确认。
12. tools: 浏览器自动化与视觉
    body: 无 → headless browser 驱动(第三方库)+截图/视觉分析。
    验收: 模型可浏览页面并"看图"回答。