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
    stop_sub_reactors();
    close(m_epollfd);
    close(m_listenfd);
}

void WebServer::init(int port , std::string user, std::string passWord, std::string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num, int thread_num,
              int close_log){

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
            Log::get_instance()->init("./logs/ServerLog", m_close_log, 2000, 800000, 1);
        // 同步日志，每次调用 write_log() 直接写文件
        else
            Log::get_instance()->init("./logs/ServerLog", m_close_log, 2000, 800000, 0);
    }
}

void WebServer::sql_pool(){
    // 初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    // 使用静态方法初始化数据库用户数据
    http_conn::init_database_users(m_connPool);
}

void WebServer::create_sub_reactors(){
    m_sub_reactors.reserve(m_thread_num);

    for (int i = 0; i < m_thread_num; i++) {
        auto sub_reactor = std::make_unique<SubReactor>(
            i, m_root, m_CONNTrigmode, m_close_log,
            m_user, m_passWord, m_databaseName, m_connPool
        );
        m_sub_reactors.push_back(std::move(sub_reactor));

        LOG_INFO("Created SubReactor %d", i);
    }
}

void WebServer::start_sub_reactors(){
    for (auto& sub_reactor : m_sub_reactors) {
        sub_reactor->start();
    }
    LOG_INFO("All %d SubReactors started", m_thread_num);
}

void WebServer::stop_sub_reactors(){
    for (auto& sub_reactor : m_sub_reactors) {
        if (sub_reactor) {
            sub_reactor->stop();
        }
    }
    LOG_INFO("All SubReactors stopped");
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
    
    // epoll创建内核事件表（主Reactor）
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    // 监听套接字添加到epoll（主Reactor只监听连接事件）
    Utils::addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);

    // 忽略SIGPIPE信号，防止在写入已关闭的socket时服务器崩溃
    Utils::addsig(SIGPIPE, SIG_IGN);
}

// 连接分发：使用轮询算法将连接分配给SubReactor
bool WebServer::dispatch_connection(int connfd, struct sockaddr_in client_address) {
    if (m_sub_reactors.empty()) {
        LOG_ERROR("No SubReactors available!");
        return false;
    }

    // 轮询选择SubReactor
    int reactor_index = m_next_sub_reactor.fetch_add(1) % m_thread_num;

    bool success = m_sub_reactors[reactor_index]->add_connection(connfd, client_address);

    if (success) {
        LOG_DEBUG("MainReactor: Dispatched connection %d to SubReactor %d", connfd, reactor_index);
    } else {
        LOG_WARN("MainReactor: Failed to dispatch connection %d to SubReactor %d", connfd, reactor_index);
    }

    return success;
}

// 处理新客户端连接（主Reactor）
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

        // 分发连接到SubReactor
        return dispatch_connection(connfd, client_address);
    }
    else{  // ET模式
        bool success = true;
        while (1){
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlenth);
            if (connfd < 0){
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;  // accept已经读取不到连接了，正常结束
                LOG_ERROR("ET accept error: %d", errno);
                LOG_ERROR("accept error: errno=%d (%s)", errno, strerror(errno));
                return false;
            }

            // 分发连接到SubReactor
            if (!dispatch_connection(connfd, client_address)) {
                success = false;  // 记录失败，但继续处理其他连接
            }
        }
        return success;
    }
}


// 主Reactor事件循环：只负责处理新连接
void WebServer::eventLoop(){
    bool stop_server = false;

    LOG_INFO("MainReactor: Event loop started, listening on port %d", m_port);

    while (!stop_server){
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);

        // 出错且不是信号中断引起的
        if (number < 0 && errno != EINTR){
            LOG_ERROR("MainReactor: epoll failure, errno=%d", errno);
            break;
        }

        for (int i = 0; i < number; i++){
            int sockfd = events[i].data.fd;

            // 处理新客户连接
            if (sockfd == m_listenfd){
                LOG_DEBUG("MainReactor: New connection event on listen fd");
                dealclientdata();
            }
            // 处理监听socket异常
            else if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                if (sockfd == m_listenfd) {
                    LOG_ERROR("MainReactor: Listen socket error");
                    break;
                }
            }
        }
    }

    LOG_INFO("MainReactor: Event loop ended");
}
