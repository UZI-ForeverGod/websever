#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "lst_timer.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量
#define TIMESLOT 3  //超时时间系数
//初始化任务类中共享的定时器链表
sort_timer_lst<http_conn> http_conn::timerList;

// 向epoll中添加要监视的文件描述符
extern void addfd( int epollfd, int fd, bool one_shot );
// 从epoll中删除要监视的文件描述符
extern void removefd( int epollfd, int fd );
    
//添加要捕捉的信号
//在捕捉的信号函数执行过程中阻塞其他信号
void addsig(int sig, void( *handler )(int))
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

void timer_handler(int)
{
    // 定时处理任务，实际上就是调用tick()函数
    http_conn::timerList.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}
// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
int main(int argc, char* argv[]) 
{
    
    //要输入好监听的端口号
    if(argc <= 1) 
    {
        printf("usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);
    
    
    //忽略SIGPIPE信号，以防向已断开TCP连接的socket发送数据时产生该信号并且终止进程
    addsig(SIGPIPE, SIG_IGN);
    //修改SIGALARM信号
    addsig(SIGALRM, timer_handler);

    //内存池
    threadpool<http_conn>* pool = new threadpool<http_conn>;

    //任务数组
    http_conn* users = new http_conn[MAX_FD];

    

    //创建服务器的监听socket
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);

    //地址结构
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );

    // 端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //绑定端口
    bind(listenfd, (struct sockaddr*)&address, sizeof(address));

    //开始监听
    listen(listenfd, 5);

    // 创建传出事件数组，添加
    epoll_event events[MAX_EVENT_NUMBER];
    //创建epoll对象
    int epollfd = epoll_create(5);
    
    // 添加到epoll对象中检测读事件
    addfd(epollfd, listenfd, false);

    //初始化任务类中共享的epoll对象
    http_conn::m_epollfd = epollfd;
    alarm(TIMESLOT);
    while(true) 
    {
        
        //开始epoll监听
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        
        //如果失败并且不是被信号打断
        if ((number < 0) && (errno != EINTR)) 
        {
            printf("epoll failure\n");
            break;
        }
        
        for (int i = 0; i < number; i++) 
        {
            
            int sockfd = events[i].data.fd;
            
            if(sockfd == listenfd)
            {
                
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);
                
                if (connfd < 0) 
                {
                    printf("errno is: %d\n", errno);
                    continue;
                }

                //大过预定的最大文件描述符数量就关闭连接
                //其实这个数字可能已经超过系统内核规定的最大文件描述符数量了
                if( http_conn::m_user_count >= MAX_FD ) 
                {
                    close(connfd);
                    continue;
                }

                //初始化任务数组中的任务对象
                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                
                

                util_timer<http_conn>* timer = new util_timer<http_conn>;
                timer->task = &users[connfd];
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users[connfd].timer = timer;
                http_conn::timerList.add_timer(timer);
                

                //初始化任务数组并加入epoll的监听中
                users[connfd].init(connfd, client_address);

            } 
            else
            {
                if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) 
                {
                    //读关闭或者读写关闭或者错误
                    //EPOLLRDHUB 检测到对端已经关闭socket的写端，本端读不到任何数据
                    //EPOLLHUP 检测到socket正常关闭
                    //EPOLLERR 检测到对方socket异常关闭

                    //从定时器链表中删除该定时器
                    
                    http_conn::timerList.del_timer(users[sockfd].timer);
                    users[sockfd].close_conn();

                } 
                else
                {
                    if(events[i].events & EPOLLIN) 
                    {
                        //检测到对方已经准备好要发送的数据
                        //接收完全部数据后将任务放入线程池的任务队列中准备执行
                        if(users[sockfd].read()) 
                        {
                            // 如果某个客户端上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间。
                        
                            
                            if(users[sockfd].timer)
                            {
                                time_t cur = time(NULL);

                                users[sockfd].timer->expire = cur + 3 * TIMESLOT;
                                
                                
                                http_conn::timerList.adjust_timer(users[sockfd].timer);
                            }
                        
                            
                    
                            pool->append(users + sockfd);
                        } 
                        else 
                        {
                            

                            http_conn::timerList.del_timer(users[sockfd].timer);
                            users[sockfd].close_conn();
                        }

                    }
                    else
                    {
                        if( events[i].events & EPOLLOUT ) 
                        {
                            //检测到对方已经准备好接收数据
                            if(!users[sockfd].write())
                            {
                                
                                http_conn::timerList.del_timer(users[sockfd].timer);
                                users[sockfd].close_conn();
                            }

                        }
                    }

                }
            }
        }
    }
    
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}