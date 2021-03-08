#ifndef HTTP_CONN_H
#define HTTP_CONN_H
#include <arpa/inet.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <string.h>
#include <errno.h>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/uio.h>
#include <stdarg.h>
#include <cstdio>
#include "lst_timer.h"

//任务类
class http_conn
{
public:
    static const int FILENAME_LEN = 200;            //请求文件名的最大长度
    static const int READ_BUFFER_SIZE = 2048;       //读缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;      //写缓冲区的大小

    static sort_timer_list<http_conn> timerList;   //共享的定时器链表
    timer_node<http_conn>* timer;                   //自己拥有的定时器


    //HTTP请求方法
    enum METHOD {GET= 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};

    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE {CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT};
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE {NO_REQUEST = 0, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION};
    
    // 从状态机的三种可能状态，即行的读取状态，分别表示
    // 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    enum LINE_STATUS {LINE_OK = 0, LINE_BAD, LINE_OPEN};
public:
    http_conn(){}
    ~http_conn(){}
public:
    void init(int sockfd, const sockaddr_in& addr);                         //初始化新接受的连接
    void close_conn();                                                      //关闭连接
    void process();                                                         //处理客户端请求
    bool read();                                                            //非阻塞读
    bool write();                                                           // 非阻塞写
private:
    void init();                                                            //初始化类自身的数据


    HTTP_CODE process_read();                                               //解析HTTP请求
    bool process_write(HTTP_CODE ret);                                      //填充HTTP应答

    // 下面这一组函数被process_read调用以解析HTTP请求
    HTTP_CODE parse_request_line(char* text);                               //解析请求行
    HTTP_CODE parse_headers(char* text);                                    //解析请求头部
    HTTP_CODE parse_content(char* text);                                    //解析请求数据
    char* get_line() {return m_read_buf + m_start_line;}                    //返回新的一行的开头
    LINE_STATUS parse_line();                                               //从读缓冲区中获取完整的一行数据


    HTTP_CODE do_request();                                                 //响应GET请求的网页文件

    // 这一组函数被process_write调用以填充HTTP应答。
    void unmap();                                                           //解除文件映射
    bool add_response(const char* format, ...);                             //往写缓冲中写入要发送的数据

    bool add_content(const char* content);                                  //写入响应正文
    bool add_status_line(int status, const char* title);                    //写入状态行

    bool add_headers(int content_length);                                   //写入响应头部，调用下边四个函数。
        bool add_content_type();                                            //写入响应头部中的content_type
        bool add_content_length(int content_length);                        //写入响应头部中的 content_length
        bool add_linger();                                                  //写入响应头部中的Connection
        bool add_blank_line();                                              //写入空行

public:
    static int m_epollfd;                   //所有用户共享的epoll对象
    static int m_user_count;                //统计任务数量, 一个任务就是一个用户

private:
    int m_sockfd;                           //该任务的socket文件描述符
    sockaddr_in m_address;                  //该任务的TCP通信socket地址
    

    char m_read_buf[READ_BUFFER_SIZE];      //该用户的读缓冲区
    int m_read_bytes;                       //读缓冲区等待读取的字节数
    int m_checked_idx;                      //正在分析的字符在读缓冲区中的下标 
    int m_start_line;                       //当前正在解析的行的起始位置

    CHECK_STATE m_check_state;              //主状态机当前所处状态
    METHOD m_method;                        //请求状态

    char m_real_file[FILENAME_LEN];         // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录
    char* m_url;                            // 客户请求的目标文件的文件名
    char* m_version;                        // HTTP协议版本号，我们仅支持HTTP1.1
    char* m_host;                           // 主机名
    int m_content_length;                   // HTTP请求数据段总长度(可能被压缩)
    bool m_linger;                          // HTTP请求是否要求保持连接

    char m_write_buf[WRITE_BUFFER_SIZE];    // 写缓冲区
    int m_write_bytes;                      // 写缓冲区中待发送的字节数
    char* m_file_address;                   // 客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;                // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息


    struct iovec m_iv[2];                   // 我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量。
    int m_iv_count;

};


#endif