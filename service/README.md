# pdfkit —— 在线 PDF 工具（8 个工具）

八个工具共用一套上传、任务队列、限额与下载链路。工具清单只写在
`service/tools.py` 一张表里，页面、导航、路由、校验全部从它派生——加一个转换
是加一行，不是再抄一份页面。

| 工具 | 页面 | 方向 |
|---|---|---|
| 合并 PDF | `/` | 多个 PDF → 一个 PDF |
| 压缩 PDF | `/compress` | PDF → 更小的 PDF |
| PDF 转 Word | `/convert/pdf-to-word` | pdf → docx |
| PDF 转 Excel | `/convert/pdf-to-excel` | pdf → xlsx（只提取表格） |
| PDF 转 PPT | `/convert/pdf-to-ppt` | pdf → pptx |
| Word 转 PDF | `/convert/word-to-pdf` | docx → pdf |
| Excel 转 PDF | `/convert/excel-to-pdf` | xlsx → pdf |
| PPT 转 PDF | `/convert/ppt-to-pdf` | pptx → pdf |
| 全部工具 | `/tools` | 工具网格 |

合并与压缩是自研实现；六个转换方向复用本仓库的 `pdfconv/` 引擎。本文档同时是
**容量模型的依据**，所有数字都来自本机实测，不是估算。

```
service/
  main.py          FastAPI 路由与限流入口
  merge.py         合并任务体（进程池内运行）
  jobs.py          任务状态机 + 有界队列 + 进程池
  storage.py       存储抽象（本地盘 → OSS/COS 只改这一个文件）
  limits.py        双桶令牌限流（IP + 设备指纹）
  static/index.html 前端页面（pdf.js 客户端生成缩略图）
  test_e2e.py      端到端测试（10 项断言）
  load_test.py     并发压测
  diag_cpu.py      并行扩展性测量
  diag_worker.py   进程池内每任务耗时拆分
```

## 一、先说目标：5 万「同时进行的任务」意味着什么

设任务平均生命周期为 W（上传 + 合并 + 下载），并发任务数 L，则

```
吞吐 λ = L / W
```

50,000 个任务同时在途：

| W | λ | 日处理量 | 双向带宽（10 MB/任务） |
|---|---|---|---|
| 15 s | 3,333 次/秒 | 2.9 亿次/天 | 53 Gbps |
| 30 s | 1,667 次/秒 | 1.4 亿次/天 | 27 Gbps |
| 60 s | 833 次/秒 | 7,200 万次/天 | 13 Gbps |

即使按最宽松的 W=60 s，也是每天 7,200 万次合并——约为 iLovePDF 全站量级的数倍。
**所以真实的容量瓶颈不是 CPU，是带宽**：27 Gbps 意味着每月约 8 PB 流量，按国内
CDN 0.2 元/GB 计约 1,600 万元/月。

结论：`L=50,000` 更适合当作**峰值在途会话数**而不是**同时执行的任务数**。工程上
正确的做法是让队列吸收尖峰（用户多等 1–2 秒），机器按 λ 备而不是按 L 备。

## 二、容量公式

```
单节点吞吐  λ_node = 有效核数 / 单任务 CPU 时间
所需节点数  N = λ_peak / λ_node / 目标利用率
```

单任务 CPU 时间只取决于**页数**，与文件体积关系不大——这是实测得出的反直觉结论：

| 输入 | 页数 | 耗时（`garbage=1`） | 耗时（`garbage=4, clean=True`） |
|---|---|---|---|
| 3 文件 / 3.5 MB | 33 | **0.089 s** | 0.362 s |
| 5 文件 / 10.2 MB | 70 | **0.285 s** | 2.404 s |

`clean=True` 是桌面端为最小体积做的调优：**慢 4–8.4 倍，输出体积只差 0.2%**
（9.51 MB vs 9.53 MB）。服务端必须去掉，这一项等于白捡 8 倍机器。

内存不是瓶颈：10 MB 输入峰值 +14.7 MB，31 MB 输入 +27.8 MB（进程基线 120 MB）。
所以扩容单位是**核**，不是内存；但每个 worker 进程要预留约 150 MB。

