#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"

// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template<typename T>
class threadpool 
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int thread_number = 4, int max_requests = 10000);
    ~threadpool();
    /*将新的任务加入任务队列*/
    bool append(T* request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void* worker(void* arg);
    /*执行任务使用的函数*/
    void run();

private:
    // 线程的数量
    int m_thread_number;  
    
    // 描述线程池的数组，大小为m_thread_number    
    pthread_t * m_threads;

    // 请求队列中最多允许的、等待处理的请求的数量  
    int m_max_requests; 
    
    // 请求队列
    std::list< T* > m_workqueue;  

    // 保护请求队列的互斥锁
    locker m_queuelocker;   

    // 信号量，记录任务队列中任务数
    sem m_queuestat;

    // 是否结束线程          
    bool m_stop;                    
};

template< typename T >
threadpool< T >::threadpool(int thread_number, int max_requests) : 
        m_thread_number(thread_number), m_max_requests(max_requests), 
        m_stop(false), m_threads(NULL) 
{

    if((thread_number <= 0) || (max_requests <= 0) ) 
    {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];
    if(!m_threads) 
    {
        throw std::exception();
    }

    // 创建thread_number 个线程，并将他们设置为脱离线程。
    for ( int i = 0; i < thread_number; ++i ) 
    {
        printf( "create the %dth thread\n", i);
        //创建线程
        if(pthread_create(m_threads + i, NULL, worker, this ) != 0) 
        {
            delete [] m_threads;
            throw std::exception();
        }
        //线程分离
        if( pthread_detach( m_threads[i] ) ) 
        {
            delete [] m_threads;
            throw std::exception();
        }
    }
}

/*析构函数*/
template< typename T >
threadpool< T >::~threadpool() 
{
    delete [] m_threads;
    m_stop = true;
}
/*将新的任务加入队列中*/
template< typename T >
bool threadpool< T >::append( T* request )
{
    // 操作工作队列时一定要加锁，因为它被所有线程共享。
    m_queuelocker.lock();
    if ( m_workqueue.size() > m_max_requests ) 
    {
        m_queuelocker.unlock();
        return false;
    }
    //加入任务队列
    m_workqueue.push_back(request);
    //解锁
    m_queuelocker.unlock();
    //增加信号量
    m_queuestat.post();
    return true;
}


/*线程执行的函数*/
template< typename T >
void* threadpool< T >::worker( void* arg )
{
    threadpool* pool = ( threadpool* )arg;
    pool->run();
    return pool;
}
/*执行的任务，从任务队列中取出任务来执行*/
template< typename T >
void threadpool< T >::run() 
{

    while (!m_stop) 
    {
        //信号量减
        m_queuestat.wait();
        //加锁
        m_queuelocker.lock();
        
//        if ( m_workqueue.empty() ) 
//        {
//           m_queuelocker.unlock();
//            continue;
//        }
        
        //取出任务
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        //解锁
        m_queuelocker.unlock();

        
//        if ( !request ) {
//            continue;
//        }


        //执行任务
        request->process();

        //任务执行完毕后继续往回循环
    }

}

#endif
