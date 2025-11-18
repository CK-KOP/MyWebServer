#include "webserver.h"

WebServer::WebServer(){
    
    // http_conn类对象，每个元素对应一个客户端连接
    users = new http_conn[MAX_FD];
    
    // root文件夹路径，资源目录
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    // 包含了该客户端对象的地址、标志符、对应的定时器指针
    users_client_data = new client_data[MAX_FD];
}

WebServer::~WebServer(){
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_client_data;
    delete m_pool;
}

void WebServer::init(int port , string user, string passWord, string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log){

    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
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

    // 初始化数据库读取表map
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool(){
    // 线程池
    m_pool = new threadpool<http_conn>(m_connPool, m_thread_num);
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
    
    // 初始化定时器
    Utils::get_instance().init(TIMESLOT);

    // epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];  //好像完全没有用到
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    // 监听套接字添加到epoll
    Utils::get_instance().addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;
    
    // 创建双向通信管道，用于信号处理
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);

    // 设置写端非阻塞，将读端添加到epoll监控
    Utils::get_instance().setnonblocking(m_pipefd[1]);
    Utils::get_instance().addfd(m_epollfd, m_pipefd[0], false, 0);

    // 信号处理设置
    // 忽略SIGPIPE信号，防止在写入已关闭的socket时服务器崩溃
    // 设置SIGALRM(定时器)和SIGTERM(终止)的处理函数
    // 启动定时器，每TIMESLOT秒产生一个SIGALRM信号
    Utils::get_instance().addsig(SIGPIPE, SIG_IGN);
    Utils::get_instance().addsig(SIGALRM, Utils::get_instance().sig_handler, false);
    Utils::get_instance().addsig(SIGTERM, Utils::get_instance().sig_handler, false);
    alarm(TIMESLOT); // 用信号来保证定时检查定时器的情况

    // 工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

// 初始化http连接类与定时器，并与当前要连接的客户端绑定
void WebServer::create_timer(int connfd, struct sockaddr_in client_address){
    // http类
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);


    // 虽然是users_client_data，但其实是client_data类，里面包含了用客户端信息以及定时器的指针
    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_client_data[connfd].address = client_address;
    users_client_data[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_client_data[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    timer->last_active = cur;
    users_client_data[connfd].timer = timer;
    // static std::atomic<long> timer_create_count{0};
    // timer_create_count++;
    // if (timer_create_count % 10000 == 0)
    //     LOG_INFO("Created timer for connfd=%d, total created: %ld", connfd, timer_create_count.load());
    Utils::get_instance().m_timer_lst.add_timer(timer);
}

// 若有数据传输，不直接修改定时器容器，而是更新该定时器的最新活跃时间
void WebServer::adjust_timer(util_timer *timer){
    time_t cur = time(NULL);
    Utils::get_instance().m_timer_lst.adjust_timer(timer, cur);
}

// 关闭超时连接
void WebServer::deal_timer(util_timer *timer, int sockfd){
    if (!timer) {
        LOG_WARN("Trying to delete null timer: fd=%d", sockfd);
        return;
    }
    // static std::atomic<long> conn_close_count{0};
    // conn_close_count++;
    // if (conn_close_count % 10000 == 0)
    //     LOG_INFO("Closing connfd=%d, total closed: %ld", sockfd, conn_close_count.load());
    timer->cb_func(&users_client_data[sockfd]);
    Utils::get_instance().m_timer_lst.del_timer(timer);
    // 清空指针（防止 double free）
    users_client_data[sockfd].timer = NULL;
    LOG_DEBUG("close fd %d", users_client_data[sockfd].sockfd);
}


// 处理新客户端连接
bool WebServer::dealclientdata(){
    struct sockaddr_in client_address;
    socklen_t client_addrlenth = sizeof(client_address);
    
    if (0 == m_LISTENTrigmode){  // LT模式
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlenth);
        if (connfd < 0){
            LOG_ERROR("LT accept error: %d", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD){
            Utils::get_instance().show_error(connfd, "Internal server busy");
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
                return false;
            }
            if (http_conn::m_user_count >= MAX_FD){
                Utils::get_instance().show_error(connfd, "Internal server busy");
                continue;  // 继续接受其他连接
            }
            create_timer(connfd, client_address);
        }
    }
    return true;
}

// 处理信号请求
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server){
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
        return false;
    else if (ret == 0)
        return false;
    else{
        for (int i = 0;i < ret;i++){
            switch (signals[i]){
            case SIGALRM:{
                timeout = true;
                break;
            }
            case SIGTERM:{
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd){
    util_timer *timer = users_client_data[sockfd].timer;
    // 日志记录ip地址
    LOG_DEBUG("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
    int flag = users[sockfd].read_once();
    if (flag > 0) {

        // 若监测到读事件，将该事件放入请求队列
        m_pool->append_p(users + sockfd);

        if (timer){
            adjust_timer(timer);
        }
    }
    else if(flag < 0){
        deal_timer(timer, sockfd);
    }
    else{
        if (users[sockfd].is_keep_alive() == 0) {
            deal_timer(timer, sockfd); // 非长连接，真正关闭连接
        } 
    }
    
}

void WebServer::dealwithwrite(int sockfd){
    util_timer *timer = users_client_data[sockfd].timer;

    LOG_DEBUG("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
    if (users[sockfd].write()) {

        if (timer){
            adjust_timer(timer);     
        }
    }
    else {
        deal_timer(timer, sockfd);
    }
    
}

void WebServer::eventLoop(){
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server){
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        // 出错且不是信号中断引起的
        if (number < 0 && errno != EINTR){
            LOG_ERROR("%s", "epoll failure");
            break;
        }
    
        for (int i = 0;i < number;i++){
            int sockfd = events[i].data.fd;
            
            //printf("触发epoll_wait的sockfd: %d\n", sockfd);
            // 处理新客户连接
            if (sockfd ==m_listenfd){
                //printf("类型为新客户端连接\n");
                bool flag = dealclientdata();
                if (!flag)
                    continue;
            }
            // 处理连接异常或关闭
            else if (events[i].events & (EPOLLHUP | EPOLLERR)){
                //printf("类型为连接异常\n");
                // 服务器端关闭连接，移除对应的定时器
                util_timer * timer = users_client_data[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            else if (events[i].events & EPOLLRDHUP) {
                LOG_DEBUG("EPOLLRDHUP triggered for fd=%d", sockfd);
                // 不直接删除定时器或关闭连接
            }
            // 处理信号，来自管道的读端 m_pipefd[0] 且是可读事件
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)){
                //printf("类型为信号处理\n");
                bool flag = dealwithsignal(timeout, stop_server);
                if (!flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            // 处理客户数据读取
            else if (events[i].events & EPOLLIN){
                //printf("类型为客户端数据读取\n");
                dealwithread(sockfd);
            }
            // 处理客户数据发送
            else if (events[i].events & EPOLLOUT){
                //printf("类型为客户端数据发送\n");
                dealwithwrite(sockfd);
            }
        }
        if (timeout){
            Utils::get_instance().timer_handler();
           LOG_DEBUG("%s", "timer tick");
           timeout = false;
        }
    }
}