### 本机实测（务必重测）

这台开发机是 1.2 GHz 笔记本，8 逻辑核 / 4 物理核，但实测并行扩展性极差：

```
1 进程 1 任务 : 0.282 s
2 并行任务    : 0.529 s   speedup 1.07x   效率 53%
4 并行任务    : 0.703 s   speedup 1.61x   效率 40%
8 并行任务    : 1.100 s   speedup 2.05x   效率 26%
```

**有效算力约 2 核**。所以本机单节点只能跑 10 jobs/s（7 worker），这不是代码问题，
而是硬件上限。生产环境请用 `diag_cpu.py` 在目标机型上重新标定，再套上面的公式。

举例：一台 16 物理核的云主机，若有效核数按 12 计、单任务 0.15 s，
则 λ_node ≈ 80 jobs/s；要扛 1,667 jobs/s 需要约 21 台（80% 利用率下 26 台）。

## 三、架构铁律：文件字节不经过应用服务器

```
浏览器 ──(1) 申请上传地址──▶ API ──(2) 签名──▶ 对象存储
   │                                            ▲
   └──(3) 直传 bytes ───────────────────────────┘
API ──(4) 入队──▶ 队列 ──▶ Worker ──(5) 读输入/写输出──▶ 对象存储
浏览器 ──(6) 轮询状态──▶ API ──(7) 返回签名下载链接──▶ CDN
```

API 层只处理几 KB 的 JSON，所以 5 万连接也只需要 2–4 台；一旦让 API 转发文件
字节，带宽和内存会同时把 API 层打穿。P0 用本地盘，但接口形状与生产一致：
`storage.py` 换成 OSS/COS 的预签名实现，`main.py` 一行不用改。

**缩略图必须在前端生成**。后端渲染一页 150 dpi 约 50–100 ms，5 个文件就是
0.5 s——比合并本身（0.089 s）还慢 5 倍，等于把算力需求翻倍。

## 四、无账号下的限流

没有身份就无法做公平配额，所以用**双桶**，两者都通过才放行：

| 桶 | 默认速率 | 默认突发 | 设计理由 |
|---|---|---|---|
| IP | 60 次/小时 | 20 | 防伪造指纹；但按「500 人共享出口」定容量，避免误伤办公室 NAT |
| 设备指纹 | 120 次/小时 | 30 | 细化到人；指纹是客户端算的，可伪造，故不能单独作数 |

设备指纹 = canvas 渲染 + UA + 屏幕 + 时区 + 语言 + 逻辑核数，SHA-256 前 16 字节。
**它是可以被刷新的**，所以真正的兜底是 IP 桶 + 队列背压 + 文件大小上限。

队列满时返回 503 + `Retry-After`，这是最后一道闸：宁可拒绝新请求，也不让进程
在内存里堆任务。`PDFM_RATE_MULT` 可整体缩放限流参数，仅用于压测。

