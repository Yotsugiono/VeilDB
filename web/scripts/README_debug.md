# 网关聚合诊断脚本

## 文件

- `debug_aggregate_gateway.py` — 诊断 `match_count=0`

## 前置条件

1. WSL 中 `encdb_server` 已启动（新编译版本）
2. 与网关相同的环境变量（可选）：

```bash
export ENCDB_HOST=127.0.0.1
export ENCDB_PORT=9000
export ENCDB_DATA_DIR=/你的路径/EncDB/data/enron
```

3. 数据集目录下存在 `1`、`2`、`3` 等文件

## 使用方式

```bash
cd web
python3 scripts/debug_aggregate_gateway.py
```

默认行为：

- `client_id=199`（避免与 CLI `client_id=1` 冲突）
- INIT → INSERT 1,2,3 → `SELECT MAX firm OR name`
- 打印 6 个步骤的诊断信息

## 常用参数

```bash
# 指定聚合 SQL
python3 scripts/debug_aggregate_gateway.py --sql "SELECT MAX firm AND enron"

# 指定 client_id（与网页网关 session 对齐时，需先在网页 init 后知道 client_id）
python3 scripts/debug_aggregate_gateway.py --client-id 100

# 已插入过数据，跳过 INSERT
python3 scripts/debug_aggregate_gateway.py --skip-insert

# 已 INIT 过且复用同一 client_id
python3 scripts/debug_aggregate_gateway.py --skip-init --skip-insert --client-id 100
```

## 如何读输出

| Step | 关注点 |
|------|--------|
| Step 3 | `select_type` 必须为 `1`（MAX）/ `2`/`3`/`4` |
| Step 5 | `decode_cstr` 应含 `ENCDB_AGG|op=MAX\|match_count=数字\|value=数字` |
| Step 6 | `match_count` 应与 CLI 的 `matched_docs` 一致 |

## 结果对照

1. **Step 5 有完整 ENCDB_AGG，Step 6 match_count=0**  
   → 问题在 `response_util.py` 解析，把 Step 5 全文复制反馈。

2. **Step 5 无 ENCDB_AGG，CLI 却有 meta**  
   → 脚本连的 server/库与 CLI 不一致；或用了不同 `client_id`。

3. **Step 5 和 Step 6 都正确，网页仍为 0**  
   → 浏览器看 `/api/query` 原始 JSON；或重启 uvicorn、清缓存。

## 对比网页 session（可选）

网页 INIT 后，在网关日志或临时打印 session 的 `client_id`，然后：

```bash
python3 scripts/debug_aggregate_gateway.py \
  --client-id <网页的client_id> \
  --skip-init --skip-insert \
  --sql "SELECT MAX firm OR name"
```

若此时 Step 5 正常而网页为 0，问题在 FastAPI 路由或前端，不在 TCP 解码。
