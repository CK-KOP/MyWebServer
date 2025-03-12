#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

class sem{
public:
    sem(){
        // sem_init是POSIX 信号量的初始化函数
        if (sem_init(&m_sem, 0, 0) != 0)
            throw std::exception();
    }
    
    sem(int num){
        // sem_init()第二个参数是线程间共享标志（0表示仅限本进程内使用）    
        if (sem_init(&m_sem, 0, num) != 0)
            throw std::exception();
    }
    
    ~sem(){
        sem_destroy(&m_sem);
    }
    
    bool wait(){
        // 信号等待，如果信号量值为零，调用线程会阻塞直到信号量大于零
        return sem_wait(&m_sem) == 0;   
    }
    
    bool post(){
        // 信号释放，增加信号量的值，并唤醒一个等待的线程
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;

};

class locker{
public:
    locker(){
        // 初始化互斥锁，m_mutex 是互斥锁对象，NULL 参数表示该锁是进程内的锁。
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
            throw std::exception();
    }
    
    ~locker(){
        pthread_mutex_destroy(& m_mutex);
    }
    
    bool lock(){
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    
    bool unlock(){
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    
    pthread_mutex_t *get(){
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex; 
};

class cond{
public:
    cond(){
        // 初始化条件变量，m_cond 是条件变量对象。NULL 参数表示该条件变量是进程内的。
        if (pthread_cond_init(&m_cond, NULL) != 0)
            throw std::exception();
    }
    
    ~cond(){
        pthread_cond_destroy(&m_cond);
    }
    
    bool wait(pthread_mutex_t *m_mutex){
        int ret = 0;
        ret = pthread_cond_wait(&m_cond, m_mutex);
        return ret == 0;
    }
    
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t){
        int ret = 0;
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        return ret == 0;
    }
    
    bool signal(){
        return pthread_cond_signal(&m_cond) == 0;
    }
    
    bool broadcast(){
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    pthread_cond_t m_cond;
};
#endif
