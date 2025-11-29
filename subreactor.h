#ifndef SUBREACTOR_H
#define SUBREACTOR_H

#include <sys/socket.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
#include <queue>
#include <unordered_map>
#include <memory>
#include <string>
#include <atomic>
#include <thread>

#include "./http/http_conn.h"
#include "./timer/lst_timer.h"
#include "./utils/utils.h"

const int SUB_MAX_EVENT_NUMBER = 10000; // SubReactor最大事件数

class SubReactor {
public:
    SubReactor(int sub_reactor_id, const char* root, int conn_trig_mode, int close_log,
               const std::string& user, const std::string& passWord, const std::string& databaseName,
               connection_pool* connPool);
    ~SubReactor();

    // 启动SubReactor线程
    void start();

    // 停止SubReactor线程
    void stop();

    // 添加新连接（由主Reactor调用）
    bool add_connection(int connfd, struct sockaddr_in client_address);

    // 获取当前连接数
    int get_connection_count() const { return m_user_count.load(); }

    // 获取SubReactor ID
    int get_sub_reactor_id() const { return m_sub_reactor_id; }

private:
    // SubReactor主事件循环
    void eventLoop();

    // 处理客户端数据读取
    void dealwithread(int sockfd);

    // 处理客户端数据发送
    void dealwithwrite(int sockfd);

    // 处理异常连接
    void dealwithexception(int sockfd);

    // 初始化epoll
    void initEpoll();

    // 创建定时器
    void create_timer(int connfd, struct sockaddr_in client_address);

    // 调整定时器
    void adjust_timer(util_timer* timer);

    // 关闭连接
    void close_connection(util_timer *timer, int sockfd);
    void close_connection_by_timer(int sockfd);

    // 定时器处理
    void timer_handler();

private:
    int m_sub_reactor_id;                              // SubReactor ID
    char* m_root;                                      // 资源目录路径
    int m_conn_trig_mode;                              // 连接触发模式
    int m_close_log;                                   // 是否关闭日志

    // 数据库相关
    connection_pool* m_connPool;                       // 数据库连接池
    std::string m_user;                                // 数据库用户名
    std::string m_passWord;                            // 数据库密码
    std::string m_databaseName;                        // 数据库名

    // epoll相关
    int m_epollfd;                                     // epoll文件描述符
    epoll_event events[SUB_MAX_EVENT_NUMBER];          // 事件数组

    // 定时器相关
    int m_timerfd;                                     // 定时器文件描述符
    TimingWheel m_timer_wheel;                         // 时间轮

    // 客户端连接管理
    std::atomic<int> m_user_count{0};                  // 当前连接数
    std::unordered_map<int, std::unique_ptr<http_conn>> m_users;     // HTTP连接对象
    std::unordered_map<int, std::unique_ptr<client_data>> m_clients; // 客户端数据

    // 线程相关
    std::thread m_thread;                              // SubReactor线程
    std::atomic<bool> m_running{false};                // 运行状态

    // 连接添加队列（线程安全）
    std::queue<std::pair<int, sockaddr_in>> m_pending_connections;
    std::mutex m_connection_mutex;                     // 连接队列锁
    std::condition_variable m_connection_cv;           // 连接条件变量
};

#endif // SUBREACTOR_H