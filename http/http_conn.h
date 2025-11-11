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
#include "../mydb/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

// 引入 picohttpparser
extern "C" {
    #include "picohttpparser/picohttpparser.h"
}

class http_conn{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    static const int MAX_HEADERS = 32;  // 最多支持的header数量
    
    enum METHOD {
        GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATH
    };
    
    enum HTTP_CODE{
        NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE,
        FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char * root, int TRIGMode, 
              int close_log, string user, string passwd, string sqlname);
    void initmysql_result(connection_pool * connPool);
    void close_conn(bool real_close = true);
    bool read_once();
    bool write();
    void process();
    sockaddr_in *get_address(){ return &m_address; }

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;
    int m_state;        // 读为0, 写为1
    int timer_flag;
    int improv;

private:
    void init();
    
    // HTTP 解析相关（使用 picohttpparser）
    HTTP_CODE process_read();
    bool parse_method(const char *method, size_t len);
    bool parse_url(const char *path, size_t len);
    bool parse_headers(struct phr_header *headers, size_t num);
    
    // 业务逻辑处理
    HTTP_CODE do_request();
    
    // 文件映射
    void unmap();
    
    // 响应构建
    bool add_response(const char *format, ...);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
    bool add_content(const char *content);
    bool process_write(HTTP_CODE ret);

private:
    // 连接信息
    int m_sockfd;
    sockaddr_in m_address;
    
    // 读缓冲区
    char m_read_buf[READ_BUFFER_SIZE];
    long m_read_idx;
    
    // 写缓冲区
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;

    // 请求信息
    METHOD m_method;
    char m_url_buf[FILENAME_LEN];   // URL存储
    char *m_url;                     // 指向 m_url_buf
    char m_host_buf[256];            // Host存储
    char *m_host;                    // 指向 m_host_buf
    long m_content_length;
    bool m_linger;
    
    // 响应信息
    char m_real_file[FILENAME_LEN];
    char *m_file_address;
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;
    int bytes_to_send;
    int bytes_have_send;
    
    // POST 相关
    int cgi;
    char *m_string;     // POST body数据
    
    // 配置
    char *doc_root;
    map<string, string> m_users;
    int m_TRIGMode;
    int m_close_log;
    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif