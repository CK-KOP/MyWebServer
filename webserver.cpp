#include "webserver.h"

WebServer::WebServer(){
    // root文件夹路径，资源目录
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);
}

WebServer::~WebServer(){
    close(m_epollfd);
    close(m_listenfd);
    close(m_timerfd);
    // unique_ptr会自动清理m_users和m_clients中的对象
}

void WebServer::init(int port , std::string user, std::string passWord, std::string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int close_log){

    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_sql_num = sql_num;
    m_close_log = close_log;
}

// 根据传入的TRIGMode 给listenfd和connfd配置LT/RT
void WebServer::trig_mode(){
    // LT + LT
    if (0 == m_TRIGMode){
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    // LT + ET
    else if (1 == m_TRIGMode){
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;       
    }
    // ET + LT
    else if (2 == m_TRIGMode){
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;  
    }
    // ET + ET
    else if (3 == m_TRIGMode){
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;   
    }
}

// 初始化日志系统
void WebServer::log_write(){
    // 不关闭日志则运行
    if (0 == m_close_log){
        // 异步日志，创建日志队列 + 独立日志线程
        // 五个参数，最后一个代表是否使用异步日志
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 1);    
        // 同步日志，每次调用 write_log() 直接写文件
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

void WebServer::sql_pool(){
    // 初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    // 使用静态方法初始化数据库用户数据
    http_conn::init_database_users(m_connPool);
}


// 服务器启动
void WebServer::eventListen(){
    // 网络编程基础步骤
    // 创建TCP套接字
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd > 0);

    // 立即关闭连接，可能丢弃未发送数据
    if (0 == m_OPT_LINGER){
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER,  &tmp, sizeof(tmp));
    }
    // 延迟关闭，等待数据发送完成或超时(1秒)
    else if (1 == m_OPT_LINGER){
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    
    // 初始化服务器地址结构
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    // 初始化服务器地址结构
    // SO_REUSEADDR：重启服务器时，可以快速绑定同一个端口
    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    
    // 将socket绑定到指定地址和端口 
    // 开始监听连接请求，队列长度为65535
    int ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 65535);
    assert(ret >= 0);
    
    // 初始化定时器（时间轮）
    m_timer_wheel.set_timeslot(TIMESLOT);

    // epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];  //好像完全没有用到
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    // 监听套接字添加到epoll
    Utils::addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    // http_conn::m_epollfd = m_epollfd;  // 移除，改为成员变量
    
    // 创建 timerfd 替代 SIGALRM
    m_timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    assert(m_timerfd > 0);

    // 设置周期定时器（等价原 TIMESLOT 秒触发一次）
    itimerspec new_value{};
    new_value.it_value.tv_sec = TIMESLOT;      // 第一次触发时间
    new_value.it_interval.tv_sec = TIMESLOT;   // 周期
    timerfd_settime(m_timerfd, 0, &new_value, nullptr);

    // 将 timerfd 添加到 epoll
    Utils::addfd(m_epollfd, m_timerfd, false, 0);

    // 忽略SIGPIPE信号，防止在写入已关闭的socket时服务器崩溃
    Utils::addsig(SIGPIPE, SIG_IGN);
}

// 初始化http连接类与定时器，并与当前要连接的客户端绑定
void WebServer::create_timer(int connfd, struct sockaddr_in client_address){
    // 创建http连接对象
    auto http_conn_ptr = std::make_unique<http_conn>();
    http_conn_ptr->init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName, m_epollfd);

    // 增加连接计数
    m_user_count++;

    // 创建client_data对象
    auto client_data_ptr = std::make_unique<client_data>();
    client_data_ptr->address = client_address;
    client_data_ptr->sockfd = connfd;

    // 创建定时器
    util_timer *timer = new util_timer;
    timer->user_data = client_data_ptr.get();  // 使用裸指针，但不拥有所有权

    // 使用lambda创建回调函数，可以捕获WebServer的this指针
    timer->cb_func = [this, connfd]() {
        this->close_connection_by_timer(connfd);
    };

    time_t cur = time(NULL);
    // 使用随机超时时间（15-25秒之间）
    int random_timeout = m_timer_wheel.get_random_timeout();
    timer->expire = cur + random_timeout;
    timer->last_active = cur;

    client_data_ptr->timer = timer;
    m_timer_wheel.add_timer(timer);

    // 将对象添加到map中
    m_users[connfd] = std::move(http_conn_ptr);
    m_clients[connfd] = std::move(client_data_ptr);
}

// 若有数据传输，不直接修改定时器容器，而是更新该定时器的最新活跃时间
void WebServer::adjust_timer(util_timer* timer) {
    time_t cur = time(NULL);
    // adjust_timer 内部会自动使用随机超时
    m_timer_wheel.adjust_timer(timer, cur);
}

