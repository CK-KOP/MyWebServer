#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <mysql/mysql.h>
#include <string>
#include <list>
#include <mutex>
#include <condition_variable>
#include "../log/log.h"


class connection_pool{
public:
    MYSQL *GetConnection();              // 获取数据库连接
    bool ReleaseConnection(MYSQL *conn); // 释放连接
    int GetFreeConn();                   // 获取连接
    void DestoryPool();                  // 销毁所有连接

    // 单例模式
    static connection_pool *GetInstance();
    void init(const std::string& url,
        const std::string& user,
        const std::string& password,
        const std::string& dbName,
        int port,
        int maxConn,
        int close_log);

private:
    connection_pool();
    ~connection_pool();

    int m_MaxConn;                      // 最大连接数
    int m_CurConn;                      // 当前使用的连接数
    int m_FreeConn;                     // 当前空闲连接数
    
    std::list<MYSQL*> connList;         // 连接池容器

    // ---------- 修改锁和条件变量 ----------
    std::mutex mtx;                      // 保护 connList 和计数
    std::condition_variable cv;          // 等待空闲连接
    
public:
    std::string m_url;           // 主机开关
    std::string m_Port;          // 数据库端口号
    std::string m_User;          // 登陆数据库用户名
    std::string m_PassWord;      // 登陆数据库密码
    std::string m_DataBaseName;  //使用数据库名
    int m_close_log;        //日志开关
};

// RAII 连接管理类
class ConnectionGuard {
    public:
        explicit ConnectionGuard(connection_pool& pool)
            : pool_(pool), conn_(pool_.GetConnection()) {}
        ~ConnectionGuard() { pool_.ReleaseConnection(conn_); }
    
        MYSQL* get() const { return conn_; }
        MYSQL* operator->() const { return conn_; }
    
    private:
        connection_pool& pool_;
        MYSQL* conn_;
    };
#endif
