#ifndef LOCKER_H
#define LOCKER_H

#include <pthread.h>
#include <semaphore.h>


//互斥锁类
class locker
{
public:
    locker()
    {
        pthread_mutex_init(&m_mutex, nullptr);
    }

    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock()
    {
        return pthread_mutex_lock(&m_mutex);
    }

    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex);
    }
private:
    pthread_mutex_t m_mutex;

};
// 信号量类
class sem
{
public:
    sem()
    {
        //初始化信号量为0, 不与其他进程共享
        sem_init(&m_sem, 0, 0);

    }

    sem(int num)
    {
        sem_init(&m_sem, 0, num);
    }

    ~sem()
    {
        sem_destroy(&m_sem);
    }

    bool wait()
    {
        return sem_wait(&m_sem);
    }

    bool post()
    {
        return sem_post(&m_sem);
    }

private:
    sem_t m_sem;
};


#endif