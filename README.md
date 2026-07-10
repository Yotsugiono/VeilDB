# MyEncDB

MyEncDB 是一个基于 Intel SGX 的加密数据库实验项目，包含 SGX/C++ 核心服务端、FastAPI HTTP 网关和静态 Web 控制台。浏览器通过 FastAPI 调用网关，网关再通过 TCP/RPC 与 `encdb_server` 通信。

## 项目结构

```text
.
├── AGENTS.md                 # 仓库开发规范
├── README.md                 # 统一项目说明
├── EncDB/
│   ├── System_high/          # SGX/C++ 核心服务、Enclave、客户端和 benchmark
│   │   ├── CryptoEnclave/    # Enclave 代码
│   │   ├── CryptoTestingApp/ # 服务端、客户端、RPC、管理逻辑
│   │   ├── common/           # 公共头文件
│   │   └── Makefile
│   ├── data/                 # 示例数据和数据生成脚本
│   └── Databases/            # 运行时持久化数据库文件
└── web/
    ├── requirements.txt      # FastAPI 网关依赖
    ├── gateway/              # HTTP 网关和 EncDB RPC 桥接
    ├── frontend/             # 静态前端页面
    └── scripts/              # 调试脚本
```

## 技术栈

- 核心数据库：C/C++、Intel SGX SDK、SGX Enclave
- 网关接口：Python、FastAPI、Uvicorn、Pydantic
- 前端：HTML、CSS、原生 JavaScript
- 通信链路：Browser -> HTTP JSON -> FastAPI -> TCP/RPC -> `encdb_server` -> SGX Enclave

## SGX 环境配置

以下步骤来自原 `环境配置.md`，适用于 Ubuntu 20.04 / WSL 环境。参考资料：

