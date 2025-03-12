#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"
using namespace std;

class connection_pool{
public:
    MYSQL *GetConnection();              // 获取数据库连接
    bool ReleaseConnection(MYSQL *conn); // 释放连接
    int GetFreeConn();                   // 获取连接
    void DestoryPool();                  // 销毁所有连接

    // 单例模式
    static connection_pool *GetInstance();
    void init(string url, string User, string PassWord, string DataBaseName, int port, int MaxConn, int close_log);

private:
    connection_pool();
    ~connection_pool();

    int m_MaxConn;
    int m_CurConn;
    int m_FreeConn;
    locker lock;
    list<MYSQL *>connList;
    sem reserve;
public:
    string m_url;           // 主机开关
    string m_Port;          // 数据库端口号
    string m_User;          // 登陆数据库用户名
    string m_PassWord;      // 登陆数据库密码
    string m_DataBaseName;  //使用数据库名
    int m_close_log;        //日志开关
};

class connectionRAII{
public:
    connectionRAII(MYSQL **con, connection_pool *connPool);
    ~connectionRAII();
private:
    MYSQL *conRAII;
    connection_pool *poolRAII;
};
#endif
