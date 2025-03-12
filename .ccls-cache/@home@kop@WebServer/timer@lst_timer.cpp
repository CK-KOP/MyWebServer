#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst(){
    head = NULL;
    tail = NULL;
}

sort_timer_lst::~sort_timer_lst(){
    util_timer *tmp = head;
    while (tmp){
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer){
    if (!timer)
        return;
    if (!head){
        head = tail = *timer;
        return;
    }
    util_timer *tmp = head;
    while (tmp){
        if (timer->expire < tmp->expire){
            util_timer *PREV = tmp->prev;
            timer->prev = tmp->prev;
            timer->next = tmp;
            if (PREV)
                PREV->next = timer;
            tmp->prev = timer;
            break;
        }
        tmp = tmp->next;
    }
    // timer的时间在队列中是最大的
    if (!tmp){
        tail->next = timer;
        timer->prev = tail;
        timer->next = NULL;
        tail = timer;
    }
}

void sort_timer_lst::adjust_timer(util_timer *timer){
    if (!timer)
        return;
    util_timer *tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire))
        return;
    // 假如该timer不是有序的了 就删除再添加
    if (timer == head){
        head = timer->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer);
    }
    else{
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        timer->prev = NULL;
        timer->next = NULL;
        add_timer(timer);
    }
}

void sort_timer_lst::del_timer(util_timer *timer){
    if (!timer)
        return;
    if ((timer == head) && (timer == tail)){
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head){
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail){
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

// 遍历链表，如果定时器的过期时间小于当前时间，则触发定时器的回调函数并删除到期定时器
void sort_timer_lst::tick(){
    if (!head)
        return;
    time_t cur = time(NULL);
    util_timer *tmp = head;
    while (tmp){
        if (tmp->expire > tmp)
            break;
        tmp->cb_func(tmp->user_data);
        head = tmp->next;
        if (head)
            head->prev = NULL;
        delete tmp;
        tmp = head;
    }
}

void Utils::init(int timeslot){
    m_TIMESLOT = timeslot;
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
    event.data_fd = fd;

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
    sa.sa_handler = handler;
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
    alarm(_TIMESLOT);
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
