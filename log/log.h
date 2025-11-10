#ifndef LOG_H
#define LOG_H

#include "../third_party/concurrentqueue/concurrentqueue.h"
#include <string>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <iostream>

class Log{
public:
    // 单例模式获取实例
    static Log *get_instance(){
        //C++11以后,使用局部变量懒汉不用加锁
        static Log instance;
        return &instance;
    }

    bool init(const char *file_name, const int close_log, int log_buf_size = 8192, int split_lines = 5000000, bool is_async = 1);

    void write_log(int level, const char *format, ...);

    void flush(void);

    // 日志刷新模式
    enum class FlushMode {
        Immediate,  // 每条日志立即 flush
        Interval    // 定时 flush
    };
    bool get_debug_enable() {return m_debug_enable;}
    int get_is_close_log() {return m_close_log;}

private:
    Log();
    virtual ~Log();
    void async_write_log();


private:
    char dir_name[128]; // 路径名
    char log_name[128]; // log文件名
    int m_split_lines;  // 日志最大行数
    int m_log_buf_size; // 日志缓冲区大小 
    long long m_count;  // 日志行数记录
    int m_today;        // 因为按天分类，记录当前时间是哪一天
    FILE *m_fp;         // 打开log的文件指针 
    char *m_buf;

    // 刷盘模式
    FlushMode m_flush_mode;
    int m_flush_interval_sec; // 定时刷盘间隔
    bool m_debug_enable;  // 是否开启debug选项

    // 异步日志
    moodycamel::ConcurrentQueue<std::string> m_log_queue;
    std::condition_variable m_cond;
    std::mutex m_cond_mutex;
    std::thread m_thread;
    std::atomic<bool> m_exit_flag{false};

    bool m_is_async;           // 是否同步标志位
    std::mutex m_file_mutex;
    int m_close_log; // 关闭日志
};
#define LOG_DEBUG(format, ...) if(0 == Log::get_instance()->get_is_close_log() && Log::get_instance()->get_debug_enable()) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); }
#define LOG_INFO(format, ...) if(0 == Log::get_instance()->get_is_close_log()) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); }
#define LOG_WARN(format, ...) if(0 == Log::get_instance()->get_is_close_log()) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); }
#define LOG_ERROR(format, ...) if(0 == Log::get_instance()->get_is_close_log()) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); }

#endif
