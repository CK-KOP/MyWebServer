#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
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
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn{
public:
    static const int FILENAME_LEN = 200;        // 文件名最大长度
    static const int READ_BUFFER_SIZE = 2048;   // 读取缓冲区大小
    static const int WRITE_BUFFER_SIZE = 1024;  // 写入缓冲区大小
    
    enum METHOD {                               // HTTP请求方法枚举
        GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATH
    };

    enum CHECK_STATE {                         // 主状态机 请求解析的状态
        CHECK_STATE_REQUESTLINE = 0,            // 请求行状态
        CHECK_STATE_HEADER,                     // 请求头状态
        CHECK_STATE_CONTENT                    // 请求体状态
    };
    
    enum HTTP_CODE{         // HTTP 响应状态代码
        NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE,
        FORBIDDEN_REQUST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION
    };

    enum LINE_STATUS{       // 从状态机 解析行的状态 
        LINE_OK = 0, LINE_BAD, LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char * root, int TRIGMode, int close_log, string user, string passwd,string sqlname);
    void initmysql_result(connection_pool * connPool);
    void close_conn(bool real_close = true);
    bool read_once(); // 从客户端socket读取数据到缓冲区 
    bool write();     // 将缓冲区和文件的内容传输到客户端socket
    void process();   // 将读缓存区的数据读取出来并解析将返回数据写入到写缓冲区 
    sockaddr_in *get_address(){
        return &m_address;
    }

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;
    int m_state; // 读为0, 写为1
    int timer_flag;
    int improv;

private:
    void init();
    // 将读缓冲区请求内容读取出来并解析所用到的函数
    LINE_STATUS parse_line();
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    char *get_line() { return m_read_buf + m_start_line; }
    HTTP_CODE process_read(); // 从读缓冲区解析整体HTTP请求
    HTTP_CODE do_request();   // 根据解析结果获取要返回的文件内容
    
    void unmap(); // 解除文件映射
    
    // 将响应内容写入到写缓存区所用到的函数
    bool add_response(const char *format, ...);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
    bool add_content(const char *content);
    bool process_write(HTTP_CODE ret); // 写入到写缓冲区并初始化m_iv参数

private:
    int m_sockfd;
    sockaddr_in m_address;
    
    char m_read_buf[READ_BUFFER_SIZE];
    long m_read_idx;
    long m_checked_idx;
    int m_start_line; // 现在要解析的行的开始位置
    
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;

    CHECK_STATE m_check_state;
    METHOD m_method;

    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    long m_content_length;
    bool m_linger;         // 是否使用长连接
    char *m_file_address;
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;
    int cgi;            // 是否启用的POST
    char *m_string;     // 存储POST的请求内容
    int bytes_to_send;
    int bytes_have_send;
    char *doc_root;  // 网站根目录

    map<string, string>m_users; // 数据库中读取出来的用户账号
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
