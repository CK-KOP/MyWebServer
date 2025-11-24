
#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

// 定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

std::mutex m_mutex;
std::map<std::string, std::string>users;

void http_conn::initmysql_result(connection_pool *connPool){
    // 先从连接池中取一个连接
    ConnectionGuard connGuard(*connPool);
    MYSQL* mysql = connGuard.get();

    // 在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));

    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行，将对应的用户名和密码，存入map中
    while(MYSQL_ROW row = mysql_fetch_row(result)){
        std::string temp1(row[0]);
        std::string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
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

// 从内核时间表删除描述符
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode){
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

// 关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close){
    if (real_close && (m_sockfd != -1)){
        //printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, std::string user, std::string passwd, std::string sqlname){
    m_sockfd = sockfd;                          // 保存客户端的socket描述符
    m_address = addr;                           // 保存客户端地址
    addfd(m_epollfd, sockfd, true, m_TRIGMode); // 将客户端socket添加到epoll事件表中
    m_user_count++;                             // 用户数量增加
    doc_root = root;                            // 网站根目录
    m_TRIGMode = TRIGMode;                      // 触发模式
    m_close_log = close_log;                    // 日志开关
    strcpy(sql_user, user.c_str());             // 保存数据库用户名
    strcpy(sql_passwd, passwd.c_str());         // 保存数据库密码
    strcpy(sql_name, sqlname.c_str());          // 保存数据库名称

    init();                                     // 初始化其他成员变量
}

// 初始化新接受的连接
// check_state默认为分析请求行状态
void http_conn::init(){
    m_read_idx = 0;
    m_write_idx = 0;
    
    m_method = GET;
    m_url = m_url_buf;
    m_host = m_host_buf;
    m_content_length = 0;
    m_linger = false;
    
    cgi = 0;
    bytes_to_send = 0;
    bytes_have_send = 0;
    
    mysql = NULL;
    m_state = 0;
    
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
    memset(m_url_buf, '\0', FILENAME_LEN);
    memset(m_host_buf, '\0', 256);
}


// 循环读取客户数据，直到无数据可读或对方关闭连接
// 非阻塞ET工作模式下，需要一次性将数据读完
// 从客户端socket读取数据到缓冲区 
int http_conn::read_once(){
    if (m_read_idx >= READ_BUFFER_SIZE)
        return false;
    int bytes_read = 0;

    // LT读取数据
    if (0 == m_TRIGMode){
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read; 
        if (bytes_read < 0)
            return -1;
        else if (bytes_read == 0)
            return 0;
        //printf("LT读入数据: \n%s\n", m_read_buf);
        return 1;
    }
    // ET读取数据
    else{
        while (true){
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1){
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return -1;
            }
            else if (bytes_read == 0)
                return 0;
            m_read_idx += bytes_read;
        }
        //printf("RT读入数据: \n%s\n", m_read_buf);
        return 1;
    }
}

// ============ 解析方法 ============
bool http_conn::parse_method(const char *method, size_t len) {
    // 用简单的比较替代 strcasecmp，更快
    if (len == 3) {
        if (method[0] == 'G' && method[1] == 'E' && method[2] == 'T') {
            m_method = GET;
            cgi = 0;
            return true;
        }
        if (method[0] == 'g' && method[1] == 'e' && method[2] == 't') {
            m_method = GET;
            cgi = 0;
            return true;
        }
    }
    if (len == 4) {
        if ((method[0] == 'P' || method[0] == 'p') &&
            (method[1] == 'O' || method[1] == 'o') &&
            (method[2] == 'S' || method[2] == 's') &&
            (method[3] == 'T' || method[3] == 't')) {
            m_method = POST;
            cgi = 1;
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

// ============ 解析URL ============
bool http_conn::parse_url(const char *path, size_t len) {
    if (len == 0 || len >= FILENAME_LEN - 20)  // 留点空间给后面可能的拼接
        return false;
    
    // picohttpparser 已经帮我们处理好了，直接是路径部分
    memcpy(m_url_buf, path, len);
    m_url_buf[len] = '\0';
    
    // 处理根路径
    if (len == 1 && m_url_buf[0] == '/') {
        strcat(m_url_buf, "judge.html");
    }
    
    return true;
}

// ============ 解析Headers ============
bool http_conn::parse_headers(struct phr_header *headers, size_t num) {
    m_content_length = 0;
    m_linger = false;
    m_host_buf[0] = '\0';
    
    for (size_t i = 0; i < num; i++) {
        size_t name_len = headers[i].name_len;
        size_t value_len = headers[i].value_len;
        
        // 优化：先比较长度再比较内容
        if (name_len == 10) {  // "Connection"
            if (strncasecmp(headers[i].name, "Connection", 10) == 0) {
                if (value_len == 10 && 
                    strncasecmp(headers[i].value, "keep-alive", 10) == 0) {
                    m_linger = true;
                }
            }
        }
        else if (name_len == 14) {  // "Content-Length"
            if (strncasecmp(headers[i].name, "Content-Length", 14) == 0) {
                // 手动转换（value不是null结尾的）
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
        }
        else if (name_len == 4) {  // "Host"
            if (strncasecmp(headers[i].name, "Host", 4) == 0) {
                size_t copy_len = value_len < 255 ? value_len : 255;
                memcpy(m_host_buf, headers[i].value, copy_len);
                m_host_buf[copy_len] = '\0';
            }
        }
    }
    return true;
}


// ============ 主解析函数 ============
http_conn::HTTP_CODE http_conn::process_read() {
    const char *method, *path;
    size_t method_len, path_len;
    int minor_version;
    struct phr_header headers[MAX_HEADERS];
    size_t num_headers = MAX_HEADERS;
    
    // 调用 picohttpparser
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
    
    // 解析成功，pret 是 header 结束位置
    
    // 提取方法
    if (!parse_method(method, method_len)) {
        LOG_ERROR("Unsupported method");
        return BAD_REQUEST;
    }
    
    // 提取 URL
    if (!parse_url(path, path_len)) {
        LOG_ERROR("Invalid URL");
        return BAD_REQUEST;
    }
    
    // 提取 headers
    parse_headers(headers, num_headers);
    
    // 检查 HTTP 版本 (minor_version: 0->1.0, 1->1.1)
    if (minor_version != 1 && minor_version != 0) {
        LOG_ERROR("Unsupported HTTP version");
        return BAD_REQUEST;
    }
    
    // POST 请求需要检查 body
    if (m_method == POST && m_content_length > 0) {
        size_t body_start = pret;
        size_t total_needed = body_start + m_content_length;
        
        // body 不完整
        if (m_read_idx < total_needed) {
            return NO_REQUEST;
        }
        
        // 提取 body
        m_string = m_read_buf + body_start;
        // 确保字符串结尾
        if (total_needed < READ_BUFFER_SIZE) {
            m_read_buf[total_needed] = '\0';
        }
    }
    
    return do_request();
}


http_conn::HTTP_CODE http_conn::do_request(){
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    const char *p = strrchr(m_url, '/');
    
    if (!p) {
        return BAD_REQUEST;
    }
    
    // 处理cgi (登录/注册提交)
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')){
        char flag = *(p + 1);
        
        // 优化: 直接操作字符串,消除malloc
        strncpy(m_real_file + len, "/", FILENAME_LEN - len - 1);
        strncat(m_real_file, m_url + 2, FILENAME_LEN - strlen(m_real_file) - 1);
        m_real_file[FILENAME_LEN - 1] = '\0';

        // 提取用户名和密码 (user=123&password=123)
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; i++)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; i++, j++)
            password[j] = m_string[i];
        password[j] = '\0';

        if (flag == '3'){  // 注册
            // 优化: 使用snprintf替代多次strcat
            char sql_insert[256];
            snprintf(sql_insert, sizeof(sql_insert),
                     "INSERT INTO user(username, passwd) VALUES('%s', '%s')",
                     name, password);

            if (!users.count(name)){
                int res = 0;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    res = mysql_query(mysql, sql_insert);
                    users[name] = password;
                }
                strcpy(m_url, res == 0 ? "/log.html" : "/registerError.html");
            }
            else {
                strcpy(m_url, "/registerError.html");
            }
        }
        else if (flag == '2'){  // 登录
            strcpy(m_url, (users.count(name) && users[name] == password) 
                          ? "/welcome.html" : "/logError.html");
        }
    }
    
    // 优化: 使用switch替代if-else链
    switch (*(p + 1)) {
        case '0':  // 注册页面
            strncpy(m_real_file + len, "/register.html", FILENAME_LEN - len - 1);
            break;
            
        case '1':  // 登录页面
            strncpy(m_real_file + len, "/log.html", FILENAME_LEN - len - 1);
            break;
            
        case '5':  // 图片页面
            strncpy(m_real_file + len, "/picture.html", FILENAME_LEN - len - 1);
            break;
            
        case '6':  // 视频页面
            strncpy(m_real_file + len, "/video.html", FILENAME_LEN - len - 1);
            break;
            
        case '7':  // 粉丝页面
            strncpy(m_real_file + len, "/fans.html", FILENAME_LEN - len - 1);
            break;
            
        default:  // 其他路径直接拼接
            strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
            break;
    }
    
    m_real_file[FILENAME_LEN - 1] = '\0';

    // 检查文件是否存在
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    // 检查文件权限
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    // 检查是否是目录
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;
    
    // mmap 和 sendfile 区分处理
    if (m_file_stat.st_size < SENDFILE_THRESHOLD) {
        // 小文件：使用 mmap + writev
        int fd = open(m_real_file, O_RDONLY);
        m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        m_use_sendfile = false;
    } else {
        // 大文件：使用 sendfile
        m_file_fd = open(m_real_file, O_RDONLY);
        m_sf_offset = 0;
        m_sf_remaining = m_file_stat.st_size;
        m_file_address = nullptr;      // 告诉 write_writev 不使用 mmap
        m_use_sendfile = true;
    }
    
    return FILE_REQUEST;
}


// 内存映射的解除或者file_fd关闭
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


// 添加response到写缓冲区的模板
bool http_conn::add_response(const char *format, ...){
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    // 初始化 arg_list，并使得它指向 format 后面的变长参数
    va_list arg_list;
    va_start(arg_list, format);
    
    // 格式化字符串并写入缓冲区,返回实际写入的字节数
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - m_write_idx - 1, format, arg_list);
    
    // 假如缓冲区溢出，清理资源退出 
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)){
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    return true;
}

