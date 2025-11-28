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
#include <sys/sendfile.h>
#include <map>

#include "../mydb/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

extern "C" {
    #include "picohttpparser/picohttpparser.h"
}

class http_conn {
public:
    // ========== 常量定义 ==========
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 8192;
    static const int WRITE_BUFFER_SIZE = 4096;
    static const int MAX_HEADERS = 32;
    static const int SENDFILE_THRESHOLD = 32 * 1024;  // 32KB
    
    // ========== 枚举类型 ==========
    enum METHOD {
        GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATH
    };
    
    enum HTTP_CODE {
        NO_REQUEST,           // 请求不完整，需要继续读取
        GET_REQUEST,          // 获得了完整的HTTP请求
        BAD_REQUEST,          // HTTP请求报文有语法错误
        NO_RESOURCE,          // 请求的资源不存在
        FORBIDDEN_REQUEST,    // 没有访问权限
        FILE_REQUEST,         // 文件请求成功
        INTERNAL_ERROR,       // 服务器内部错误
        CLOSED_CONNECTION     // 客户端已关闭连接
    };
    
    // URL路由常量
    enum ROUTE_TYPE {
        ROUTE_REGISTER_PAGE = '0',  // 注册页面
        ROUTE_LOGIN_PAGE = '1',     // 登录页面
        ROUTE_LOGIN_CHECK = '2',    // 登录验证
        ROUTE_REGISTER_CHECK = '3', // 注册验证
        ROUTE_PICTURE = '5',        // 图片页面
        ROUTE_VIDEO = '6',          // 视频页面
        ROUTE_FANS = '7'            // 粉丝页面
    };

    // process处理结果状态
    enum PROCESS_RESULT {
        PROCESS_OK = 0,         // 处理成功，等待写事件
        PROCESS_ERROR = -1,      // 处理失败，需要关闭连接
        PROCESS_CONTINUE = 1     // 请求不完整，需要继续读取
    };

public:
    http_conn() {}
    ~http_conn() {}

    // ========== 公共接口 ==========
    void init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
              int close_log, std::string user, std::string passwd, std::string sqlname,
              int epollfd);
    
    int read_once();
    int write();  // 1: 写完成, 0: 需要继续写, -1: 写错误
    PROCESS_RESULT process();
    
    sockaddr_in *get_address() { return &m_address; }
    bool is_keep_alive() { return m_keep_alive; }

    // ========== epollfd改为成员变量 ==========
    int m_epollfd;

    // ========== 静态方法 ==========
    // 初始化数据库用户数据表（静态方法）
    static void init_database_users(connection_pool *connPool);

    // 连接计数管理（可以外部维护）
    // static int m_user_count;  // 移除，改为外部管理

    // ========== 数据库连接 ==========
    MYSQL *mysql;
    
    // ========== 读写状态 ==========
    int m_state;  // 0: 读, 1: 写

    // ========== 连接状态 ==========
    bool m_peer_closed;  // 对端是否已经关闭

private:
    // ========== 初始化 ==========
    void init();
    
    // ========== HTTP解析（使用picohttpparser）==========
    HTTP_CODE process_read();
    bool parse_method(const char *method, size_t len);
    bool parse_url(const char *path, size_t len);
    bool parse_headers(struct phr_header *headers, size_t num);
    
    // ========== 请求处理 ==========
    HTTP_CODE do_request();
    HTTP_CODE handle_cgi_request(char route_flag);
    bool handle_user_login(const char *name, const char *password);
    bool handle_user_register(const char *name, const char *password);
    void route_to_page(char route_type);
    
    // ========== 响应构建 ==========
    bool process_write(HTTP_CODE ret);
    bool add_response(const char *format, ...);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_connection_header();
    bool add_blank_line();
    bool add_content(const char *content);
    
    // ========== 数据发送 ==========
    int write_with_mmap();
    int write_with_sendfile();
    
    // ========== 文件处理 ==========
    void unmap();

private:
    // ========== 连接信息 ==========
    int m_sockfd;
    sockaddr_in m_address;
    
    // ========== 读缓冲区 ==========
    char m_read_buf[READ_BUFFER_SIZE];
    long m_read_idx;
    
    // ========== 写缓冲区 ==========
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;

    // ========== HTTP请求信息 ==========
    METHOD m_method;
    char m_url_buf[FILENAME_LEN];
    char *m_url;
    char m_host_buf[256];
    char *m_host;
    long m_content_length;
    bool m_keep_alive;  // 改名: m_linger -> m_keep_alive
    
    // ========== POST请求相关 ==========
    bool m_is_post_form;  // 改名: cgi -> m_is_post_form
    char *m_request_body; // 改名: m_string -> m_request_body
    
    // ========== 响应文件信息 ==========
    char m_real_file_path[FILENAME_LEN];  // 改名: m_real_file -> m_real_file_path
    char *m_file_address;
    struct stat m_file_stat;
    
    // ========== sendfile支持 ==========
    bool m_use_sendfile;
    int m_file_fd;
    off_t m_sendfile_offset;     // 改名: m_sf_offset -> m_sendfile_offset
    size_t m_sendfile_remaining; // 改名: m_sf_remaining -> m_sendfile_remaining
    
    // ========== 响应发送控制 ==========
    struct iovec m_response_iov[2];  // 改名: m_iv -> m_response_iov
    int m_response_iov_count;        // 改名: m_iv_count -> m_response_iov_count
    int m_bytes_to_send;             // 改名: bytes_to_send -> m_bytes_to_send
    int m_bytes_have_send;           // 改名: bytes_have_send -> m_bytes_have_send
    
    // ========== 数据库配置 ==========
    char m_sql_user[100];
    char m_sql_passwd[100];
    char m_sql_name[100];
    
    // ========== 服务器配置 ==========
    char *m_doc_root;      // 改名: doc_root -> m_doc_root
    int m_trigger_mode;    // 改名: m_TRIGMode -> m_trigger_mode
    int m_close_log;
    
};

#endif