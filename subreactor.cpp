#include "subreactor.h"

// 常量定义（从webserver.h移动过来）
const int MAX_FD = 65536;           // 最大文件描述符
const int TIMESLOT = 1;             // 最小超时单位

SubReactor::SubReactor(int sub_reactor_id, const char* root, int conn_trig_mode, int close_log,
                       const std::string& user, const std::string& passWord, const std::string& databaseName,
                       connection_pool* connPool)
    : m_sub_reactor_id(sub_reactor_id), m_conn_trig_mode(conn_trig_mode), m_close_log(close_log),
      m_connPool(connPool), m_user(user), m_passWord(passWord), m_databaseName(databaseName) {

    // 复制资源目录路径
    m_root = new char[strlen(root) + 1];
    strcpy(m_root, root);

    // 初始化定时器（时间轮）
    m_timer_wheel.set_timeslot(TIMESLOT);

    LOG_INFO("SubReactor %d created", m_sub_reactor_id);
}

SubReactor::~SubReactor() {
    stop();
    delete[] m_root;

    // 清理文件描述符
    if (m_epollfd != -1) close(m_epollfd);
    if (m_timerfd != -1) close(m_timerfd);

    LOG_INFO("SubReactor %d destroyed", m_sub_reactor_id);
}

void SubReactor::start() {
    if (m_running.load()) {
        LOG_WARN("SubReactor %d is already running", m_sub_reactor_id);
        return;
    }

    initEpoll();
    m_running.store(true);
    m_thread = std::thread(&SubReactor::eventLoop, this);

    LOG_INFO("SubReactor %d started", m_sub_reactor_id);
}

void SubReactor::stop() {
    if (!m_running.load()) {
        return;
    }

    m_running.store(false);

    // 通知线程退出
    {
        std::lock_guard<std::mutex> lock(m_connection_mutex);
        m_connection_cv.notify_all();
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    LOG_INFO("SubReactor %d stopped", m_sub_reactor_id);
}

void SubReactor::initEpoll() {
    // 创建epoll实例
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    // 创建 timerfd
    m_timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    assert(m_timerfd > 0);

    // 设置周期定时器
    itimerspec new_value{};
    new_value.it_value.tv_sec = TIMESLOT;
    new_value.it_interval.tv_sec = TIMESLOT;
    timerfd_settime(m_timerfd, 0, &new_value, nullptr);

    // 将 timerfd 添加到 epoll
    Utils::addfd(m_epollfd, m_timerfd, false, 0);
}

bool SubReactor::add_connection(int connfd, struct sockaddr_in client_address) {
    // 检查连接数限制
    if (m_user_count.load() >= MAX_FD) {
        LOG_WARN("SubReactor %d: Too many connections", m_sub_reactor_id);
        Utils::show_error(connfd, "Internal server busy");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_connection_mutex);
        m_pending_connections.push({connfd, client_address});
        m_connection_cv.notify_one();
    }

    LOG_DEBUG("SubReactor %d: New connection %d queued", m_sub_reactor_id, connfd);
    return true;
}