// 关闭连接（包含定时器超时、错误等情况）
void WebServer::close_connection(util_timer *timer, int sockfd){
    if (!timer) {
        LOG_WARN("Trying to delete null timer: fd=%d", sockfd);
        return;
    }

    // 从epoll中删除文件描述符
    epoll_ctl(m_epollfd, EPOLL_CTL_DEL, sockfd, 0);

    // 关闭套接字
    close(sockfd);

    // 从时间轮中删除定时器
    m_timer_wheel.del_timer(timer);

    // 从map中移除，自动删除对象
    auto client_it = m_clients.find(sockfd);
    if (client_it != m_clients.end()) {
        m_clients.erase(client_it);  // 自动删除client_data对象
    }

    // 从http连接map中移除
    auto user_it = m_users.find(sockfd);
    if (user_it != m_users.end()) {
        m_users.erase(user_it);  // 自动删除http_conn对象
    }

    // 减少连接计数
    m_user_count--;

    LOG_DEBUG("close fd %d", sockfd);
}

// 定时器回调调用版本，只负责资源清理，不删除定时器（定时器由时间轮自己删除）
void WebServer::close_connection_by_timer(int sockfd) {
    // 从epoll中删除文件描述符
    epoll_ctl(m_epollfd, EPOLL_CTL_DEL, sockfd, 0);
    // 关闭套接字
    close(sockfd);

    // 从map中移除，自动删除对象
    auto client_it = m_clients.find(sockfd);
    if (client_it != m_clients.end()) {
        m_clients.erase(client_it);  // 自动删除client_data对象
    }

    // 从http连接map中移除
    auto user_it = m_users.find(sockfd);
    if (user_it != m_users.end()) {
        m_users.erase(user_it);  // 自动删除http_conn对象
    }

    // 减少连接计数
    m_user_count--;

    LOG_DEBUG("Timer callback closed fd %d", sockfd);
}

// 处理新客户端连接
bool WebServer::dealclientdata(){
    struct sockaddr_in client_address;
    socklen_t client_addrlenth = sizeof(client_address);
    
    if (0 == m_LISTENTrigmode){  // LT模式
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlenth);
        if (connfd < 0){
            LOG_ERROR("LT accept error: %d", errno);
            LOG_ERROR("accept error: errno=%d (%s)", errno, strerror(errno));
            return false;
        }
        if (m_user_count >= MAX_FD){
            Utils::show_error(connfd, "Internal server busy");
            return false;
        }
        create_timer(connfd, client_address);
    }
    else{  // ET模式
        while (1){
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlenth);
            if (connfd < 0){
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;  // accept已经读取不到连接了，正常结束
                LOG_ERROR("ET accept error: %d", errno);
                LOG_ERROR("accept error: errno=%d (%s)", errno, strerror(errno));
                return false;
            }
            if (m_user_count >= MAX_FD){
                Utils::show_error(connfd, "Internal server busy");
                continue;  // 继续接受其他连接
            }
            create_timer(connfd, client_address);
        }
    }
    return true;
}


void WebServer::dealwithread(int sockfd){
    auto client_it = m_clients.find(sockfd);
    auto user_it = m_users.find(sockfd);

    if (client_it == m_clients.end() || user_it == m_users.end()) {
        LOG_WARN("Connection %d not found in maps", sockfd);
        return;
    }

    util_timer *timer = client_it->second->timer;

    // 日志记录ip地址
    LOG_DEBUG("deal with the client(%s)", inet_ntoa(user_it->second->get_address()->sin_addr));

    int flag = user_it->second->read_once();
    if (flag > 0) {
        // 成功读取到数据
        ConnectionGuard connGuard(*m_connPool);
        user_it->second->mysql = connGuard.get();
        http_conn::PROCESS_RESULT result = user_it->second->process();

        // 根据process返回值处理
        if (result == http_conn::PROCESS_ERROR) {
            // 处理失败，关闭连接
            close_connection(timer, sockfd);
            return;
        }
        else if (result == http_conn::PROCESS_CONTINUE) {
            // 请求不完整，继续监听读事件
            Utils::modfd(m_epollfd, sockfd, EPOLLIN, m_CONNTrigmode);
        }
        else if (result == http_conn::PROCESS_OK) {
            // 处理成功，注册写事件
            Utils::modfd(m_epollfd, sockfd, EPOLLOUT, m_CONNTrigmode);
        }

        if (timer){
            adjust_timer(timer);
        }
    }
    else if(flag < 0){
        // 读取错误，关闭连接
        close_connection(timer, sockfd);
    }
    else{
        // flag == 0，对端关闭了连接(FIN)
        // 但是我们可能还需要发送响应数据，先处理已读取的数据
        ConnectionGuard connGuard(*m_connPool);
        user_it->second->mysql = connGuard.get();
        http_conn::PROCESS_RESULT result = user_it->second->process();

        if (result == http_conn::PROCESS_ERROR) {
            // 处理失败，关闭连接
            close_connection(timer, sockfd);
            return;
        }
        else if (result == http_conn::PROCESS_CONTINUE) {
            // 请求不完整但连接已关闭，直接关闭连接
            LOG_DEBUG("Incomplete request but client closed: fd=%d", sockfd);
            close_connection(timer, sockfd);
        }
        else if (result == http_conn::PROCESS_OK) {
            // 处理成功，发送响应后关闭连接
            LOG_DEBUG("Client closed, sending final response: fd=%d", sockfd);
            user_it->second->m_peer_closed = true;  // 标记对端已关闭
            Utils::modfd(m_epollfd, sockfd, EPOLLOUT, m_CONNTrigmode);

            if (timer){
                adjust_timer(timer);
            }
        }
    }
}

