#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst(){}

// 析构函数，删除所有定时器，防止内存泄漏
sort_timer_lst::~sort_timer_lst(){
    for(auto timer : timers_set){
        delete timer;
    }
}

void sort_timer_lst::set_timeslot(int timeslot){
    m_TIMESLOT = timeslot;
}

void sort_timer_lst::add_timer(util_timer *timer){
    if (!timer) return;
    timers_set.insert(timer);
}

void sort_timer_lst::adjust_timer(util_timer *timer, time_t new_last_active){
    if (!timer) return;
    
    // ✓ 只更新last_active，不调整set
    timer->last_active = new_last_active;
}

void sort_timer_lst::del_timer(util_timer *timer){
    if (!timer) return;
    timers_set.erase(timer);
    delete timer;
}

// 遍历set，如果定时器的过期时间小于当前时间，则触发定时器的回调函数并删除到期定时器
void sort_timer_lst::tick(){
    if (timers_set.empty()) return;
    
    auto start = std::chrono::steady_clock::now();
    time_t cur = time(nullptr);
    
    std::vector<util_timer*> to_update;
    std::vector<util_timer*> to_delete;
    
    int expired_count = 0;  // 统计过期的
    int extended_count = 0;  // 统计延期的
    
    while (!timers_set.empty()){
        auto it = timers_set.begin();
        util_timer* timer = *it;
        
        if (timer->expire > cur) break;
        
        timers_set.erase(it);
        expired_count++;
        
        if (timer->last_active > 0 && 
            cur - timer->last_active < 3 * m_TIMESLOT){
            timer->expire = timer->last_active + 3 * m_TIMESLOT;
            to_update.push_back(timer);
            extended_count++;
        }
        else{
            to_delete.push_back(timer);
        }
    }
    
    for (auto timer : to_update){
        timers_set.insert(timer);
    }
    
    for (auto timer : to_delete){
        timer->cb_func(timer->user_data);
        delete timer;
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    LOG_INFO("tick() - took %ld us, total_timers: %lu, expired: %d, extended: %d, deleted: %lu",
             duration.count(), timers_set.size(), 
             expired_count, extended_count, to_delete.size());

    // 每隔一段时间输出一次统计
    static int tick_count = 0;
    static long total_us = 0;
    static long max_us = 0;
    
    tick_count++;
    total_us += duration.count();
    if (duration.count() > max_us) max_us = duration.count();
    
    if (tick_count % 6 == 0) {  // 每6次tick（30s）输出一次
        LOG_INFO("Timer stats - Count: %d, Avg: %ld us, Max: %ld us, "
                 "Current timers: %lu, Expired: %d, Extended: %d, Deleted: %d",
                 tick_count, total_us / tick_count, max_us,
                 timers_set.size(), expired_count, extended_count, 
                 to_delete.size());
    }
}

void Utils::init(int timeslot){
    m_TIMESLOT = timeslot;
    m_timer_lst.set_timeslot(timeslot);
}

// 对文件描述符设置非阻塞
int Utils::setnonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode){
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else 
        event.events = EPOLLIN | EPOLLRDHUP;
    
    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 信号处理函数
void Utils::sig_handler(int sig){
    // 为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

// 设置信号函数
void Utils::addsig(int sig, void(handle)(int), bool restart){
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handle;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    // 将所有信号添加到信号屏蔽字中，表示在信号处理函数执行期间其他信号将被阻塞。
    sigfillset(&sa.sa_mask);
    // 通过 sigaction 系统调用注册信号处理函数
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler(){
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info){
    send(connfd, info, strlen(info), 0);
    close(connfd);
} 

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
// 回调函数 关闭套接字，并更新用户连接数
void cb_func(client_data *user_data){
    // 在webserver.cpp 会将epollfd赋值给这里的 u_epollfd的
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    // 关闭套接字
    close(user_data->sockfd);
    http_conn::m_user_count--;
}