void SubReactor::eventLoop() {
    LOG_INFO("SubReactor %d: Event loop started", m_sub_reactor_id);

    bool timeout = false;

    while (m_running.load()) {
        // 检查是否有新连接需要添加
        {
            std::unique_lock<std::mutex> lock(m_connection_mutex);
            if (!m_pending_connections.empty()) {
                auto conn_info = m_pending_connections.front();
                m_pending_connections.pop();
                lock.unlock();

                // 创建连接和定时器
                create_timer(conn_info.first, conn_info.second);

                // 将新连接添加到epoll
                Utils::addfd(m_epollfd, conn_info.first, false, m_conn_trig_mode);
                LOG_DEBUG("SubReactor %d: Added connection %d to epoll", m_sub_reactor_id, conn_info.first);
            }
        }

        // 等待事件，带超时以便定期检查pending连接
        int number = epoll_wait(m_epollfd, events, SUB_MAX_EVENT_NUMBER, 100);

        if (number < 0 && errno != EINTR) {
            LOG_ERROR("SubReactor %d: epoll failure", m_sub_reactor_id);
            break;
        }

        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;

            // 定时事件
            if (sockfd == m_timerfd && (events[i].events & EPOLLIN)) {
                uint64_t exp;
                read(m_timerfd, &exp, sizeof(exp));
                timeout = true;
            }
            // 处理连接异常或关闭
            else if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                dealwithexception(sockfd);
            }
            // 处理客户数据读取
            else if (events[i].events & EPOLLIN) {
                dealwithread(sockfd);
            }
            // 处理客户数据发送
            else if (events[i].events & EPOLLOUT) {
                dealwithwrite(sockfd);
            }
        }

        if (timeout) {
            timer_handler();
            timeout = false;
        }
    }

    LOG_INFO("SubReactor %d: Event loop ended", m_sub_reactor_id);
}

void SubReactor::create_timer(int connfd, struct sockaddr_in client_address) {
    // 创建http连接对象
    auto http_conn_ptr = std::make_unique<http_conn>();
    http_conn_ptr->init(connfd, client_address, m_root, m_conn_trig_mode, m_close_log,
                       m_user, m_passWord, m_databaseName, m_epollfd);

    // 增加连接计数
    m_user_count++;

    // 创建client_data对象
    auto client_data_ptr = std::make_unique<client_data>();
    client_data_ptr->address = client_address;
    client_data_ptr->sockfd = connfd;

    // 创建定时器
    util_timer *timer = new util_timer;
    timer->user_data = client_data_ptr.get();

    // 使用lambda创建回调函数
    timer->cb_func = [this, connfd]() {
        this->close_connection_by_timer(connfd);
    };

    time_t cur = time(NULL);
    int random_timeout = m_timer_wheel.get_random_timeout();
    timer->expire = cur + random_timeout;
    timer->last_active = cur;

    client_data_ptr->timer = timer;
    m_timer_wheel.add_timer(timer);

    // 将对象添加到map中
    m_users[connfd] = std::move(http_conn_ptr);
    m_clients[connfd] = std::move(client_data_ptr);
}

void SubReactor::adjust_timer(util_timer* timer) {
    time_t cur = time(NULL);
    m_timer_wheel.adjust_timer(timer, cur);
}

void SubReactor::close_connection(util_timer *timer, int sockfd) {
    if (!timer) {
        LOG_WARN("SubReactor %d: Trying to delete null timer: fd=%d", m_sub_reactor_id, sockfd);
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
        m_clients.erase(client_it);
    }

    auto user_it = m_users.find(sockfd);
    if (user_it != m_users.end()) {
        m_users.erase(user_it);
    }

    // 减少连接计数
    m_user_count--;

    LOG_DEBUG("SubReactor %d: Closed fd %d", m_sub_reactor_id, sockfd);
}

void SubReactor::close_connection_by_timer(int sockfd) {
    // 从epoll中删除文件描述符
    epoll_ctl(m_epollfd, EPOLL_CTL_DEL, sockfd, 0);
    // 关闭套接字
    close(sockfd);

    // 从map中移除，自动删除对象
    auto client_it = m_clients.find(sockfd);
    if (client_it != m_clients.end()) {
        m_clients.erase(client_it);
    }

    auto user_it = m_users.find(sockfd);
    if (user_it != m_users.end()) {
        m_users.erase(user_it);
    }

    // 减少连接计数
    m_user_count--;

    LOG_DEBUG("SubReactor %d: Timer callback closed fd %d", m_sub_reactor_id, sockfd);
}

