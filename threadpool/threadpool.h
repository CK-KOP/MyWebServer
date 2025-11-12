#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../mydb/sql_connection_pool.h"
using namespace std;

template <typename T>
class threadpool{
public:
    //thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量
    threadpool(connection_pool *connPool, int thread_number= 8, int max_request = 10000);
    ~threadpool();
    bool append_p(T* request);

private:
    // 工作线程运行的函数，它不断从工作队列中取出任务并执行
    void run();

private:
    int m_thread_number;                        // 线程池中的线程数
    int m_max_requests;                         // 请求队列中允许的最大请求数
    std::vector<std::thread> m_threads;         // 描述线程池的数组，其大小为 m_thread_number

    // 无锁队列，出队入队的时候不需要加锁
    moodycamel::ConcurrentQueue<T*> m_workqueue;
    std::atomic<int> m_queue_size{0};
    
    // 条件变量，用于判断当前队列是否需要工作，不工作直接睡眠，防止队列自旋
    std::mutex m_cond_mutex;
    std::condition_variable m_cond;

    std::atomic<bool> m_stop{false}; 

    connection_pool *m_connPool;  // 数据库
};

template <typename T>
threadpool<T>::threadpool(connection_pool *connPool, int thread_number, int max_requests): m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL), m_connPool(connPool) {
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads.reserve(m_thread_number);
    for (int i = 0; i < m_thread_number; ++i) {
        // emplace_back能做到传值进行构造，所以传入了这个函数，以及对象指针
        m_threads.emplace_back([this]() {
            this->run();
        });
    }
}

template <typename T>
threadpool<T>::~threadpool() {
    m_stop.store(true, std::memory_order_relaxed);
    m_cond.notify_all(); // 唤醒所有线程退出

    for (auto& t : m_threads)
        if (t.joinable()) t.join();
}

template <typename T>
bool threadpool<T>::append_p(T *request){
    if (m_queue_size.fetch_add(1, std::memory_order_relaxed) >= m_max_requests) {
        m_queue_size.fetch_sub(1, std::memory_order_relaxed);
        return false; // 队列满
    }
    m_workqueue.enqueue(request);
    m_cond.notify_one();
    return true;
}

template <typename T>
void threadpool<T>::run(){
    while (true){
        // 判断当前工作队列是否为空，要不要睡眠
        std::unique_lock<std::mutex> lock(m_cond_mutex);
        m_cond.wait(lock, [this]{ return m_queue_size > 0 || m_stop; });

        // 确保线程池退出时不会丢掉队列里剩余的任务
        if (m_stop && m_queue_size == 0) break;

        T* request = nullptr;
        if (!m_workqueue.try_dequeue(request))
            continue;
        
        m_queue_size.fetch_sub(1, std::memory_order_relaxed);
        if (!request) continue;

        ConnectionGuard connGuard(*m_connPool);
        request->mysql = connGuard.get();
        request->process();
    }
}

#endif
