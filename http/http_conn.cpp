
#include "http_conn.h"
#include <mysql/mysql.h>
#include <fstream>

// ========== HTTP响应状态信息 ==========
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file from this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

// ========== 全局变量 ==========
std::mutex m_mutex;
std::map<std::string, std::string> users;

// ========== 静态成员初始化 ==========
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

// ========== 工具函数（无需修改，仅更新变量名引用）==========

// 设置文件描述符为非阻塞
int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将文件描述符添加到epoll
void addfd(int epollfd, int fd, bool one_shot, int trigger_mode) {
    epoll_event event;
    event.data.fd = fd;

    if (trigger_mode == 1)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 从epoll中删除文件描述符
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改epoll中的文件描述符
void modfd(int epollfd, int fd, int ev, int trigger_mode) {
    epoll_event event;
    event.data.fd = fd;

    if (trigger_mode == 1)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// ========== 初始化函数 ==========

// 从数据库加载用户信息
void http_conn::init_mysql_result(connection_pool *connPool) {
    // 从连接池获取一个连接
    ConnectionGuard connGuard(*connPool);
    MYSQL* mysql = connGuard.get();

    // 查询用户表
    if (mysql_query(mysql, "SELECT username, passwd FROM user")) {
        LOG_ERROR("SELECT error: %s\n", mysql_error(mysql));
        return;
    }

    // 获取结果集
    MYSQL_RES *result = mysql_store_result(mysql);
    
    // 将用户名和密码存入map
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        std::string username(row[0]);
        std::string password(row[1]);
        users[username] = password;
    }
}

// 外部调用的初始化函数
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int trigger_mode,
                     int close_log, std::string user, std::string passwd, std::string sqlname) {
    m_sockfd = sockfd;
    m_address = addr;
    
    // 将socket添加到epoll
    addfd(m_epollfd, sockfd, true, trigger_mode);
    m_user_count++;
    
    // 保存配置
    m_doc_root = root;
    m_trigger_mode = trigger_mode;
    m_close_log = close_log;
    strcpy(m_sql_user, user.c_str());
    strcpy(m_sql_passwd, passwd.c_str());
    strcpy(m_sql_name, sqlname.c_str());

    init();
}

// 内部初始化函数
void http_conn::init() {
    // 重置读写索引
    m_read_idx = 0;
    m_write_idx = 0;
    
    // 重置请求信息
    m_method = GET;
    m_url = m_url_buf;
    m_host = m_host_buf;
    m_content_length = 0;
    m_keep_alive = false;
    
    // 重置POST相关
    m_is_post_form = false;
    m_request_body = nullptr;
    
    // 重置发送控制
    m_bytes_to_send = 0;
    m_bytes_have_send = 0;
    
    // 重置文件相关
    m_use_sendfile = false;
    m_file_fd = -1;
    m_file_address = nullptr;
    
    // 其他状态
    mysql = nullptr;
    m_state = 0;
    
    // 清空缓冲区
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file_path, '\0', FILENAME_LEN);
    memset(m_url_buf, '\0', FILENAME_LEN);
    memset(m_host_buf, '\0', 256);
}

// 关闭连接
void http_conn::close_conn(bool real_close) {
    if (real_close && (m_sockfd != -1)) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 解除内存映射或关闭文件描述符
void http_conn::unmap() {
    if (!m_use_sendfile && m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = nullptr;
    }
    if (m_use_sendfile && m_file_fd != -1) {
        close(m_file_fd);
        m_file_fd = -1;
    }
}


// ========== 数据读取 ==========

// 从socket读取数据到缓冲区
int http_conn::read_once() {
    if (m_read_idx >= READ_BUFFER_SIZE)
        return false;
    
    int bytes_read = 0;

    // LT模式
    if (m_trigger_mode == 0) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, 
                         READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;
        
        if (bytes_read < 0)
            return -1;
        else if (bytes_read == 0)
            return 0;
        
        return 1;
    }
    // ET模式：需要一次性读完
    else {
        while (true) {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, 
                            READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return -1;
            }
            else if (bytes_read == 0)
                return 0;
            
            m_read_idx += bytes_read;
        }
        return 1;
    }
}


