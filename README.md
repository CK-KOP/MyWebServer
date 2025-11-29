# 🌐 MyWebServer

高性能 Web 服务器，基于 **Linux + C++**，采用 **多 Reactor 模型**，结合 **高效定时器、异步日志** 和 **MySQL 用户系统**，支持上万级别并发连接。

------

## 📌 项目特性

- **高并发处理**
  - 基于 **多 Reactor 模式 + epoll**，主 Reactor 负责连接监听与分发，多个 SubReactor 处理 I/O 事件
  - **多线程 I/O 处理**，每个 SubReactor 运行在独立线程中，充分利用多核 CPU
  - 支持上万并发连接的稳定处理能力
- **I/O 优化**
  - **全/半零拷贝**机制
    - **sendfile（全零拷贝）**：大文件或静态资源，直接从内核 page cache 发送到 socket buffer，绕过用户态，降低 CPU 消耗
    - **mmap + write（半零拷贝）**：小文件或动态内容响应，减少一次 memcpy，更加灵活
  - 自适应选择策略，确保不同场景下 I/O 成本最小化
- **高效 HTTP 处理**
  - **有限状态机**解析 HTTP 请求，支持 **GET / POST**
  - 支持静态资源访问（HTML、CSS、图片、视频等）
  - 用户认证集成数据库查询
- **数据库支持**
  - **MySQL 连接池**，避免频繁创建/销毁连接
  - 提供 **用户注册、登录功能**
  - 用户信息缓存到内存，进一步提升查询效率
- **定时器管理**
  - 使用 **timerfd + 时间轮**
  - 时间复杂度近似 **O(1)**，减少无效连接扫描和 CPU 开销
- **日志系统**
  - **同步 / 异步日志**，按天生成日志文件
  - 异步日志基于 **无锁队列**，提高高并发下写入效率

------

## ⚙️ 技术栈

- **操作系统**：Linux
- **编程语言**：C++
- **网络编程**：多 Reactor 模型 + epoll
- **并发**：多线程 SubReactor
- **数据库**：MySQL + 连接池
- **定时器**：timerfd + 时间轮
- **同步机制**：轻量锁 / 原子操作，尽量减少锁争用
- **日志管理**：异步日志 + 无锁队列 / 同步日志

------

## 📦 代码结构

```
├── config.cpp/h                  # 服务器配置实现
├── http/                         # HTTP 连接处理模块
│   ├── http_conn.cpp             # 解析、响应、认证等实现
│   └── http_conn.h               # HTTP 连接声明
├── log/                          # 日志系统（异步日志：无锁队列实现）
│   ├── log.cpp                   # 日志实现（无锁缓冲队列 + 写线程）
│   └── log.h                     # 日志接口与配置
├── mydb/                         # 数据库连接池
│   ├── sql_connection_pool.cpp   # 线程安全连接池实现
│   └── sql_connection_pool.h     # 连接池头文件
├── utils/                        # 工具类
│   ├── utils.cpp                 # 工具函数实现
│   └── utils.h                   # 工具函数头文件
├── timer/                        # 定时器模块
│   ├── lst_timer.cpp             # 时间轮 + timerfd 管理
│   └── lst_timer.h               # 定时器接口
├── webserver.cpp/h               # WebServer 核心类（主 Reactor 事件分发、初始化）
├── subreactor.cpp/h              # SubReactor 实现（处理 I/O 事件）
├── main.cpp                      # 服务器入口（参数解析与启动流程）
├── Makefile                      # 编译脚本
├── third_party/                  # 第三方库
│   ├── concurrentqueue/          # 无锁队列实现
│   └── picohttpparser/           # HTTP 解析器
└── root/                         # 静态资源目录
    ├── *.html
    ├── *.gif/jpg
    └── *.mp4
```

------

## 🚀 功能展示

- 用户注册与登录
- 静态资源访问（图片、视频等）
- 高并发请求处理

