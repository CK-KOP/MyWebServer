#ifndef WEBSERVER_H
#define WEBSERVER_H
#include <sys/socket.h>
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

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

const int MAX_FD = 65536;           // 最大文件描述符
const int MAX_EVENT_NUMBER = 10000; // 最大事件数
const int TIMESLOT = 5;             // 最小超时单位

class WebServer{
public:
    WebServer();
    ~WebServer();

    // 初始化
    void init(int port, string user, string passWord, string databaseName,
              int log_write, int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log);
    void log_write();
    void sql_pool();
    void thread_pool();
    void trig_mode();

    // 关于用户http类数据以及定时器的相关函数
    void create_timer(int connfd, struct sockaddr_in client_address);
    void adjust_timer(util_timer *timer);
    void deal_timer(util_timer * timer, int sockfd);
    
    // 关于新客户端连接、信号响应、客户端数据读写的处理函数
    bool dealclientdata();
    bool dealwithsignal(bool &timeout, bool &stop_server);
    void dealwithread(int sockfd);
    void dealwithwrite(int sockfd);

    // epoll注册创建以及事件循环
    void eventListen();
    void eventLoop();
    
public:
    // 基础
    int m_port;
    char *m_root;
    int m_log_write;    // 日志方法 0为同步，1为异步
    int m_close_log;    // 是否关闭日志

    int m_pipefd[2];
    int m_epollfd;
    http_conn *users;   // 保存每个客户端的 HTTP 连接对象，每个对象对应一个 socket

    // 数据库相关
    connection_pool *m_connPool;
    string m_user;          // 登陆数据库用户名
    string m_passWord;      // 登陆数据库密码
    string m_databaseName;  // 使用数据库名
    int m_sql_num;

    // 线程池相关
    threadpool<http_conn> *m_pool;
    int m_thread_num;

    //epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER];
    
    int m_listenfd;
    int m_OPT_LINGER;
    int m_TRIGMode;
    int m_LISTENTrigmode;
    int m_CONNTrigmode;

    //定时器和工具相关
    client_data *users_client_data; // 每个客户端对应的定时器信息，包含 socket、客户端地址以及 util_timer*
};

#endif