// ========== HTTP解析 ==========

// 解析HTTP方法
bool http_conn::parse_method(const char *method, size_t len) {
    if (len == 3) {
        if ((method[0] == 'G' || method[0] == 'g') &&
            (method[1] == 'E' || method[1] == 'e') &&
            (method[2] == 'T' || method[2] == 't')) {
            m_method = GET;
            m_is_post_form = false;
            return true;
        }
    }
    
    if (len == 4) {
        if ((method[0] == 'P' || method[0] == 'p') &&
            (method[1] == 'O' || method[1] == 'o') &&
            (method[2] == 'S' || method[2] == 's') &&
            (method[3] == 'T' || method[3] == 't')) {
            m_method = POST;
            m_is_post_form = true;
            return true;
        }
        if ((method[0] == 'H' || method[0] == 'h') &&
            (method[1] == 'E' || method[1] == 'e') &&
            (method[2] == 'A' || method[2] == 'a') &&
            (method[3] == 'D' || method[3] == 'd')) {
            m_method = HEAD;
            return true;
        }
    }
    
    return false;
}

// 解析URL
bool http_conn::parse_url(const char *path, size_t len) {
    if (len == 0 || len >= FILENAME_LEN - 20)
        return false;
    
    // picohttpparser已经处理好路径，直接复制
    memcpy(m_url_buf, path, len);
    m_url_buf[len] = '\0';
    
    // 根路径默认跳转到judge.html
    if (len == 1 && m_url_buf[0] == '/') {
        strcat(m_url_buf, "judge.html");
    }
    
    return true;
}

// 解析HTTP头部
bool http_conn::parse_headers(struct phr_header *headers, size_t num) {
    m_content_length = 0;
    m_keep_alive = false;
    m_host_buf[0] = '\0';
    
    for (size_t i = 0; i < num; i++) {
        size_t name_len = headers[i].name_len;
        size_t value_len = headers[i].value_len;
        
        // Connection头
        if (name_len == 10 && strncasecmp(headers[i].name, "Connection", 10) == 0) {
            if (value_len == 10 && 
                strncasecmp(headers[i].value, "keep-alive", 10) == 0) {
                m_keep_alive = true;
            }
        }
        // Content-Length头
        else if (name_len == 14 && strncasecmp(headers[i].name, "Content-Length", 14) == 0) {
            // 手动解析数字（value不是null结尾）
            m_content_length = 0;
            for (size_t j = 0; j < value_len && j < 20; j++) {
                char c = headers[i].value[j];
                if (c >= '0' && c <= '9') {
                    m_content_length = m_content_length * 10 + (c - '0');
                } else {
                    break;
                }
            }
        }
        // Host头
        else if (name_len == 4 && strncasecmp(headers[i].name, "Host", 4) == 0) {
            size_t copy_len = value_len < 255 ? value_len : 255;
            memcpy(m_host_buf, headers[i].value, copy_len);
            m_host_buf[copy_len] = '\0';
        }
    }
    
    return true;
}

