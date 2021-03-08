#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <pthread.h>
#include <cstdio>
#include "locker.h"
#define THREAD_NUMBER 4
#define MAX_REQUESTS 10000



template<typename T>
class threadpool
{
public:
    threadpool(int thread_number = THREAD_NUMBER, int max_requests = MAX_REQUESTS);
    ~threadpool();

    //将新的任务加入任务队列
    bool append(T* request);
private:
    //工作线程运行时的函数
    static void* worker(void* arg);
    //线程执行任务所用函数
    void run();
private:
    int m_thread_number;                //线程池中线程数量
    pthread_t* m_threads;               //存放线程池中线程ID的数组
    int m_max_requests;                 //请求队列中最多允许等待的任务数量
    std::list<T*> m_workqueue;          //任务队列
    locker m_queuelocker;               //保护任务队列的互斥锁
    sem m_requests_number;              //信号量，值等于任务队列中的任务数量
    bool m_stop;                        //线程停止运行标志
};

template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests):
        m_thread_number(thread_number), m_max_requests(max_requests), m_threads(nullptr), m_stop(false)
{
    //创建线程ID数组以及线程
    m_threads = new pthread_t[m_thread_number];
    for(int i = 0; i < thread_number; ++i)
    {
        printf("create %d pthread\n", i + 1);
        pthread_create(m_threads + i, nullptr, worker, this);
        pthread_detach(m_threads[i]);

    }
}

template<typename T>
threadpool<T>::~threadpool()
{
    delete [] m_threads;
    //线程停止运行
    m_stop = true;
}

template<typename T>
bool threadpool<T>::append(T* request)
{
    //临界区，加锁
    m_queuelocker.lock();
    //如果任务队列已满, 则返回错误
    if(m_workqueue.size() > m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_requests_number.post();
    

    return true;
}

template<typename T>
void* threadpool<T>::worker(void* arg)
{
    threadpool* pool = static_cast<threadpool*>(arg);
    pool->run();
    
    //没有意义的返回值, 不可能执行到这里
    return pool;

}

template<typename T>
void threadpool<T>::run()
{
    while(!m_stop)
    {
        //取出任务, 信号量减少
        m_requests_number.wait();
        //临界区, 上锁
        m_queuelocker.lock();

        //取任务
        T* request = m_workqueue.front();
        m_workqueue.pop_front();

        //解锁
        m_queuelocker.unlock();


        //执行任务
        request->process();
    }
}
#endif