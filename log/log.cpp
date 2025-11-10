#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

Log::Log(){
    m_count = 0;
    m_is_async = false;
}

Log::~Log(){
    if (m_fp != NULL)
        fclose(m_fp);
}

bool Log::init(const char* file_name, int close_log, int log_buf_size,int split_lines, bool is_async) {
    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, 0, m_log_buf_size);
    m_split_lines = split_lines;
    m_is_async = is_async;
    // 默认间隔一秒flush
    m_flush_mode = FlushMode::Interval;
    m_flush_interval_sec = 1; // 每秒 flush
    m_debug_enable = 0; // 默认不开启DEBUG

    time_t t = time(NULL);
    struct tm* sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    const char* p = strrchr(file_name, '/');
    char log_full_name[256] = {0};

    if (p == nullptr) {
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s",
                 my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
        strcpy(log_name, file_name);
        dir_name[0] = '\0';
    } else {
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);
        dir_name[p - file_name + 1] = '\0';
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s",
                 dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;
    m_fp = fopen(log_full_name, "a");
    if (m_fp == nullptr)
        return false;

    if (m_is_async)
        m_thread = std::thread(&Log::async_write_log, this);

    return true;
}

// 工作线程异步写日志
void Log::async_write_log() {
    std::string log_line;
    auto last_flush = std::chrono::steady_clock::now();

    while (!m_exit_flag) {
        // 处理队列中的日志
        while (m_log_queue.try_dequeue(log_line)) {
            std::lock_guard<std::mutex> lock(m_file_mutex);
            fputs(log_line.c_str(), m_fp);
        }

        // 刷盘逻辑
        auto now = std::chrono::steady_clock::now();
        if (m_flush_mode == FlushMode::Immediate) {
            std::lock_guard<std::mutex> lock(m_file_mutex);
            fflush(m_fp);
            last_flush = now;
        } else if (m_flush_mode == FlushMode::Interval) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_flush).count() >= m_flush_interval_sec) {
                std::lock_guard<std::mutex> lock(m_file_mutex);
                fflush(m_fp);
                last_flush = now;
            }
        }

        // 队列为空等待或短暂休眠，降低 CPU 占用
        std::unique_lock<std::mutex> lock(m_cond_mutex);
        m_cond.wait_for(lock, std::chrono::milliseconds(50));
    }

    // 退出前把剩余日志写完
    while (m_log_queue.try_dequeue(log_line)) {
        std::lock_guard<std::mutex> lock(m_file_mutex);
        fputs(log_line.c_str(), m_fp);
    }

    // 最后一次强制 flush
    std::lock_guard<std::mutex> lock(m_file_mutex);
    fflush(m_fp);
}

void Log::write_log(int level, const char* format, ...) {
    // 获取时间并格式化
    struct timeval now;
    gettimeofday(&now, NULL);
    struct tm* sys_tm = localtime(&now.tv_sec);

    const char* level_str = "[info]:";
    switch(level){
        case 0: level_str = "[debug]:"; break;
        case 1: level_str = "[info]:"; break;
        case 2: level_str = "[warn]:"; break;
        case 3: level_str = "[error]:"; break;
    }

    va_list valst;
    va_start(valst, format);
    int n = snprintf(m_buf, m_log_buf_size, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     sys_tm->tm_year+1900, sys_tm->tm_mon+1, sys_tm->tm_mday,
                     sys_tm->tm_hour, sys_tm->tm_min, sys_tm->tm_sec, now.tv_usec, level_str);
    int m = vsnprintf(m_buf+n, m_log_buf_size - n, format, valst);
    m_buf[n+m] = '\n';
    m_buf[n+m+1] = '\0';
    va_end(valst);

    std::string log_line(m_buf);

    if (m_is_async) {
        m_log_queue.enqueue(log_line);
        m_cond.notify_one();
    } else {
        std::lock_guard<std::mutex> lock(m_file_mutex);
        fputs(log_line.c_str(), m_fp);
        fflush(m_fp); // 同步日志每次自动flush
    }
    
}

void Log::flush() {
    std::lock_guard<std::mutex> lock(m_file_mutex);
    fflush(m_fp);
}
