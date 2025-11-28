#ifndef UTILS_H
#define UTILS_H

#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cassert>
#include <sys/epoll.h>

// 网络编程工具类 - 静态方法集合
class Utils {
public:
    // 设置文件描述符为非阻塞
    static int setnonblocking(int fd);

    // 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    // trigger_mode: 0=LT模式, 1=ET模式
    static void addfd(int epollfd, int fd, bool one_shot, int trigger_mode);

    // 修改文件描述符在epoll中的事件
    // trigger_mode: 0=LT模式, 1=ET模式
    static void modfd(int epollfd, int fd, int ev, int trigger_mode);

    // 从epoll中删除文件描述符
    static void removefd(int epollfd, int fd);

    // 设置信号函数
    static void addsig(int sig, void(handler)(int), bool restart = true);

    // 显示错误信息并关闭连接
    static void show_error(int connfd, const char *info);
};

#endif