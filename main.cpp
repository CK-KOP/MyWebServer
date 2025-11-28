#include "config.h"

int main(int argc, char *argv[]){
    
    // 需要修改的数据库信息，登陆名、密码、库名
    string user = "root";
    string passwd = "171023Cxl!";
    string databasename = "kopdb";

    // 命令行解析
    Config config;
    config.parse_arg(argc, argv);

    WebServer server;

    // 初始化
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite,
                config.OPT_LINGER, config.TRIGMode,  config.sql_num,
                config.close_log);

    //日志
    server.log_write();

    //数据库
    server.sql_pool();

    //触发模式
    server.trig_mode();
    
    //printf("即将进入监听\n");

    //监听
    server.eventListen();
    
    //printf("进入监听\n");
    
    //printf("即将进入运行\n");
    //运行
    server.eventLoop();
    //printf("进入运行\n");

    return 0;
}
