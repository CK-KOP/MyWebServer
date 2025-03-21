
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

locker m_lock;
map<string, string>users;

void http_conn::initmysql_result(connection_pool *connPool){
    // 先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

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
        string temp1(row[0]);
        string temp2(row[1]);
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
                     int close_log, string user, string passwd, string sqlname){
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
    m_checked_idx = 0;
    m_start_line = 0;
    m_write_idx = 0;
    
    m_check_state = CHECK_STATE_REQUESTLINE;   // 默认检查请求行状态
    m_method = GET;				// 默认请求方法为 GET
    
    m_url = 0;
    m_version = 0;
    m_host = 0;
    m_content_length = 0;
    m_linger = false;			        // 默认不使用长连接
    
    cgi = 0;        // 默认不使用 CGI
    bytes_to_send = 0;
    bytes_have_send = 0;
    
    mysql = NULL;
    m_state = 0;    // 读写
    timer_flag = 0; // 是否需要定时调整
    improv = 0;     // 是否被尝试读入或写入过
    
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);    // 清空读取缓冲区
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);  // 清空写入缓冲区	
    memset(m_real_file, '\0', FILENAME_LEN);	     // 清空真实文件路径
}


// 循环读取客户数据，直到无数据可读或对方关闭连接
// 非阻塞ET工作模式下，需要一次性将数据读完
// 从客户端socket读取数据到缓冲区 
bool http_conn::read_once(){
    if (m_read_idx >= READ_BUFFER_SIZE)
        return false;
    int bytes_read = 0;

    // LT读取数据
    if (0 == m_TRIGMode){
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read; 
        if (bytes_read <= 0)
            return false;
        //printf("LT读入数据: \n%s\n", m_read_buf);
        return true;
    }
    // ET读取数据
    else{
        while (true){
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1){
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
                return false;
            m_read_idx += bytes_read;
        }
        //printf("RT读入数据: \n%s\n", m_read_buf);
        return true;
    }
    
}


// 从状态机，用于分析出一行内容
// 返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line(){
    char tmp;
    for (; m_checked_idx < m_read_idx; m_checked_idx++){
        tmp = m_read_buf[m_checked_idx];
        // 假如是'\r' 如果后面紧跟'\n'则OK; '\n'还没被读进来则OPEN; 否则BAD
        if (tmp == '\r'){
            if (m_checked_idx + 1 == m_read_idx)
                return LINE_OPEN;
            else if(m_read_buf[m_checked_idx + 1] == '\n'){
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        // 同理检查前面是否为'\r' 是则OK，否则BAD
        else if(tmp == '\n'){
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r'){
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}



// 解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text){
    m_url = strpbrk(text, " \t");
    if (!m_url)
        return BAD_REQUEST;
    *m_url++ = '\0';    // 隔断请求方法
    
    char *method = text; //获取请求方法
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0){
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    
    m_url += strspn(m_url, " \t"); // 获取URL
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';

    m_version += strspn(m_version, " \t"); // 获取HTTP版本
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    
    if (strncasecmp(m_url, "http://", 7) == 0){  // 处理URL格式
        m_url += 7;
        m_url = strchr(m_url, '/'); // 跳过这些前缀，并找到后面的第一个'/'字符
    }
    if (strncasecmp(m_url, "https://", 8) == 0){
        m_url += 8;
        m_url = strchr(m_url, '/'); // 跳过这些前缀，并找到后面的第一个'/'字符
    }

    if (!m_url || m_url[0] != '/')  // URL有效性检查
        return BAD_REQUEST;
    
    if (strlen(m_url) == 1)         // 当url为/时，显示判断界面
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER; // 状态机进入头部解析状态
    return NO_REQUEST;                  // 请求尚未解析完
}

// 解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text){
    // 遇到空行，表示头部解析完毕
    if (text[0] == '\0'){
        // content_length 不为0，还有信息需要解析
        if (m_content_length != 0){
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0){
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
            m_linger = true;
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0){
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0){
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else{
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

// 判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text){
    if (m_read_idx >= m_content_length + m_checked_idx){
        text[m_content_length] = '\0';
        // POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}



// 从读缓冲区解析整体HTTP请求
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    // 用于判断是否尝试解析过请求内容，解析过后 无论对错都会退出
    // 假如正确说明HTTP头全部被解析完了，假如错误说明还没读入完也要返回NO_REQUEST
    int flag = 1; 
    
    // 假如是刚尝试解析请求内容或者在缓冲区找到了一个完整行即可继续解析内容
    // 因为POST里的请求内容是没有换行符的 所以不能用后者parse_line()判断是否找到了一个完整行
    while ((m_check_state == CHECK_STATE_CONTENT && flag) || (line_status = parse_line()) == LINE_OK){
        text = get_line();
        m_start_line = m_checked_idx; // 这行已经完整写入缓存区，更新下一个轮到解析行的位置
        LOG_INFO("%s", text);
        switch (m_check_state){
        case CHECK_STATE_REQUESTLINE:{
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST){
                //printf("--------------------------------------------------------\n");
                return BAD_REQUEST;
            }
            break;
        }
        case CHECK_STATE_HEADER:{
            ret = parse_headers(text);
            if (ret == BAD_REQUEST){
                //printf("/////////////////////////////////////////////////////////\n");
                return BAD_REQUEST;
            }
            if (ret == GET_REQUEST)
                return do_request();
            break;
        }
        case CHECK_STATE_CONTENT:{
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            flag = 0;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }   
    }
    return NO_REQUEST;
}


http_conn::HTTP_CODE http_conn::do_request(){
    //printf("开始根据解析结果获取要返回的文件内容\n");
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    const char *p = strrchr(m_url, '/');
    
    // 处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')){
        // 根据标志判断是登陆检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        // 将用户名和密码提取出来
        // user=123&password=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; i++)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; i++, j++)
            password[j] = m_string[i];
        password[j] = '\0';

        // 注册
        if (*(p + 1) == '3'){
            // 如果是注册，先检测数据库中是否有重名的
            // 没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (!users.count(name)){
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();
            
                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        // 如果是登录，直接判断
        // 若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2'){
            if (users.count(name) && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }
    
    if (* (p + 1) == '0'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (* (p + 1) == '1'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (* (p + 1) == '5'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (* (p + 1) == '6'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if(* (p + 1) == '7'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    // 使用stat函数检查请求的文件是否存在
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    // 检查文件是否有读取权限
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    // 如果请求的是目录而非文件
    if (S_ISDIR(m_file_stat.st_mode)){
        //printf("||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||\n");
        return BAD_REQUEST;
}
    // 内存映射
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}


// 内存映射的解除
void http_conn::unmap(){
    if (m_file_address){
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
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

    LOG_INFO("request: %s", m_write_buf);
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
bool http_conn::add_content_type(){
    return add_response("Content-Type:%s\r\n", "text/html");
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
}

// 从写缓存区传输到客户端socket
bool http_conn::write(){
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