// 主解析函数
http_conn::HTTP_CODE http_conn::process_read() {
    const char *method, *path;
    size_t method_len, path_len;
    int minor_version;
    struct phr_header headers[MAX_HEADERS];
    size_t num_headers = MAX_HEADERS;
    
    // 使用picohttpparser解析请求
    int pret = phr_parse_request(
        m_read_buf, m_read_idx,
        &method, &method_len,
        &path, &path_len,
        &minor_version,
        headers, &num_headers,
        0
    );
    
    if (pret == -1) {
        LOG_ERROR("HTTP parse error");
        return BAD_REQUEST;
    }
    
    if (pret == -2) {
        return NO_REQUEST;  // 需要更多数据
    }
    
    // 解析成功，提取各部分信息
    if (!parse_method(method, method_len)) {
        LOG_ERROR("Unsupported method");
        return BAD_REQUEST;
    }
    
    if (!parse_url(path, path_len)) {
        LOG_ERROR("Invalid URL");
        return BAD_REQUEST;
    }
    
    parse_headers(headers, num_headers);
    
    // 检查HTTP版本
    if (minor_version != 0 && minor_version != 1) {
        LOG_ERROR("Unsupported HTTP version");
        return BAD_REQUEST;
    }
    
    // POST请求需要检查body
    if (m_method == POST && m_content_length > 0) {
        size_t body_start = pret;
        size_t total_needed = body_start + m_content_length;
        
        // body不完整
        if (m_read_idx < total_needed) {
            return NO_REQUEST;
        }
        
        // 提取body
        m_request_body = m_read_buf + body_start;
        if (total_needed < READ_BUFFER_SIZE) {
            m_read_buf[total_needed] = '\0';
        }
    }
    
    return do_request();
}

// ========== 请求处理 ==========

// 根据路由类型设置页面路径
void http_conn::route_to_page(char route_type) {
    int len = strlen(m_doc_root);
    
    switch (route_type) {
        case ROUTE_REGISTER_PAGE:  // '0' - 注册页面
            strncpy(m_real_file_path + len, "/register.html", FILENAME_LEN - len - 1);
            break;
            
        case ROUTE_LOGIN_PAGE:  // '1' - 登录页面
            strncpy(m_real_file_path + len, "/log.html", FILENAME_LEN - len - 1);
            break;
            
        case ROUTE_PICTURE:  // '5' - 图片页面
            strncpy(m_real_file_path + len, "/picture.html", FILENAME_LEN - len - 1);
            break;
            
        case ROUTE_VIDEO:  // '6' - 视频页面
            strncpy(m_real_file_path + len, "/video.html", FILENAME_LEN - len - 1);
            break;
            
        case ROUTE_FANS:  // '7' - 粉丝页面
            strncpy(m_real_file_path + len, "/fans.html", FILENAME_LEN - len - 1);
            break;
            
        default:  // 其他路径直接拼接
            strncpy(m_real_file_path + len, m_url, FILENAME_LEN - len - 1);
            break;
    }
    
    m_real_file_path[FILENAME_LEN - 1] = '\0';
}

// 处理用户登录
bool http_conn::handle_user_login(const char *name, const char *password) {
    return users.count(name) && users[name] == password;
}

// 处理用户注册
bool http_conn::handle_user_register(const char *name, const char *password) {
    // 用户已存在
    if (users.count(name)) {
        return false;
    }
    
    // 插入数据库
    char sql_insert[256];
    snprintf(sql_insert, sizeof(sql_insert),
             "INSERT INTO user(username, passwd) VALUES('%s', '%s')",
             name, password);
    
    int res = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        res = mysql_query(mysql, sql_insert);
        if (res == 0) {
            users[name] = password;
        }
    }
    
    return res == 0;
}

// 处理CGI请求（登录/注册）
http_conn::HTTP_CODE http_conn::handle_cgi_request(char route_flag) {
    int len = strlen(m_doc_root);
    
    // 构建基础路径
    strncpy(m_real_file_path, m_doc_root, FILENAME_LEN - 1);
    strncpy(m_real_file_path + len, "/", FILENAME_LEN - len - 1);
    strncat(m_real_file_path, m_url + 2, FILENAME_LEN - strlen(m_real_file_path) - 1);
    m_real_file_path[FILENAME_LEN - 1] = '\0';

    // 从POST body中提取用户名和密码
    // 格式: user=username&password=passwd
    char name[100], password[100];
    int i;
    
    // 提取用户名（跳过"user="，遇到'&'停止）
    for (i = 5; m_request_body[i] != '&' && m_request_body[i] != '\0'; i++) {
        name[i - 5] = m_request_body[i];
    }
    name[i - 5] = '\0';

    // 提取密码（跳过"&password="）
    int j = 0;
    for (i = i + 10; m_request_body[i] != '\0'; i++, j++) {
        password[j] = m_request_body[i];
    }
    password[j] = '\0';

    // 处理注册
    if (route_flag == ROUTE_REGISTER_CHECK) {  // '3'
        if (handle_user_register(name, password)) {
            strcpy(m_url, "/log.html");
        } else {
            strcpy(m_url, "/registerError.html");
        }
    }
    // 处理登录
    else if (route_flag == ROUTE_LOGIN_CHECK) {  // '2'
        if (handle_user_login(name, password)) {
            strcpy(m_url, "/welcome.html");
        } else {
            strcpy(m_url, "/logError.html");
        }
    }
    
    return FILE_REQUEST;
}

