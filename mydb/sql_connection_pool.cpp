#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

connection_pool::connection_pool() 
    : m_CurConn(0), m_FreeConn(0), m_MaxConn(0) { }

connection_pool* connection_pool::GetInstance() {
    static connection_pool instance;
    return &instance;
}

// 构造初始化
void connection_pool::init(const std::string& url, const std::string& user, const std::string& password,
                            const std::string& dbName, int port, int maxConn, int close_log){
    m_url = url;
	m_Port = port;
	m_User = user;
	m_PassWord = password;
	m_DataBaseName = dbName;
	m_close_log = close_log;

    for (int i = 0;i < maxConn; i++){
        MYSQL* con = mysql_init(nullptr);

        if (!con){
            LOG_ERROR("MySQL Init Error");
            throw std::runtime_error("MySQL Init Error");
        }
        con = mysql_real_connect(con, url.c_str(), user.c_str(), password.c_str(), dbName.c_str(), port, NULL, 0);
        
        if (!con){
            LOG_ERROR("MySQL Connect Error");
            throw std::runtime_error("MySQL Connect Error: " + std::string(mysql_error(con)));
        }

        // 用 lock_guard 简单保护 connList
        {
            std::lock_guard<std::mutex> lock(mtx);
            connList.push_back(con);
            ++m_FreeConn;
        }
    }

    m_MaxConn = m_FreeConn;
}

// 当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection(){
    std::unique_lock<std::mutex> lock(mtx);  // 条件变量等待需要 unique_lock
    cv.wait(lock, [this] { return !connList.empty(); });

    MYSQL* con = connList.front();
    connList.pop_front();
    
    --m_FreeConn;
    ++m_CurConn;

    return con;
}

// 释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con){
    if (!con) return false;

    {
        std::lock_guard<std::mutex> lock(mtx);   // 临界区短，轻量用 lock_guard
        connList.push_back(con);
        --m_CurConn;
        ++m_FreeConn;
    }
    cv.notify_one(); // 唤醒等待 GetConnection 的线程
    return true;
}

// 当前空闲连接数量
int connection_pool::GetFreeConn() {
    std::lock_guard<std::mutex> lock(mtx);
    return m_FreeConn;
}

// 销毁数据库连接池
void connection_pool::DestoryPool(){
    std::lock_guard<std::mutex> lock(mtx);
    for (auto con : connList){
        if (con) mysql_close(con);
    }
    connList.clear();
    m_CurConn = 0;
    m_FreeConn = 0;
}


connection_pool::~connection_pool(){
    DestoryPool();
}