void SubReactor::dealwithread(int sockfd) {
    auto client_it = m_clients.find(sockfd);
    auto user_it = m_users.find(sockfd);

    if (client_it == m_clients.end() || user_it == m_users.end()) {
        LOG_WARN("SubReactor %d: Connection %d not found in maps", m_sub_reactor_id, sockfd);
        return;
    }

    util_timer *timer = client_it->second->timer;

    LOG_DEBUG("SubReactor %d: Deal with read from client(%s)",
              m_sub_reactor_id, inet_ntoa(user_it->second->get_address()->sin_addr));

    int flag = user_it->second->read_once();
    if (flag > 0) {
        // 成功读取到数据
        ConnectionGuard connGuard(*m_connPool);
        user_it->second->mysql = connGuard.get();
        http_conn::PROCESS_RESULT result = user_it->second->process();

        if (result == http_conn::PROCESS_ERROR) {
            close_connection(timer, sockfd);
            return;
        }
        else if (result == http_conn::PROCESS_CONTINUE) {
            Utils::modfd(m_epollfd, sockfd, EPOLLIN, m_conn_trig_mode);
        }
        else if (result == http_conn::PROCESS_OK) {
            Utils::modfd(m_epollfd, sockfd, EPOLLOUT, m_conn_trig_mode);
        }

        if (timer) {
            adjust_timer(timer);
        }
    }
    else if(flag < 0) {
        // 读取错误，关闭连接
        close_connection(timer, sockfd);
    }
    else {
        // flag == 0，对端关闭了连接
        ConnectionGuard connGuard(*m_connPool);
        user_it->second->mysql = connGuard.get();
        http_conn::PROCESS_RESULT result = user_it->second->process();

        if (result == http_conn::PROCESS_ERROR) {
            close_connection(timer, sockfd);
            return;
        }
        else if (result == http_conn::PROCESS_CONTINUE) {
            LOG_DEBUG("SubReactor %d: Incomplete request but client closed: fd=%d", m_sub_reactor_id, sockfd);
            close_connection(timer, sockfd);
        }
        else if (result == http_conn::PROCESS_OK) {
            LOG_DEBUG("SubReactor %d: Client closed, sending final response: fd=%d", m_sub_reactor_id, sockfd);
            user_it->second->m_peer_closed = true;
            Utils::modfd(m_epollfd, sockfd, EPOLLOUT, m_conn_trig_mode);

            if (timer) {
                adjust_timer(timer);
            }
        }
    }
}

void SubReactor::dealwithwrite(int sockfd) {
    auto client_it = m_clients.find(sockfd);
    auto user_it = m_users.find(sockfd);

    if (client_it == m_clients.end() || user_it == m_users.end()) {
        LOG_WARN("SubReactor %d: Connection %d not found in maps", m_sub_reactor_id, sockfd);
        return;
    }

    util_timer *timer = client_it->second->timer;

    LOG_DEBUG("SubReactor %d: Send data to client(%s)",
              m_sub_reactor_id, inet_ntoa(user_it->second->get_address()->sin_addr));

    int write_result = user_it->second->write();

    if (write_result == 1) {
        // 写入完成，检查是否需要关闭连接
        bool should_close = false;

        if (user_it->second->m_peer_closed) {
            LOG_DEBUG("SubReactor %d: Peer closed, final response sent: fd=%d", m_sub_reactor_id, sockfd);
            should_close = true;
        }
        else if (!user_it->second->is_keep_alive()) {
            LOG_DEBUG("SubReactor %d: Short connection, response sent: fd=%d", m_sub_reactor_id, sockfd);
            should_close = true;
        }

        if (should_close) {
            close_connection(timer, sockfd);
        } else {
            if (timer) {
                adjust_timer(timer);
            }
        }
    }
    else if (write_result == 0) {
        // 需要继续写
        if (timer) {
            adjust_timer(timer);
        }
    }
    else {
        // 写入失败，关闭连接
        close_connection(timer, sockfd);
    }
}

void SubReactor::dealwithexception(int sockfd) {
    auto client_it = m_clients.find(sockfd);
    if (client_it != m_clients.end()) {
        util_timer *timer = client_it->second->timer;
        close_connection(timer, sockfd);
    }
}

void SubReactor::timer_handler() {
    m_timer_wheel.tick();
}