## 五、API 契约

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/config` | 限额、档位与当前方案，前端不再硬编码这些数字 |
| GET | `/api/v1/plans` | 全部方案（免费/Pro），定价页与升级卡片共用 |
| POST | `/api/v1/uploads` | 传 `{files:[{name,size}]}`，返回每个文件的 `file_id` 与上传 URL |
| PUT | `/api/v1/uploads/{id}` | 原始字节流式写入，首块校验 `%PDF-` 魔数 |
| POST | `/api/v1/jobs` | 传 `{task,files:[{file_id,rotate}],options}`，返回 `job_id` 与剩余额度 |
| GET | `/api/v1/jobs/{id}` | 状态 / 页数 / `queue_wait` / `run_time` / `expires_in` / 下载链接 |
| GET | `/api/v1/jobs/{id}/download` | 重新签发下载链接 |
| DELETE | `/api/v1/jobs/{id}` | 提前删除输入与输出 |
| GET | `/api/v1/download/{key}?exp=&sig=` | HMAC 签名下载，默认 15 分钟有效 |
| GET | `/healthz` | worker 数、队列深度、限流桶数量、计数 |

`task` 取 `merge` / `compress` / `convert`，默认 `merge`。
限额不是常量，来自 `plans.py` 的当前方案（免费版 20 MB / 5 页 / 1 次），
`GET /api/v1/config` 下发什么，前端就显示什么。

`convert` 的 `options` 为 `{tool, ocr, name}`：

| 字段 | 说明 |
|---|---|
| `tool` | 工具 id，见 `tools.py`；缺省 `pdf-to-word` |
| `ocr` | 免费版恒 `false`，传 `true` 返回 403 `plan_limit` |
| `name` | 只用来给下载文件命名；服务端剥掉路径分隔符与控制字符，**从不参与拼路径** |

`GET /api/v1/tools` 返回工具清单，`/tools` 网格页和每页顶部的导航都从它生成——
导航写死在各个页面里，是「新工具在网格里能点、在你正看的页面里看不见」的成因。

`rotate` 取 `0/90/180/270`，作用在该文件的**每一页**上，并且是相对于页面原有
`/Rotate` 的增量——扫描件本来就侧躺时，界面里转一次应当转正，而不是被强制设成
90°。写入的是 `/Rotate` 而不是重排内容流：所有阅读器都认，代价为零，原始页面
几何也不变。旧的 `{file_ids:[...]}` 请求体仍然接受，避免已缓存的页面在发版后
集体报错。

`queue_wait` 与 `run_time` 分开上报是刻意的：池子饱和时两者都会变大，但只有这个
拆分能告诉你该加 worker 还是该缩队列。

`expires_in` 由服务端按 `created + TTL` 算出，而不是让浏览器自己算：客户端时钟
偏差会让倒计时显示成负数或几十小时。

签名密钥持久化在 `service/data/secret.key`（或用 `PDFM_SECRET` 覆盖）。
**不能每次启动随机生成**：`uvicorn --workers 4` 下四个进程会各自拿到不同密钥，
A 进程签发的下载链接到 B 进程就是 403，而且每次重启会让已发出的链接全部失效。

## 六、压缩与付费分层

`POST /api/v1/jobs` 用 `task` 区分任务类型，默认 `merge`：

```json
{"task": "compress",
 "files": [{"file_id": "..."}],
 "options": {"level": "extreme"}}
