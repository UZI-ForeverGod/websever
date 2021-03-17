#ifndef LST_TIMER_H
#define LST_TIMER_H

#include <stdio.h>
#include <time.h>
#include <arpa/inet.h>
#include "http_conn.h"


#define BUFFER_SIZE 64


//定时器节点
template<typename T>
class timer_node
{
public:
    time_t expire;              //任务超时时间
    T* task;                    //任务指针
    timer_node():prev(nullptr), next(nullptr){}
    ~timer_node(){}

    timer_node<T>* prev;        //指向前一个定时器
    timer_node<T>* next;        //指向后一个定时器


};


//升序定时器链表
template<typename T>
class sort_timer_list
{

public:

    sort_timer_list():head(nullptr), tail(nullptr){}
    ~sort_timer_list();

    void add_timer(timer_node<T>* timer);        //向链表中添加节点timer
    void update_timer(timer_node<T>* timer);     //在链表中更新timer的超时时间
    void del_timer(timer_node<T>* &timer);        //在链表中删除节点timer
    void tick();                            //SIGALRM每次触发时都要调用的函数


private:

    void add_timer(timer_node<T>* timer, timer_node<T>* list_head);     //重载函数, 在list_head及其之后的链表中删除timer

    timer_node<T>* head;       //头结点
    timer_node<T>* tail;       //尾结点

};




//析构函数
template<typename T>
sort_timer_list<T>::~sort_timer_list()
{
    timer_node<T>* cur = head;
    while(cur != nullptr)
    {
        timer_node<T>* temp = cur;
        cur = cur->next;
        delete temp;

    }

}


//将新的节点插入链表
template<typename T>
void sort_timer_list<T>::add_timer(timer_node<T>* timer)
{

    //如果链表本来为空
    if(head == nullptr)
    {
        head = timer;
        tail = head;
        printf("本来为空\n");
        return;
    }

    //如果头节点超时时间已经大于或者等于timer, 那么将timer插入成为头节点
    //否则寻找第一个超时时间大于timer的节点, 将timer插在这个节点前边
    if(head->expire >= timer->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    else
    {
        timer_node<T>* cur = head->next;
        while(cur)
        {
            if(cur->expire > timer->expire)
            {
                cur->prev->next = timer;
                timer->prev = cur->prev;
                cur->prev = timer;
                timer->next = cur;
                return;
            }
            else
            {
                cur = cur->next;
            }
        }

        //要是能到这里, 说明应该把节点插入链表尾部
        tail->next = timer;
        timer->next = nullptr;
        timer->prev = tail;
        tail = timer;
        
        
    }
}

template<typename T>
void sort_timer_list<T>::update_timer(timer_node<T>* timer)
{


    timer_node<T>* temp = timer->next;
    //如果当前节点已经是尾节点, 那么就不用更新位置
    if(temp == nullptr)
    {
        return;
    }

    //新的超时时间一定比链表中所有存在的节点的超时时间大, 直接将原来的节点位置移动到末尾即可
    
    //将该节点移出来
    //如果该节点是头节点
    if(timer->prev == nullptr)
    {
        //移除头结点
        head = temp;
        temp->prev = nullptr;

        //放到尾结点
        timer->next = nullptr;
        timer->prev = tail;
        tail->next = timer;
        tail = timer;
    }
    else
    {
        //移除头结点
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        
        //放到尾结点
        timer->next = nullptr;
        timer->prev = tail;
        tail->next = timer;
        tail = timer;
    }
    
}

template<typename T>
void sort_timer_list<T>::del_timer(timer_node<T>* &timer)
{
    //已经删除, 防止重复删除
    if(timer == nullptr)
    {
        return;
    }
    //如果只有一个节点
    if(timer == head && timer == tail)
    {
        delete timer;
        head = nullptr;
        tail = nullptr;
        timer = nullptr;
        return;

    }


    //如果是头节点
    if(timer->prev == nullptr)
    {
        head = timer->next;
        timer->next->prev = nullptr;
        delete timer;
        timer = nullptr;
        return;

    }

    //如果是尾结点
    if(timer->next == nullptr)
    {
        tail = timer->prev;
        tail->next = nullptr;
        delete timer;
        timer = nullptr;
        return;
    }

    //如果位于链表中间
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
    timer = nullptr;
    
}


template<typename T>
void sort_timer_list<T>::tick()
{
    //链表空则不作操作
    if(head == nullptr)
    {
        printf("empty list\n");
        return;
    }

    printf("timer tick\n");
    //获取当前时间
    time_t cur = time(nullptr);

    timer_node<T>* temp = head;
    while(temp)
    {

        if(cur < temp->expire)
        {
            //链表中没有超时
            break;
        }
        else
        {
            //链表中至少有一个超时

            //关闭连接
            temp->task->shut();

            //更新头节点位置
            head = temp->next;
            if(head)
            {
                head->prev = nullptr;
            } 
            delete temp;

            //往前继续判断有无超时
            temp = head;
        }
    }
}








#endif