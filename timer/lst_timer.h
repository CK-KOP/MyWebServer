#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <time.h>
#include <vector>
#include <set>
#include "../log/log.h"

class util_timer;

struct client_data{
    sockaddr_in address;
    int sockfd;         // 为什么还要保存fd，是因为这个对象也许不是通过fd下标访问的，可能是定时器的指针访问到的
    util_timer *timer;  // 指向定时器是为了通过fd管理到这个客户端的定时器
};

class util_timer{
public:
    util_timer() {}

public:
    time_t expire;
    time_t last_active; // 最近活跃时间
    void (* cb_func)(client_data *);
    client_data *user_data;    // 以便定时器需要销毁的时候顺便清理该连接

};


struct TimerCmp {
    bool operator()(const util_timer* a, const util_timer* b) const {
        if (a->expire != b->expire) return a->expire < b->expire;
        return a < b;  // 指针地址作为二级排序，保证不同 timer 不会被认为重复
    }
};

class sort_timer_lst{
public:
    sort_timer_lst();
    ~sort_timer_lst();
    
    void add_timer(util_timer *timer);
    void adjust_timer(util_timer *timer, time_t new_last_active);
    void del_timer(util_timer *timer);
    void tick();

    void set_timeslot(int timeslot);

private:
    std::set<util_timer*, TimerCmp> timers_set;
    int m_TIMESLOT; // 由 Utils类 初始化
};

class Utils{
public:
    static Utils& get_instance() {
        static Utils instance;
        return instance;
    }

    // 禁止拷贝与赋值
    Utils(const Utils&) = delete;
    Utils& operator=(const Utils&) = delete;

    
    void init(int timeslot);
    
    // 对文件描述符设置非阻塞
    int setnonblocking(int fd);

    // 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);
    
    // 信号处理函数
    static void sig_handler(int sig);

    // 设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    // 定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();
    
    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;
    sort_timer_lst m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT;  // 由 Weberver类初始化

private:
    Utils() {}  // 构造函数私有化
    ~Utils() {}
};

void cb_func(client_data *user_data);
#endif
