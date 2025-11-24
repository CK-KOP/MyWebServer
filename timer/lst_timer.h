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
#include <ctime>
#include <random>
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
    util_timer() : expire(0), last_active(0), cb_func(nullptr), user_data(nullptr),
                    prev(nullptr), next(nullptr), wheel_level(0), slot_index(0) {}

public:
    time_t expire;
    time_t last_active; // 最近活跃时间
    void (* cb_func)(client_data *);
    client_data *user_data;    // 以便定时器需要销毁的时候顺便清理该连接

    // 时间轮专用字段
    util_timer* prev;      // 链表前驱
    util_timer* next;      // 链表后继
    int wheel_level;       // 所在层级（1或2）
    int slot_index;        // 所在槽索引

};

// 多级时间轮
class TimingWheel {
public:
    TimingWheel();
    ~TimingWheel();
    
    // 基本操作
    void add_timer(util_timer* timer);
    void adjust_timer(util_timer* timer, time_t new_last_active);
    void del_timer(util_timer* timer);
    void tick();
    
    // 配置
    void set_timeslot(int timeslot);
    void set_enable_level2(bool enable);  // 启用/禁用第二层（未来扩展）

    // 随机超时时间配置
    void set_timeout_range(int min_timeout, int max_timeout);
    int get_random_timeout();  // 获取随机超时时间（min~max 范围）

private:
    // ============ 第一层：实际使用（秒级粒度）============
    struct Level1 {
        static constexpr int SLOTS = 32;      // 32个槽
        static constexpr int GRANULARITY = 1; // 1秒粒度
        
        struct Slot {
            util_timer* head;  // 链表头
            Slot() : head(nullptr) {}
        };
        
        Slot slots[SLOTS];
        int current_slot;
        
        Level1();
    } level1;
    
    // ============ 第二层：预留扩展（32秒粒度）============
    struct Level2 {
        static constexpr int SLOTS = 8;        // 8个槽
        static constexpr int GRANULARITY = 32; // 32秒粒度
        
        struct Slot {
            util_timer* head;
            Slot() : head(nullptr) {}
        };
        
        Slot slots[SLOTS];
        int current_slot;
        
        Level2();
    } level2;
    
    bool enable_level2;  // 是否启用第二层
    int m_TIMESLOT;      // tick 间隔（秒）
    int m_min_timeout;      // 最小超时时间（秒）
    int m_max_timeout;      // 最大超时时间（秒）

    // 随机数生成器
    std::mt19937 m_rng;
    std::uniform_int_distribution<int> m_timeout_dist;
    
    // Level1 实际实现
    void add_to_level1(util_timer* timer, int timeout);
    void tick_single_level();
    void insert_to_slot_l1(int slot_idx, util_timer* timer);

    // Level2 预留接口（暂不实现）
    void add_to_level2(util_timer* timer, int timeout) {}
    void tick_multilevel() {}
    void demote_from_level2() {}
    void insert_to_slot_l2(int slot_idx, util_timer* timer) {}

    // 辅助函数
    void remove_from_slot(util_timer* timer);
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
    
    // 设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    // 定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();
    
    void show_error(int connfd, const char *info);

public:
    static int u_epollfd;
    int m_TIMESLOT;  // 由 Weberver类初始化

    // 修改：替换为新的时间轮
    TimingWheel m_timer_wheel;  // 替代原来的 sort_timer_lst m_timer_lst;
private:
    Utils() {}  // 构造函数私有化
    ~Utils() {}
};

void cb_func(client_data *user_data);
#endif