```

三个档位实测（同一台机器，语料中的真实论文）：

| 档位 | dpi / jpeg 质量 | 94% 图片的论文 | 纯文字论文 |
|---|---|---|---|
| `extreme` 极限 | 72 / 45 | 5.77 MB → **0.39 MB（7%）** | 2.66 → 2.62 MB |
| `recommended` 推荐 | 110 / 70 | 5.77 MB → 0.70 MB（12%） | 2.66 → 2.63 MB |
| `less` 轻度 | 200 / 85 | 5.77 MB → 1.71 MB（30%） | 2.66 → 2.64 MB |

**决定压缩率的是图片占比，不是文件大小。** 图片占 3% 的论文怎么压都是原体积的
95–100%，占 94% 的论文能到 7%。所以对已经优化过的文件，服务端**原样返回原文件**
并在 `result` 里给出 `before == after`——前端据此提示「已是最优」，而不是假装省了 0%。
一个把文件变大、还把画质搞差的「压缩器」，比没有这个功能更糟。

`rewrite_images` 只动图像 XObject；矢量文字完全不受影响（实测 300% 放大后文字
依旧锐利）。极限档在 300% 下图片略有软化，正常阅读尺寸下不可分辨。

### 六个转换方向（`task: "convert"`）

引擎是本仓库的 `pdfconv/`（纯 Python：PyMuPDF 读，python-docx / openpyxl /
python-pptx 写，**没有 Aspose、没有 LibreOffice**）。服务层只做引擎不该关心的
三件事：拒绝、进度、报告。

本机实测（7 页论文 0.58 MB，或 3 页 PPT）：

| 方向 | 耗时 | 产物 |
|---|---|---|
| pdf → docx | 16.4 s | 2.08 MB，136 段文字 · 5 张图 |
| pdf → xlsx | 10.6 s | 0.03 MB，2 个工作表（只提取表格） |
| pdf → pptx | 12.6 s | 0.39 MB，7 张幻灯片 |
| docx → pdf | 0.5 s | 1 页 |
| xlsx → pdf | 7.1 s | 11 页，1355 个单元格 |
| pptx → pdf | 0.5 s | 3 页，文字保留 |

- **只有 pdf → docx 能报页进度**。其余是引擎里一次阻塞调用，所以 `GET /jobs/{id}`
  的 `progress` 只在这一路出现，别的方向前端显示不确定进度条——不编一个假的百分比。
- **扫描件默认拒绝**，返回 `needs_ocr`，而不是产出一份没有可编辑文字的文件——
  后者在用户眼里就是「转坏了」。判据是采样前 3 页，平均每页文字少于 20 个字符。
- **OCR 是付费钩子**，与 smallpdf 一致：`plans.py` 里 `ocr` 免费版 `false`、
  Pro `true`。引擎本身只要装了 rapidocr 就会自动 OCR，所以「关」是在
  `service/convert.py` 里临时替换 `pdfconv.ocr.available` 实现的——池里的
  worker 进程会被复用，因此**每次调用都要重新设置**，不能只在进程启动时设一次。
- **文件类型按容器判断，不看扩展名**。上传只校验 PDF 魔数或 zip 魔数（三种 OOXML
  共用），所以「把 .pptx 传到 Word 工具」能过上传这一关；`_detect_kind()` 打开 zip
  看 `word/` `xl/` `ppt/` 前缀，在**入队前**返回 400 `wrong_type`。
- **上传件没有扩展名，openpyxl 会因此报错**。存储键是 `in/<file_id>`，而
  openpyxl 靠文件名后缀判断格式，实测直接抛 `does not support file format`。
  `convert_task` 先把源文件复制成带正确后缀的临时文件再交给引擎，一条路径同时
  解决校验和转换两处。
- **Office → PDF 不套用免费版页数上限**。输出页数在渲染前无从得知，而额度在入队时
  就扣了——按页数事后判失败，等于把用户唯一一次机会烧在一个注定失败的任务上，
  正是 PDF 输入那条路刻意避免的坑。这个方向用 5000 页硬上限，靠 20 MB 输入上限
  控滥用。
- 输出扩展名写进存储键（`out/<job_id>.docx|xlsx|pptx|pdf`）。下载路由据此决定
  `Content-Type`：把 DOCX 当 `application/pdf` 发出去，Word 会直接拒绝打开。
- pdf → xlsx 的「工作表数」取自产物文件本身，不是引擎返回值：引擎返回的是它写了
  几个表格页，而它还会额外加 `Overview`/`Text` 页，直接显示会自相矛盾。

### 付费分层怎么接

所有限额集中在 `plans.py` 的 `PLANS` 表里，业务代码只读这张表：

| | 免费版 | Pro |
|---|---|---|
| 每次合并文件数 | 20 | 50 |
| 单文件上限 | **20 MB** | 2 GB |
| 合并后页数 | **5 页** | 5000 页 |
| 每天任务数 | **1 次** | 不限 |
| 批量压缩 | 否（一次 1 个文件） | 是 |
| OCR（扫描件转 Word） | 否 | 是 |

接计费时只需要动一个函数：

```python
def resolve_plan(request):   # plans.py
    return DEFAULT_PLAN      # ← 改成校验会话/签名令牌后返回 "pro"