// response状态行
bool http_conn::add_status_line(int status, const char *title){
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// response响应头
bool http_conn::add_headers(int content_len){
    return (add_content_length(content_len) && add_linger() && add_blank_line());    
}
bool http_conn::add_content_length(int content_length){
    return add_response("Content-Length:%d\r\n", content_length);
}
bool http_conn::add_linger(){
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line(){
    return add_response("%s", "\r\n");
}

// response响应正文
bool http_conn::add_content(const char *content){
    return add_response("%s", content);
}

// 写入到写缓冲区并初始化m_iv参数
bool http_conn::process_write(HTTP_CODE ret){
    switch(ret){
    case INTERNAL_ERROR:{
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:{
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:{
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:{
        add_status_line(200, ok_200_title);
        // 假如文件正常，则初始化m_iv参数
        // m_iv[0]指向写缓冲区为状态行和响应头
        // m_iv[1]指向mmap映射的文件地址即响应正文
        if (m_file_stat.st_size != 0){
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else{
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
    // 没有进入FILE_REQUEST，则不额外传输文件只传输写缓存区里报文响应
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}



// 将读缓存区的数据读取出来并解析将返回数据写入到写缓冲区
void http_conn::process(){
    //printf("进入process()\n");
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST){
        //printf("没解析完\n");
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    //printf("解析完了\n");
    bool write_ret = process_write(read_ret);
    if (!write_ret)
        close_conn();
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);

    // 后期可以考虑添加一个如Nginx的access日志，形式如下
    // 127.0.0.1 - - [10/Nov/2025:10:30:00 +0800] "GET /index.html HTTP/1.1" 200 1024
}


// 从写缓存区传输到客户端socket
bool http_conn::write() {
    // 大文件 → sendfile
    if (m_use_sendfile) {
        return write_sendfile();
    }
    // 小文件 → mmap + writev
    return write_mmap();
}


bool http_conn::write_mmap(){
    int tmp = 0;
    
    // 判断是否需要写数据
    if (bytes_to_send == 0){
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }
    
    while(true){    
        // 使用m_iv传输多块不连续的缓冲区数据
        tmp = writev(m_sockfd, m_iv, m_iv_count);

        if (tmp < 0){ // 出现异常
           if (errno == EAGAIN){ // 发送缓冲区已满，再次置为EPOLLOUT 等待下一次事件通知
            modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
            return true;
           }
           unmap();
        }

        bytes_have_send += tmp;
        bytes_to_send -= tmp;
        // 请求头部分已经发送完
        if (bytes_have_send >= m_iv[0].iov_len){
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        // 请求头部分还没发送完
        else{
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        // 数据传输完毕，进入监听读取请求模式
        // 并初始化为监听请求的状态
        if (bytes_to_send <= 0){
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger){
                init();
                return true;
            }
            else
                return false;
        }
    }
}


bool http_conn::write_sendfile() {
    // ① 先发 header (writev)
    while (m_iv[0].iov_len > 0) {
        ssize_t ret = writev(m_sockfd, m_iv, 1);
        if (ret < 0) {
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            return false;
        }
        bytes_have_send += ret;
        m_iv[0].iov_len -= ret;
        m_iv[0].iov_base = m_write_buf + bytes_have_send;
    }

    // ② header 发完 -> sendfile body
    if (m_sf_remaining > 0) {
        ssize_t ret = sendfile(m_sockfd, m_file_fd, &m_sf_offset, m_sf_remaining);
        if (ret < 0) {
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            return false;
        }
        m_sf_remaining -= ret;
    }

    // ③ 若 body 未完全发完，继续等待可写
    if (m_sf_remaining > 0) {
        modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
        return true;
    }

    // ④ 全部发送完毕
    unmap();
    modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

    if (m_linger) {
        init();
        return true;    // 长连接
    }
    return false;       // 非长连接 -> 关闭
}