// 主请求处理函数
http_conn::HTTP_CODE http_conn::do_request() {
    strcpy(m_real_file_path, m_doc_root);
    int len = strlen(m_doc_root);
    
    const char *p = strrchr(m_url, '/');
    if (!p) {
        return BAD_REQUEST;
    }
    
    char route_char = *(p + 1);
    
    // 处理POST表单提交（登录/注册）
    if (m_is_post_form && (route_char == ROUTE_LOGIN_CHECK || route_char == ROUTE_REGISTER_CHECK)) {
        return handle_cgi_request(route_char);
    }
    
    // 路由到对应页面
    route_to_page(route_char);

    // 检查文件是否存在
    if (stat(m_real_file_path, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    // 检查文件权限
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    // 检查是否是目录
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }
    
    // 根据文件大小选择传输方式
    if (m_file_stat.st_size < SENDFILE_THRESHOLD) {
        // 小文件：使用mmap
        int fd = open(m_real_file_path, O_RDONLY);
        m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        m_use_sendfile = false;
    } else {
        // 大文件：使用sendfile
        m_file_fd = open(m_real_file_path, O_RDONLY);
        m_sendfile_offset = 0;
        m_sendfile_remaining = m_file_stat.st_size;
        m_file_address = nullptr;
        m_use_sendfile = true;
    }
    
    return FILE_REQUEST;
}

// ========== 响应构建 ==========

// 添加响应内容到写缓冲区（可变参数）
bool http_conn::add_response(const char *format, ...) {
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    
    va_list arg_list;
    va_start(arg_list, format);
    
    int len = vsnprintf(m_write_buf + m_write_idx, 
                       WRITE_BUFFER_SIZE - m_write_idx - 1, 
                       format, arg_list);
    
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        va_end(arg_list);
        return false;
    }
    
    m_write_idx += len;
    va_end(arg_list);
    
    return true;
}

// 添加状态行
bool http_conn::add_status_line(int status, const char *title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
// 添加Content-Length头
bool http_conn::add_content_length(int content_length) {
    return add_response("Content-Length:%d\r\n", content_length);
}
// 添加Connection头
bool http_conn::add_connection_header() {
    return add_response("Connection:%s\r\n", 
                       m_keep_alive ? "keep-alive" : "close");
}
// 添加空行
bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}
// 添加所有响应头
bool http_conn::add_headers(int content_length) {
    return (add_content_length(content_length) && 
            add_connection_header() && 
            add_blank_line());
}
// 添加响应体内容
bool http_conn::add_content(const char *content) {
    return add_response("%s", content);
}

// 根据处理结果构建响应
bool http_conn::process_write(HTTP_CODE ret) {
    switch(ret) {
        case INTERNAL_ERROR: {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form))
                return false;
            break;
        }
        
        case BAD_REQUEST: {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form))
                return false;
            break;
        }
        
        case FORBIDDEN_REQUEST: {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
                return false;
            break;
        }
        
        case FILE_REQUEST: {
            add_status_line(200, ok_200_title);
            
            if (m_file_stat.st_size != 0) {
                add_headers(m_file_stat.st_size);
                
                // 设置响应iovec
                m_response_iov[0].iov_base = m_write_buf;
                m_response_iov[0].iov_len = m_write_idx;
                m_response_iov[1].iov_base = m_file_address;
                m_response_iov[1].iov_len = m_file_stat.st_size;
                m_response_iov_count = 2;
                m_bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else {
                // 空文件
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
            break;
        }
        
        default:
            return false;
    }
    
    // 非FILE_REQUEST的情况，只发送写缓冲区的内容
    m_response_iov[0].iov_base = m_write_buf;
    m_response_iov[0].iov_len = m_write_idx;
    m_response_iov_count = 1;
    m_bytes_to_send = m_write_idx;
    return true;
}