```

三条已经守住的边界：

1. **不读任何客户端头**（如 `X-Plan`）。头是攻击者可控的，一旦信任它，付费层
   就形同虚设。`resolve_plan` 是唯一判定入口。
2. **额度只在任务真正入队后才扣**。参数校验失败、队列满返回 503、文件过期，
   都不消耗当天次数——否则用户会被自己的重试扣光。
3. **批量压缩是付费钩子**：免费版传 2 个文件返回 403 `plan_limit` 并说明原因，
   而不是悄悄只压第一个。
4. **页数在 API 层先算，早于扣额度**。免费版一天只有 1 次，如果页数超限留到
   worker 里才发现，用户唯一的免费机会就被一个注定失败的任务烧掉了。所以
   `create_job` 里先 `fitz.open()` 读 xref 统计总页数（20 MB 文件几毫秒），
   超限直接 400；worker 里再按同一上限校验一次，防止 API 之外的调用方绕过。
5. **前端先拦一次**。合并页在缩略图解析出页数后就禁用按钮并显示
   「合并后共 8 页，免费版最多 5 页」，压缩页在文件解析后直接拒绝，避免
   用户先把 20 MB 传完才被服务端打回。

额度显示必须带 `X-Device-Id` 请求头去问 `/api/v1/config`：配额是按设备键的，
不带这个头服务端会退回 User-Agent 分组，于是「刚用完额度」的页面会显示
「今天还剩 1 次」——这个 bug 已经踩过一次。

`Quota` 现在是进程内计数，`--workers 4` 下实际额度是标称值的 4 倍。生产必须换成
Redis `INCR` + 当日过期，接口已经是 `(key, limit) -> (ok, used)`，可以直接替换。

## 七、运行

```bash
pip install -r service/requirements.txt

python service/run.py            # 推荐入口，见第八节
python service/run.py --open     # 顺便把浏览器打开
python service/run.py --port 9001   # 换端口
```

默认端口是 **8090**（8080 常被别的服务占用）。三种改法任选：
`--port 9001`、环境变量 `PDFM_PORT=9001`、systemd 改单元里的
`Environment=PDFM_PORT`。Docker/nginx 里的端口是写死的，换端口时要同步改
`Dockerfile` 的 `EXPOSE`/`CMD`、`docker-compose.yml` 的映射、`nginx.conf` 的
`proxy_pass` 三处。

也可以直接交给 uvicorn —— 但**必须 `--workers 1`**：

```bash
python -m uvicorn service.main:app --host 0.0.0.0 --port 8090 --workers 1
# 合并 http://127.0.0.1:8090/    压缩 http://127.0.0.1:8090/compress    转换 http://127.0.0.1:8090/tools
```

### 测试

```bash
python service/test_e2e.py        # 14 项合并端到端断言
python service/test_compress.py   # 8 项压缩端到端断言（含「不会变大」）
python service/test_convert.py    # 12 项：六个方向全跑 + 四类拒绝
python service/test_limits.py     # 用生产默认值验证 5 页 / 20 MB / 1 次 / 无批量
python service/shot_ui.py         # 合并页：真实浏览器跑一遍全部交互
python service/shot_compress.py   # 压缩页：真实浏览器跑两条路径并截图
python service/shot_convert.py    # 工具网格 + 通用转换页（含反向 Word→PDF）
python service/shot_limits.py     # 浏览器里验证限额拦截与额度耗尽
python service/load_test.py 60    # 60 个并发任务
```

这八个套件**都自己起服务**，不需要先手动启动，也不会和正在跑的实例抢端口。
区别只在限额：

* `test_e2e` / `test_compress` / `test_convert` / `shot_ui` / `shot_compress` /
  `shot_convert` 走 `service/_harness.py`：随机空闲端口 + 独立 `PDFM_DATA` + 放宽限额；
* `test_limits` / `shot_limits` 故意**不共用** harness，自己按生产默认值起服务。

为什么要分开：语料里的论文是 8–30 页，功能套件必须放宽到 5000 页才跑得动；
而限额又必须用**生产默认值**验证，否则「5 页」被改成「500 页」没人会发现。

> 这两类断言混在一起的后果是真实发生过的：`test_e2e` 早期写死了固定端口，免费版
> 落地后立刻 `400 too_many_pages` 全红 —— 功能没坏，是测试在替产品限额报警。

环境变量：`PDFM_WORKERS`（默认 CPU-1）、`PDFM_QUEUE`（默认 64）、
`PDFM_TTL`（默认 3600 秒）、`PDFM_DATA`、`PDFM_MAX_FILE_MB`（默认 20）、
`PDFM_MAX_TASK_MB`、`PDFM_MAX_FILES`、`PDFM_MAX_PAGES`（默认 5）、
`PDFM_FREE_JOBS_PER_DAY`（默认 1）、`PDFM_SECRET`、`PDFM_RATE_MULT`、
`PDFM_HOST`、`PDFM_PORT`。

## 八、部署（后台常驻 + 网页操作）

四种形态，业务代码完全相同。**每种都是「一条命令」**，区别只是那条命令之前需要
准备什么：

| 形态 | 命令 | 前提 |
|---|---|---|
| 本机试一下 | `python service/run.py` | 有 Python + 依赖 |
| Linux 常驻 | `sudo ./service/deploy/install.sh` | root、systemd、能联网装依赖 |
| Docker | `docker compose -f service/docker-compose.yml up -d --build` | 装了 Docker |
| 已有 nginx | 先跑上面任一，再套 `deploy/nginx.conf` | 域名/证书 |

`run.py` 是**前台**进程，关掉终端就停——适合试；常驻要 systemd 或 Docker。

### 1. 本机 / 局域网：`run.py`

```bash
python service/run.py                 # 0.0.0.0:8090，手机同网可访问
python service/run.py --port 9000
python service/run.py --host 127.0.0.1   # 只给本机
```

启动时会打印可直接点开的地址：

```
pdfkit
  local : http://127.0.0.1:8090/
  LAN   : http://192.168.150.19:8090/  (same Wi-Fi / office network)
  note  : binding 0.0.0.0 exposes this to your network; there is no login by design
  merge : http://127.0.0.1:8090/
  compress: http://127.0.0.1:8090/compress
  convert : http://127.0.0.1:8090/tools  (8 个工具 / 6 个转换方向)
  workers: 7 PDF processes, 1 web process
  data   : C:\...\service\data