void WebServer::dealwithwrite(int sockfd){
    auto client_it = m_clients.find(sockfd);
    auto user_it = m_users.find(sockfd);

    if (client_it == m_clients.end() || user_it == m_users.end()) {
        LOG_WARN("Connection %d not found in maps", sockfd);
        return;
    }

    util_timer *timer = client_it->second->timer;

    LOG_DEBUG("send data to the client(%s)", inet_ntoa(user_it->second->get_address()->sin_addr));
    int write_result = user_it->second->write();

    if (write_result == 1) {
        // 写入完成，检查是否需要关闭连接
        bool should_close = false;

        if (user_it->second->m_peer_closed) {
            // 对端已关闭，必须关闭连接
            LOG_DEBUG("Peer closed, final response sent: fd=%d", sockfd);
            should_close = true;
        }
        else if (!user_it->second->is_keep_alive()) {
            // 短连接，响应发送完毕后关闭
            LOG_DEBUG("Short connection, response sent: fd=%d", sockfd);
            should_close = true;
        }
        // else: 长连接且对端未关闭，保持连接(已由write方法处理)

        if (should_close) {
            close_connection(timer, sockfd);
        } else {
            // 长连接，继续处理
            if (timer){
                adjust_timer(timer);
            }
        }
    }
    else if (write_result == 0) {
        // 需要继续写，write方法内部已经注册了EPOLLOUT事件
        // 只需要调整定时器
        if (timer){
            adjust_timer(timer);
        }
    }
    else {
        // 写入失败(write_result == -1)，关闭连接
        close_connection(timer, sockfd);
    }
}

void WebServer::eventLoop(){
    bool timeout = false;
    bool stop_server = false;

    // 留着stop_server，后续可以优雅关闭
    while (!stop_server){
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        // 出错且不是信号中断引起的
        if (number < 0 && errno != EINTR){
            LOG_ERROR("%s", "epoll failure");
            break;
        }
    
        for (int i = 0;i < number;i++){
            int sockfd = events[i].data.fd;
            
            // 处理新客户连接
            if (sockfd ==m_listenfd){
                bool flag = dealclientdata();
                if (!flag)
                    continue;
            }
            // 处理连接异常或关闭
            else if (events[i].events & (EPOLLHUP | EPOLLERR)){
                // 服务器端关闭连接，移除对应的定时器
                auto client_it = m_clients.find(sockfd);
                if (client_it != m_clients.end()) {
                    util_timer * timer = client_it->second->timer;
                    close_connection(timer, sockfd);
                }
            }
            else if (events[i].events & EPOLLRDHUP) {
                LOG_DEBUG("EPOLLRDHUP triggered for fd=%d", sockfd);
                // 不直接删除定时器或关闭连接
            }
            // 定时事件
            else if (sockfd == m_timerfd && (events[i].events & EPOLLIN)) {
                uint64_t exp;
                read(m_timerfd, &exp, sizeof(exp));  // 清空计数，避免重复触发
                timeout = true;   // 保持原来的模型
            }
            // 处理客户数据读取
            else if (events[i].events & EPOLLIN){
                dealwithread(sockfd);
            }
            // 处理客户数据发送
            else if (events[i].events & EPOLLOUT){
                dealwithwrite(sockfd);
            }
        }
        if (timeout){
            timer_handler();
           LOG_DEBUG("%s", "timer tick");
           timeout = false;
        }
    }
}

// WebServer工具方法已移至Utils类

void WebServer::timer_handler(){
    m_timer_wheel.tick();
}