// 主处理函数
void http_conn::process() {
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    
    if (read_ret == NO_REQUEST) {
        // 请求不完整，继续监听读事件
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_trigger_mode);
        return;
    }
    
    // 构建HTTP响应
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
        return;
    }
    
    // 注册写事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_trigger_mode);
}


// ========== 数据发送 ==========

// 主写入函数
bool http_conn::write() {
    if (m_use_sendfile) {
        return write_with_sendfile();
    }
    return write_with_mmap();
}

// 使用mmap方式发送数据
bool http_conn::write_with_mmap() {
    int tmp = 0;
    
    // 没有数据需要发送
    if (m_bytes_to_send == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_trigger_mode);
        init();
        return true;
    }
    
    while (true) {
        // 使用writev发送多个不连续的缓冲区
        tmp = writev(m_sockfd, m_response_iov, m_response_iov_count);

        if (tmp < 0) {
            // 发送缓冲区已满
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_trigger_mode);
                return true;
            }
            unmap();
            return false;
        }

        m_bytes_have_send += tmp;
        m_bytes_to_send -= tmp;
        
        // 第一个iovec（响应头）已发送完
        if (m_bytes_have_send >= m_response_iov[0].iov_len) {
            m_response_iov[0].iov_len = 0;
            m_response_iov[1].iov_base = m_file_address + (m_bytes_have_send - m_write_idx);
            m_response_iov[1].iov_len = m_bytes_to_send;
        }
        // 第一个iovec还没发送完
        else {
            m_response_iov[0].iov_base = m_write_buf + m_bytes_have_send;
            m_response_iov[0].iov_len = m_response_iov[0].iov_len - m_bytes_have_send;
        }

        // 所有数据已发送完毕
        if (m_bytes_to_send <= 0) {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_trigger_mode);

            // 保持连接
            if (m_keep_alive) {
                init();
                return true;
            }
            // 关闭连接
            else {
                return false;
            }
        }
    }
}

// 使用sendfile方式发送数据
bool http_conn::write_with_sendfile() {
    // 步骤1: 先发送响应头
    while (m_response_iov[0].iov_len > 0) {
        ssize_t ret = writev(m_sockfd, m_response_iov, 1);
        if (ret < 0) {
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_trigger_mode);
                return true;
            }
            return false;
        }
        m_bytes_have_send += ret;
        m_response_iov[0].iov_len -= ret;
        m_response_iov[0].iov_base = m_write_buf + m_bytes_have_send;
    }

    // 步骤2: 响应头发送完成，使用sendfile发送文件内容
    if (m_sendfile_remaining > 0) {
        ssize_t ret = sendfile(m_sockfd, m_file_fd, &m_sendfile_offset, m_sendfile_remaining);
        if (ret < 0) {
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_trigger_mode);
                return true;
            }
            return false;
        }
        m_sendfile_remaining -= ret;
    }

    // 步骤3: 如果文件内容未完全发送，继续等待可写事件
    if (m_sendfile_remaining > 0) {
        modfd(m_epollfd, m_sockfd, EPOLLOUT, m_trigger_mode);
        return true;
    }

    // 步骤4: 所有数据发送完毕
    unmap();
    modfd(m_epollfd, m_sockfd, EPOLLIN, m_trigger_mode);

    // 保持连接
    if (m_keep_alive) {
        init();
        return true;
    }
    // 关闭连接
    return false;
}