```

Windows 双击 `service/start.bat`，Linux/macOS 用 `./service/start.sh`。

**绑 `0.0.0.0` 等于把服务交给整个网段**——设计上没有登录，任何能连上的人都能
上传文件、消耗当天额度。只在可信网络这么做；对公网必须走下面的 nginx。

Windows 首次启动会弹防火墙授权，必须勾选「专用网络」否则手机连不上；
事后可在「Windows Defender 防火墙 → 允许应用通过防火墙」里补。想临时只给本机
用就加 `--host 127.0.0.1`，那样根本不触发防火墙。

### 2. Docker

```bash
docker build -f service/Dockerfile -t pdfkit .
docker run -d --name pdfkit -p 8090:8090 -v pdfdata:/data pdfkit
# 或
docker compose -f service/docker-compose.yml up -d --build
```

镜像以 uid 10001 非 root 运行，`PDFM_DATA=/data` 挂在命名卷上（`secret.key`
在里面，卷丢了所有已签发的下载链接一起失效），HEALTHCHECK 打 `/healthz`。

### 3. systemd（Linux 常驻）

**一条命令**（脚本会自己装依赖、建用户、建 venv、装单元、启动、等健康检查）：

```bash
sudo ./service/deploy/install.sh                  # 默认 0.0.0.0:8090
sudo ./service/deploy/install.sh --port 9000
sudo ./service/deploy/install.sh --host 127.0.0.1 # 只给本机，前面挂 nginx
```

跑完会打印可直接访问的地址。幂等：改完代码再跑一次就是原地升级 + 重启，
`/var/lib/pdfkit`（上传件、签名密钥）不动。

> 注意「一条命令」指的是**首次安装**。装完之后的日常运维也是一条：
> `systemctl restart pdfkit` / `journalctl -u pdfkit -f`。
> 而且这条命令要求：root、systemd、能联网装 pip 依赖。缺一样它会明确告诉你缺什么。

**不想用脚本就手工来**，其实只有五步：

```bash
sudo useradd --system --home-dir /opt/pdfkit --shell /usr/sbin/nologin pdfkit
sudo mkdir -p /opt/pdfkit /var/lib/pdfkit
sudo tar -C . -cf - --exclude=service/data service pdfconv | sudo tar -C /opt/pdfkit -xf -
sudo python3 -m venv /opt/pdfkit/.venv
sudo /opt/pdfkit/.venv/bin/pip install -r /opt/pdfkit/service/requirements.txt
sudo chown -R pdfkit:pdfkit /var/lib/pdfkit
sudo cp service/deploy/pdfkit.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now pdfkit
journalctl -u pdfkit -f
```

（Debian/Ubuntu 上 `python3-venv` 是单独一个包，缺了 `venv` 会失败——脚本会先探测再装。）

单元里已经写好 `--workers 1`、`NoNewPrivileges`、`ProtectSystem=strict`，
`ReadWritePaths=/var/lib/pdfkit` 是唯一的可写点（`instance.pid`、`secret.key`、
上传件都在里面，所以整个 `/opt` 可以只读）。绑地址由 `PDFM_HOST`/`PDFM_PORT`
两个环境变量控制，单元里用 `${...}` 引用，改地址不用动 `ExecStart`。

### 4. nginx 反代（对公网时）

```bash
sudo cp service/deploy/nginx.conf /etc/nginx/conf.d/pdfkit.conf
```

下面几项是**功能性**的，不是调优项，写错会以很难查的方式坏掉：

| 配置 | 为什么 |
|---|---|
| `client_max_body_size 25m` | 默认 1m。免费版允许 20 MB，nginx 会先返回 413，应用根本收不到请求 |
| `X-Forwarded-For $remote_addr` | 不能用 `$proxy_add_x_forwarded_for`：那会**追加**客户端自带的值，攻击者伪造首段就能绕过 IP 限流 |
| `/healthz` 只允许 127.0.0.1 | 健康检查不该对外暴露 worker/队列深度 |

### 三条硬约束

1. **一个数据目录只能跑一个 web 进程。** 任务状态和每日额度都在进程内存里，
   `--workers 2` 会让 A 建的 job 在 B 上 404。并发靠 `PDFM_WORKERS`
   （只做 PDF 的子进程），不是多开 web worker。启动时 `_instance_guard()`
   会检测同目录下的 `instance.pid` 并警告，而不是让你之后收到随机 404。
2. **`docker run --scale` / 多副本横向扩容现在还不行。** 同上：状态在内存。
   要扩容必须先按第九节的表把队列、任务状态、限流、额度换到 Redis。
3. **数据目录别提交、别共享。** `service/data/` 是上传件 + 产物 + 签名密钥，
   已在 `.gitignore` 里。

## 九、P0 → 生产还差什么

| 项 | P0 现状 | 生产要求 |
|---|---|---|
| 存储 | 本地盘 | OSS/COS + 生命周期规则 1 小时删除 + 跨可用区冗余 |
| 队列 | 进程内 `queue.Queue` | Redis Stream / Kafka，跨节点共享，支持重投与死信 |
| 任务状态 | 进程内 dict | Redis 带 TTL，多副本可读 |
| 限流 | 进程内令牌桶 | Redis + Lua 原子扣减，否则多副本各算各的 |
| 每日额度 | 进程内计数（4 worker = 4 倍额度） | Redis `INCR` + 当日过期，同一身份跨节点共享 |
| 计费 | `resolve_plan()` 恒返回 free | 会话校验 + 支付回调改方案，业务代码不动 |
| 扩容 | 手动 | 按队列深度 HPA（K8s） |
| 下载 | 应用直接回源 | CDN 回源 + Range 支持 |
| 前端缩略图 | 已在前端 | 同 |
| 加密/损坏 PDF | 已拦截并给出可读错误 | 同，另加 XFA、超大页面兜底 |
| 可观测 | `/healthz` 计数 | Prometheus 指标 + 队列等待时间分位 + 失败码分布 |
| 合规 | 无 | ICP 备案（域名 + CDN 加速域名均需备案）、上传内容留存策略 |

**Windows 上的坑**：`ProcessPoolExecutor` 用 spawn 启动子进程，会重新导入
`__main__`。任何在模块顶层创建进程池的脚本都必须包在 `if __name__ ==
"__main__":` 里，否则子进程会递归创建进程池并崩溃（`diag_pool.py` 踩过）。
服务本身不受影响，因为 uvicorn 的 `__main__` 已有该保护。