- [Ubuntu 20.04 Intel SGX 快速环境配置](https://zhuanlan.zhihu.com/p/682980135)
- [Ubuntu 20.04 安装 Intel SGX](https://blog.csdn.net/weixin_43292691/article/details/119917066)

安装必要软件包：

```bash
sudo apt install build-essential ocaml ocamlbuild automake autoconf libtool wget python-is-python3 libssl-dev git cmake perl unzip
sudo apt install libssl-dev libcurl4-openssl-dev protobuf-compiler libprotobuf-dev debhelper cmake reprepro unzip pkgconf libboost-dev libboost-system-dev libboost-thread-dev lsb-release libsystemd0
```

获取并准备 Intel SGX SDK 源码：

```bash
git clone https://github.com/intel/linux-sgx.git
cd linux-sgx
make preparation
sudo cp external/toolset/ubuntu20.04/{ar,as,ld,objcopy,objdump,ranlib} /usr/local/bin
```

编译并安装 SGX SDK：

```bash
make sdk
make sdk_install_pkg
cd linux/installer/bin
./sgx_linux_x64_sdk_${version}.bin
```

安装时可选择 `no`，并指定安装目录为 `/opt/intel`。安装后加载环境变量：

```bash
source /opt/intel/sgxsdk/environment
```

验证 SGX SDK：

```bash
cd /opt/intel/sgxsdk/SampleCode/LocalAttestation
make SGX_MODE=SIM
cd bin
./app
```

## 常见编译问题

如果编译项目时出现类似错误：

```bash
/usr/local/bin/ld: cannot find -lpcap
/usr/local/bin/ld: cannot find -lsgx_usgxssl
collect2: error: ld returned 1 exit status
make: *** [Makefile:162: encdb_client] Error 1
```

先安装 `libpcap-dev`：

```bash
sudo apt install libpcap-dev
```

再安装 sgx-ssl：

```bash
git clone https://github.com/intel/intel-sgx-ssl.git
cd intel-sgx-ssl/openssl_source
wget https://www.openssl.org/source/openssl-3.0.12.tar.gz
cd ../Linux
make
```

将生成的 include 和 lib64 放到 `/opt/intel/sgxssl`：

```bash
sudo mkdir -p /opt/intel/sgxssl
sudo cp -r ./package/include /opt/intel/sgxssl
sudo cp -r ./package/lib64 /opt/intel/sgxssl
```

## 构建 EncDB 服务端

进入核心服务目录：

```bash
cd EncDB/System_high
```

常用构建命令：

```bash
make SGX_MODE=SIM
```

如果升级了服务端代码，建议清理后重编：

```bash
make clean
make SGX_MODE=SIM
```

服务端和客户端可执行文件：

```bash
# 服务端
./encdb_server

# 客户端
./encdb_client
```

`EncDB/System_high/README.md` 中还保留了 SGX HW/SIM、Debug/Release 等 Makefile 构建模式说明。

## Web 网关与前端

Web 层通过 HTTP 网关桥接 TCP/RPC。上传原文功能需要使用支持双平面 UPDATE 和捆绑存储的新版本 `encdb_server`。

目录说明：

```text
web/
├── requirements.txt
├── gateway/
│   ├── main.py        # FastAPI 入口
│   ├── config.py      # 环境变量和路径配置
│   ├── audit.py       # 审计日志持久化与读取
│   ├── bridge.py      # TCP 客户端
│   ├── encdb_rpc.py   # 与 RPC.cpp 对齐的编解码
│   ├── sql_builder.py # SQL 到 EncDB 请求的转换
│   └── tokenize.py    # 上传文档索引构建
└── frontend/
    ├── index.html
    ├── app.js
    └── style.css
```

## 运行方式

终端 1：启动 EncDB 服务端。

```bash
cd EncDB/System_high
./encdb_server
```

终端 2：启动 FastAPI 网关。

```bash
cd web
pip install -r requirements.txt
export ENCDB_HOST=127.0.0.1
export ENCDB_PORT=9000
export ENCDB_DATA_DIR=../EncDB/data/enron
uvicorn gateway.main:app --host 0.0.0.0 --port 8080
```

浏览器访问：

```text
http://<机器IP>:8080/static/index.html
```

本机访问通常为：

```text
http://127.0.0.1:8080/static/index.html
```

## 网关环境变量

`web/gateway/config.py` 支持以下环境变量：

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `ENCDB_HOST` | `127.0.0.1` | `encdb_server` 地址 |
| `ENCDB_PORT` | `9000` | `encdb_server` 端口 |
| `ENCDB_DATA_DIR` | `EncDB/data/enron` | `/api/insert` 读取示例文档的目录 |
| `ENCDB_UPLOAD_DIR` | `web/gateway/uploads` | 上传文档 sidecar 存放目录 |
| `ENCDB_DATABASES_DIR` | `EncDB/Databases` | 持久化数据库目录 |
| `DOC_ID_MIN` | `1` | 文档 ID 下限 |
| `DOC_ID_MAX` | `10000` | 文档 ID 上限 |
| `HTTP_HOST` | `0.0.0.0` | 网关监听地址配置项 |
| `HTTP_PORT` | `8080` | 网关监听端口配置项 |
| `SESSION_CLIENT_ID_START` | `100` | 网关会话 client_id 起始值 |
| `AUDIT_LOG_LIMIT` | `500` | 审计日志默认返回条数 |

## API 摘要

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/` | 服务信息，返回 UI 和 API docs 路径 |
| `GET` | `/api/health` | 网关配置和目录健康信息 |
| `GET` | `/api/databases` | 已持久化数据库列表 |
| `GET` | `/api/databases/{edb_id}/key/status` | 查询数据库密钥状态，不返回密钥内容 |
| `DELETE` | `/api/databases/{edb_id}` | 管理者删除未连接的持久化数据库 |
| `POST` | `/api/session/init` | 创建新会话；传入 `edb_id` 时连接已有数据库 |
| `POST` | `/api/session/shutdown` | 保存并关闭当前会话 |
| `GET` | `/api/session/{session_id}` | 查询会话信息 |
| `POST` | `/api/insert` | 从 `ENCDB_DATA_DIR` 插入 Enron 示例文档 |
| `POST` | `/api/upload` | 上传本地文档并建立索引 |
| `POST` | `/api/replace` | 替换已有文档 |
| `POST` | `/api/delete` | 删除指定文档 |
| `POST` | `/api/query` | 执行 `SELECT` 查询 |
| `GET` | `/api/audit/logs` | 查询审计日志；支持 `edb_id` 和 `limit` 查询参数 |

FastAPI 自动文档地址：

```text
http://127.0.0.1:8080/docs
```

## 密钥管理

当前版本采用“每个数据库一个独立密钥”的设计。创建新数据库时，`encdb_server` 只负责调度 Enclave；真实数据密钥由 Enclave 内部通过 SGX 随机数生成接口产生，FastAPI 网关、浏览器前端和普通服务端进程都不生成、不上传、不导出密钥明文。

密钥保存在 Enclave 的运行时上下文中，并随 `ClientContext` 一起写入 SGX sealed context：

```text
EncDB/Databases/edb_<id>/context.dat
```

重新连接已有数据库时，Enclave 会从 `context.dat` 解封上下文并恢复该数据库自己的密钥。网关提供密钥状态查询接口，只返回作用域、托管来源、封存状态和状态说明，不返回密钥值或密钥指纹。历史版本创建的数据库仍会使用其 sealed context 中已有的密钥；新创建的数据库会使用 Enclave 内部生成的独立密钥。当前版本不提供密钥上传、导出或轮换功能。

## 数据库删除

管理者界面可以在“数据库连接”模块输入数据库编号，或在数据库状态列表中点击已有数据库后，点击“删除数据库”移除对应的持久化目录。后端会拒绝删除正在被当前 Web 会话使用的数据库；如需删除当前连接的数据库，请先点击“保存并断开”。

删除操作会调用：

```text
DELETE /api/databases/{edb_id}
```

删除成功后，数据库列表和审计日志会自动刷新；删除记录写入全局审计日志。

## 权限管理

前端提供演示级的基于组访问控制。使用者进入界面后必须选择且只能选择一个所属组；管理者可以在“权限管理”面板配置各组对每个数据库的权限。

权限分为三类：

- 无权限：不能连接数据库。
- 只读：可以连接数据库并执行 `SELECT` 查询。
- 读写：可以连接数据库、查询、插入、上传和替换文档。

权限矩阵保存在浏览器 `localStorage` 中，刷新页面后仍会保留。当前版本只在前端表达和限制权限，不作为后端强制鉴权机制；管理员创建、删除数据库和查看审计日志不受组权限限制。

## 审计日志

网关会在数据库操作入口写入服务器端审计日志，包括创建/连接数据库、保存断开、插入、上传、替换、删除和查询。日志按数据库持久化：

```text
EncDB/Databases/edb_<id>/audit.jsonl
```

每行是一条 JSON 记录，包含时间、数据库编号、会话、client_id、角色、操作类型、目标、状态、耗时和详情。管理者界面的“审计日志”面板通过以下接口读取：

```text
GET /api/audit/logs
GET /api/audit/logs?edb_id=3
GET /api/audit/logs?edb_id=3&limit=100
```

## 开发注意事项

- 运行数据库文件位于 `EncDB/Databases`，默认不建议提交。
- 上传缓存位于 `web/gateway/uploads`，默认不建议提交。
- 修改前端静态文件后，如果浏览器仍加载旧脚本，可强刷新或更新 `index.html` 中资源版本查询串。
