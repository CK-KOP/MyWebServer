#ifndef WEBSERVER_H
#define WEBSERVER_H
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

#include "./http/http_conn.h"
#include "./timer/lst_timer.h"
#include "./utils/utils.h"
#include "./subreactor.h"

const int MAX_FD = 65536;           // 最大文件描述符
const int MAX_EVENT_NUMBER = 10000; // 最大事件数
const int TIMESLOT = 1;             // 最小超时单位

class WebServer{
public:
    WebServer();
    ~WebServer();

    // 初始化
    void init(int port, std::string user, std::string passWord, std::string databaseName,
              int log_write, int opt_linger, int trigmode, int sql_num, int thread_num,
              int close_log);
    void log_write();
    void sql_pool();
    void trig_mode();

    // 创建并启动SubReactors
    void create_sub_reactors();

    // 启动和停止所有SubReactors
    void start_sub_reactors();
    void stop_sub_reactors();

    // 主Reactor：处理新客户端连接
    bool dealclientdata();

    // 连接分发
    bool dispatch_connection(int connfd, struct sockaddr_in client_address);

    // epoll注册创建以及事件循环（主Reactor）
    void eventListen();
    void eventLoop();
    
public:
    // 基础
    int m_port;
    char *m_root;
    int m_log_write;    // 日志方法 0为同步，1为异步
    int m_close_log;    // 是否关闭日志

    int m_epollfd;  // 主Reactor的epollfd

    // 数据库相关
    connection_pool *m_connPool;
    std::string m_user;          // 登陆数据库用户名
    std::string m_passWord;      // 登陆数据库密码
    std::string m_databaseName;  // 使用数据库名
    int m_sql_num;

    // SubReactor相关
    int m_thread_num;            // SubReactor线程数
    std::vector<std::unique_ptr<SubReactor>> m_sub_reactors;  // SubReactor数组

    //epoll_event相关（主Reactor用）
    epoll_event events[MAX_EVENT_NUMBER];

    int m_listenfd;
    int m_OPT_LINGER;
    int m_TRIGMode;
    int m_LISTENTrigmode;
    int m_CONNTrigmode;

    // 连接分发 - 轮询算法
    std::atomic<int> m_next_sub_reactor{0};
};

#endif