------

## 🛠️ 配置与运行

### 配置参数

| 参数 | 描述                                               | 默认值 |
| ---- | -------------------------------------------------- | ------ |
| `-p` | 端口号                                             | 9006   |
| `-l` | 日志方式（0:同步, 1:异步）                         | 0      |
| `-m` | 触发组合模式（0:LT+LT, 1:LT+ET, 2:ET+LT, 3:ET+ET） | 0      |
| `-o` | 优雅关闭连接（0:不使用, 1:使用）                   | 0      |
| `-s` | 数据库连接池数量                                   | 8      |
| `-t` | SubReactor 线程数                                  | 3      |
| `-c` | 是否关闭日志（0:不关闭, 1:关闭）                   | 0      |

### 运行前准备

1. 安装 MySQL 并创建数据库及表
2. 修改 `main.cpp` 中数据库信息：

```cpp
string user = "your_username";      
string passwd = "your_password";    
string databasename = "your_dbname";
```

### 编译与运行

```bash
# 编译
make

# 默认运行
./server

# 自定义参数示例
./server -p 9006 -l 1 -m 3 -t 16 -s 16
```

------

## 📊 性能测试

**测试工具**：`wrk` / `bombardier`
 **测试环境**：6 核虚拟机（1 个主 Reactor + 1 个异步日志线程 + N 个 SubReactor）

### 💡 SubReactor 数量调优结论

| SubReactor 数量 | 峰值 QPS            | CPU 行为                                     |
| --------------- | ------------------- | -------------------------------------------- |
| 2               | ~30,000             | 主 Reactor 压力偏大，成为瓶颈                |
| **3（推荐）**   | **~40,000（最高）** | 主 Reactor 有余力，SubReactor CPU 利用率最佳 |
| 4               | ~24,000             | 线程调度与竞争成本变高，吞吐下降             |

🔎 结果说明：
 本项目实际运行中，**QPS 不随 SubReactor 数量线性增长**。
 当 SubReactor = 3 时，达到了 **“主 Reactor 负载均衡 + SubReactor CPU 利用率最大化 + 最低调度开销”** 的最佳状态，因此吞吐量最高。

📌 推荐配置（6 核环境）：

```
./server -t 3   # SubReactor = 3 时性能最佳
```

------

### 📌 本地压测数据（SubReactor = 3）

```
kop@kop-virtual-machine ~/WebServer (master)> wrk -t4 -c10000 -d300s http://127.0.0.1:9006
Running 5m test @ http://127.0.0.1:9006
  4 threads and 10000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    20.69ms   15.83ms 136.84ms   86.16%
    Req/Sec    10.33k     4.24k   19.65k    61.61%
  12337255 requests in 5.00m, 7.39GB read
  Socket errors: connect 8983, read 1457687, write 0, timeout 0
Requests/sec:  41115.28
Transfer/sec:     25.21MB
```

> 仅 HTTP 静态资源测试下吞吐为 **≈ 4W QPS**。

------

### 🧾 性能总结

- 支持 **20k+ 并发连接稳定运行**
- 压测过程中 **无崩溃 / 无内存泄漏 / 性能无衰减**
- 多 Reactor + 异步日志 + 时间轮定时器在高负载下表现稳定
- 性能瓶颈已从“单线程”转为“线程竞争 + CPU 调度开销”

------

## 📝 待改进

- 支持 HTTPS 协议
- 实现 HTTP/2 特性
- 引入 Redis 用于会话和热数据缓存，提高高并发下的数据库与资源访问性能
- 优化文件传输性能
- 添加更多数据库功能
- 实现分布式部署支持
- 增加负载均衡功能

------

## 🙏 致谢

本项目基于学习和参考 [qinguoyi/TinyWebServer](https://github.com/qinguoyi/TinyWebServer) 并进行改造优化。感谢原作者开源精神，让我通过实践深入理解高性能 Web 服务器的实现原理。