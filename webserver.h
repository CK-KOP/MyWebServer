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

const int MAX_FD = 65536;           // 最大文件描述符
const int MAX_EVENT_NUMBER = 10000; // 最大事件数
const int TIMESLOT = 1;             // 最小超时单位

class WebServer{
public:
    WebServer();
    ~WebServer();

    // 初始化
    void init(int port, std::string user, std::string passWord, std::string databaseName,
              int log_write, int opt_linger, int trigmode, int sql_num,
              int close_log);
    void log_write();
    void sql_pool();
    void trig_mode();

    // 关于用户http类数据以及定时器的相关函数
    void create_timer(int connfd, struct sockaddr_in client_address);
    void adjust_timer(util_timer *timer);
    void close_connection(util_timer * timer, int sockfd);
    void close_connection_by_timer(int sockfd);  // 定时器回调调用的版本，不删除定时器
    
    // 关于新客户端连接、客户端数据读写的处理函数
    bool dealclientdata();
    void dealwithread(int sockfd);
    void dealwithwrite(int sockfd);

    // 定时器方法
    void timer_handler();

    // epoll注册创建以及事件循环
    void eventListen();
    void eventLoop();
    
public:
    // 基础
    int m_port;
    char *m_root;
    int m_log_write;    // 日志方法 0为同步，1为异步
    int m_close_log;    // 是否关闭日志

    int m_epollfd;
    int m_timerfd;

    // 客户端连接管理 - 使用智能指针管理生命周期
    std::unordered_map<int, std::unique_ptr<http_conn>> m_users;
    std::unordered_map<int, std::unique_ptr<client_data>> m_clients;

    // 连接计数管理
    std::atomic<int> m_user_count{0};

    // 数据库相关
    connection_pool *m_connPool;
    std::string m_user;          // 登陆数据库用户名
    std::string m_passWord;      // 登陆数据库密码
    std::string m_databaseName;  // 使用数据库名
    int m_sql_num;

    //epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER];

    int m_listenfd;
    int m_OPT_LINGER;
    int m_TRIGMode;
    int m_LISTENTrigmode;
    int m_CONNTrigmode;

    // 时间轮 - 从Utils移过来
    TimingWheel m_timer_wheel;
};

#endif
