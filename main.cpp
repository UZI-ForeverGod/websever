#include "http_conn.h"
#include "threadpool.h"
#include <signal.h>



#define MAX_FD 65535                //最大文件描述符个数
#define MAX_EVENT_NUMBER 10000      //监听的最大事件数量
#define TIMESLOT 5                  //超时时间系数
//初始化任务类中共享的定时器链表
sort_timer_list<http_conn> http_conn::timerList;

//向epoll中添加要监视的文件描述符
extern void addfd(int epollfd, int fd, bool one_shot);
//从epoll中删除要监听的文件描述符
extern void removefd(int epollfd, int fd);



//添加要捕捉的信号
//在被捕捉的信号处理过程中阻塞其他信号
void addsig(int sig, void(*handler)(int))
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
}

void timer_handler(int)
{
    // 定时处理任务，实际上就是调用tick()函数
    http_conn::timerList.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}


int main(int argc, char* argv[])
{
    if(argc <= 1)
    {
        printf("usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);

    //忽略SIGPIPE信号, 以防止向已断开TCP连接的socket发送数据时产生的信号
    addsig(SIGPIPE, SIG_IGN);
    //捕捉SIGALARM信号
    addsig(SIGALRM, timer_handler);


    //创建线程池
    threadpool<http_conn>* pool_point = new threadpool<http_conn>;


    //任务数组
    http_conn* users = new http_conn[MAX_FD];



    //创建监听socket， tcp
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);

    //地址结构
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);


    //端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //绑定端口
    bind(listenfd, (struct sockaddr*)&address, sizeof(address));

    //监听
    listen(listenfd, 5);



    //监听事件数组
    epoll_event events[MAX_EVENT_NUMBER];

    //创建epoll
    int epollfd = epoll_create(5);

    //监听listenfd, 不开epolloneshot
    addfd(epollfd, listenfd, false);

    //初始化任务类中共享的epollfd
    http_conn::m_epollfd = epollfd;

    //激活定时器
    alarm(TIMESLOT);
    

    while(true)
    {
        //阻塞等待
        printf("wait\n");
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
    
        //如果失败并且不是被信号打断
        if(number < 0 && errno != EINTR)
        {
            printf("epoll failure\n");
            break;
        }



        //成功, events中存放着响应事件
        for(int i = 0; i < number; ++i)
        {
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addr_length = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addr_length);
                //失败
                if(connfd < 0)
                {
                    perror("accept error");
                    continue;
                }

                //超过预定最大文件描述符
                if(http_conn::m_user_count >= MAX_FD)
                {
                    
                    close(connfd);
                    continue; 
                }
                printf("accept\n");
                
                //初始化任务数组并且将连接任务放置到epoll监听中
                users[connfd].init(connfd, client_address);

                
                //初始化定时器, 记录超时时间,并加入链表中
                timer_node<http_conn>* timer = new timer_node<http_conn>;
                timer->task = &users[connfd];
                timer->expire = time(nullptr) + 3 * TIMESLOT;
                users[connfd].timer = timer;
                http_conn::timerList.add_timer(timer);
                printf("insert\n");
                
                
            }
            else
            {
                if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
                {
                    //读关闭或者读写关闭或者错误
                    //EPOLLRDHUB 检测到对端已经关闭socket的写端，本端读不到任何数据
                    //EPOLLHUP 检测到socket正常关闭
                    //EPOLLERR 检测到对方socket异常关闭

                    //从定时器链表中删除该定时器
                    printf("对方断开连接\n");
                    http_conn::timerList.del_timer(users[sockfd].timer);
                    users[sockfd].close_conn();

                }
                else
                {
                    //socket读缓冲区有数据
                    if(events[i].events & EPOLLIN)
                    {
                        //先读取出全部数据再将任务加入到线程池中
                        //读取数据失败就断开连接
                        if(users[sockfd].read())
                        {
                            // 如果有数据发来，则我们要调整该连接对应的超时时间并且更新定时器在链表中的位置。
                            
                            if(users[sockfd].timer)
                            {
                                time_t cur = time(NULL);

                                users[sockfd].timer->expire = cur + 3 * TIMESLOT;
                                
                                
                                http_conn::timerList.update_timer(users[sockfd].timer);
                            }
                            
                            pool_point->append(users + sockfd);

                        }
                        else
                        {
                            //删除定时器
                            http_conn::timerList.del_timer(users[sockfd].timer);
                            users[sockfd].close_conn();
                        }
                        
                    }
                    else
                    {
                        //检测到socket写缓存有空闲
                        if(events[i].events & EPOLLOUT)
                        {

                            //将数据全部写出
                            //如果失败则关闭连接
                            if(!users[sockfd].write())
                            {
                                //删除定时器
                                http_conn::timerList.del_timer(users[sockfd].timer);
                                users[sockfd].close_conn();

                            }

                        }
                    }
                }
            }
        }
    }


    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool_point;
    return 0;



}
