#include "http_conn.h"

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 资源目录
const char* source_root = "/home/ubuntu/webservertest/webserver/resources";

//初始化类静态成员
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//设置文件描述符非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//向epoll中添加需要监听的文件描述符
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;
    if(one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //设置非阻塞模式配合ET运行模式的EPOLL
    //要不然在判断读完所有数据的时候就会一直阻塞, 而不是返回0
    setnonblocking(fd);
}

//从epoll中移除舰艇的文件描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    
}

//修改epoll中监听的文件描述符的监听内容, 并重置epolloneshot事件, 以确保下一次还可以坚挺到该文件描述符上的事件
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLET | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

//关闭连接
void http_conn::close_conn()
{
    //这个if判断防止被多次关闭
    if(m_sockfd != -1)
    {
        printf("close\n");
        //从epoll中移除监听事件
        removefd(m_epollfd, m_sockfd);
        //标记已经删除过
        m_sockfd = -1;
        //关闭socket
        close(m_sockfd);
        --m_user_count;
    }

}

//初始化该任务的连接
void http_conn::init(int sockfd, const sockaddr_in& addr)
{
    m_sockfd = sockfd;
    m_address = addr;


    //将socket加入epoll监听中, 打开epolloneshot
    int reuse = 1;
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    addfd(m_epollfd, m_sockfd, true);
    ++m_user_count;

    //初始化其他成员
    init();

    
}
//初始化该任务的其他成员
void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始状态为检查请求行
    m_linger = false;                           // 默认不保持链接  Connection : keep-alive保持连接
    m_method = GET;                             // 默认请求方式为GET
    m_url = nullptr;                            // URL为默认nullptr             
    m_version = nullptr;                        // http版本默认为nullptr
    m_content_length = 0;                       // 请求数据长度默认为0
    m_host = nullptr;                           // 请求主机默认为nullptr
    bzero(m_real_file, FILENAME_LEN);           // 初始化客户端请求文件的路径

    m_start_line = 0;                           // 正在解析的行的行起始位置
    m_checked_idx = 0;                          // 正在处理的字符在读缓冲区的位置
    m_read_bytes = 0;                           // 读缓冲区中已经读取的字节数
    bzero(m_read_buf, READ_BUFFER_SIZE);        // 初始化读缓冲区

    m_write_bytes = 0;                          // 写缓冲区中待读取的字节数
    bzero(m_write_buf, WRITE_BUFFER_SIZE);       // 初始化写缓冲区

}

//从socket一次性读取全部数据
bool http_conn::read()
{
    if(m_read_bytes >= READ_BUFFER_SIZE)
    {
        //读缓冲区暂时还存不下就退出
        return false;
    }

    //读取到的字节数
    int bytes_read = 0;
    while(true)
    {
        bytes_read = recv(m_sockfd, m_read_buf, READ_BUFFER_SIZE - m_read_bytes, 0);
        if(bytes_read == -1)
        {
            if(errno == EAGAIN)
            {
                //没有数据， 也就是读完了
                break;
            }
            else
            {
                //发生了其他错误
                return false;
            }
        }
        else
        {
            if(bytes_read == 0)
            {
                //对方关闭了连接
                return false;
            }
        }
        m_read_bytes += bytes_read;
    }

    //能到这里说明发来的数据已经全部读完
    return true;
}


//从读缓冲区中获取完整的一行数据
http_conn::LINE_STATUS http_conn::parse_line()
{
    //当前读取到的的字符
    char cur;
    for(; m_checked_idx < m_read_bytes; ++m_checked_idx)
    {
        cur = m_read_buf[m_checked_idx];
        if(cur == '\r')
        {
            if((m_checked_idx + 1) == m_read_bytes)
            {
                //如果当前已经是最后一个字符, 那说明当前行还是不完整, 还是需要继续读取
                return LINE_OPEN;
            }
            else
            {
                if(m_read_buf[m_checked_idx + 1] == '\n')
                {
                    //已经读取到完整的行
                    m_read_buf[m_checked_idx++] = '\0';
                    m_read_buf[m_checked_idx++] = '\0';
                    return LINE_OK;
                }
                else
                {
                    //如果下一个字符不是\n, 那说明格式错误
                    return LINE_BAD;
                }
            }

        }
        else
        {
            if(cur == '\n')
            {
                if((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r'))
                {
                    //已经读取到完整的行
                    m_read_buf[m_checked_idx - 1] = '\0';
                    m_read_buf[m_checked_idx++] = '\0';
                    return LINE_OK;
                }
                else
                {
                    //如果上一个字符不是\r, 那说明格式错误
                    return LINE_BAD;
                }
            }
        }
    }

    //能运行到这说明当前行还是不完整
    return LINE_OPEN;
}


//解析HTTP请求行
http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
    
    //GET /index.html HTTP/1.1
    m_url = strpbrk(text, " ");
    if(m_url == nullptr)
    {
        
        //如果找不到空格, 那说明是错误的格式
        return BAD_REQUEST;
    }


    //提取http请求方法
    //仅支持GET请求
    *m_url++ = '\0'; //GET\0/index.html HTTP/1.1
    char* method = text;
    if(strcasecmp(method, "GET") == 0)
    {

        m_method = GET;
    }
    else
    {
        
        //不是GET的请求, 都是错误格式
        return BAD_REQUEST;
    }

    //提取HTTP版本，仅支持HTTP/1.1
    m_version = strpbrk(m_url, " ");
    if(m_version == nullptr)
    {
        
        return BAD_REQUEST;
    }
    *m_version++ = '\0';//GET\0/index.html\0HTTP/1.1
    if(strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        
        //如果不是HTTP1.1, 都是格式错误
        return BAD_REQUEST;
    }

    //提取URL
    printf("%s", m_url);
    //有可能URI处是URL格式   //http://106.52.19.182:10000/index.html
    if(strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        //提取URI
        //查找m_url中第一个/位置
        m_url = strchr(m_url, '/');
    }

    //URI格式错误
    if ( !m_url || m_url[0] != '/' ) 
    {
        return BAD_REQUEST;
    }


    //检查请求行完毕，状态转变为检查请求头部状态
    m_check_state = CHECK_STATE_HEADER;


    //继续解析请求
    return NO_REQUEST;


}

//解析请求头部
/*
只解析了这三个头部
Connection:
Content-Length:
Host:

*/
http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{
    //如果遇到空行, 表示头部字段已经解析完毕
    if(text[0] == '\0')
    {
        //如果HTTP请求有消息体, 下一个状态就是解析消息体
        if(m_content_length != 0)
        {
            //状态转移到解析消息体
            m_check_state = CHECK_STATE_CONTENT;
            //继续解析请求
            return NO_REQUEST;
        }
        else
        {
            //如果没有消息体，那么说明HTTP请求已经解析完毕
            return GET_REQUEST;
        }

    }
    else
    {
        //提取Connection字段
        if(strncasecmp(text, "Connection:", 11) == 0)
        {
            text += 11;
            text += 1;//跳过值字段的空格
            if(strncasecmp(text, "keep-alive", 10) == 0)
            {
                printf("keep alive\n");
                m_linger = true;
            }
        }
        else
        {
            //提取Content-Length字段
            if(strncasecmp(text, "Content-Length:", 15) == 0)
            {
                text += 15;
                text += 1;//跳过值字段的空格
                m_content_length = atol(text);
                
            }
            else
            {
                //处理Host头部字段
                if(strncasecmp(text, "Host:", 5) == 0)
                {
                    text += 5;
                    text += 1;//跳过字段空格
                    m_host = text;

                }
                else
                {
                    //就解析那么多头部字段
                    ;
                }
            }
        }
    }


    //如果能运行到这里, 说明还需要继续解析HTTP请求数据部分
    return NO_REQUEST;
}

//解析请求报文中的请求数据
//这里并没有处理数据的逻辑, 只是单存读取数据
http_conn::HTTP_CODE http_conn::parse_content(char* text)
{
    if(m_read_bytes >= (m_content_length + m_checked_idx))
    {
        //数据已经读取完毕
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    else
    {
        //继续解析请求
        return NO_REQUEST;
    }
}


//有限状态机解析HTTP请求
http_conn::HTTP_CODE http_conn::process_read()
{
    //从状态机状态，解析行成功状态
    //主状态机初始状态, 解析请求行

    m_check_state = CHECK_STATE_REQUESTLINE;
    LINE_STATUS line_status = LINE_OK;

    HTTP_CODE ret = NO_REQUEST;

    char* text = nullptr;


    //如果读取到新的完整行, 或者该任务处在解析请求数据且一直读取不到完整行
    while(m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OPEN
            || (line_status = parse_line()) == LINE_OK)
    {
        
        //获取一行数据
        text = get_line();
        printf("%s\n", text);
        //前往下一行
        m_start_line = m_checked_idx;




        switch(m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST)
                {
                    printf("line\n");
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if(ret == BAD_REQUEST)
                {
                    printf("head\n");
                    return BAD_REQUEST;
                }
                else
                {
                    //已经获取到完整请求, 开始响应请求
                    if(ret == GET_REQUEST)
                    {
                        return do_request();
                    }
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                //已经获取到完整请求, 开始响应请求
                if(ret == GET_REQUEST)
                {
                    return do_request();
                }
                //数据还不够完整, 需要继续读取
                line_status = LINE_OPEN;
                break;
            }

        }
    }

    //能运行到这里, 说明请求还没解析完毕
    return NO_REQUEST;
    
}


//响应HTTP请求, 如果请求网页文件存在则返回, 否则报错
http_conn::HTTP_CODE http_conn::do_request()
{
    //获取请求的文件路径
    strcpy(m_real_file, source_root);
    int len = strlen(source_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);



    //获取所请求文件的相关信息
    if(stat(m_real_file, &m_file_stat) < 0)
    {
        //文件不存在
        return NO_RESOURCE;
    }

    //判断访问权限
    if(!m_file_stat.st_mode & S_IROTH)
    {
        //没有访问权限
        return FORBIDDEN_REQUEST;
    }


    //判断是否是目录
    if(S_ISDIR(m_file_stat.st_mode))
    {
        //不能请求目录
        return BAD_REQUEST;
    }



    //能运行到这说明文件存在, 并且可以访问
    

    int fd = open(m_real_file, O_RDONLY);
    //创建内存映射
    m_file_address = static_cast<char*>(mmap(nullptr, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    
    //创建后就可以关闭打开的文件
    close(fd);

    //获取文件成功
    return FILE_REQUEST;

}



// 对内存映射区执行munmap操作
void http_conn::unmap()
{
    if(m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = nullptr;
    }
}

//写HTTP响应
bool http_conn::write()
{
    int ret = 0;
    int bytes_have_send = 0;                    //已经发送的字节数
    int bytes_to_send = m_write_bytes;          //将要发送的字节


    if(bytes_to_send == 0)
    {
        //将要发送的字节为0, 这一次响应结束
        //重新等待有数据到来
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }


    while(true)
    {
        //分散写
        ret = writev(m_sockfd, m_iv, m_iv_count);

        if(ret <= -1)
        {
            //如果TCP写缓存没有空间, 则重新等待下一轮EPOLLOUT事件
            if(errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            else
            {
                
                //发生其他错误, 返回false并释放映射的内存
                unmap();
                return false;
            }
        }

        bytes_have_send += ret;

        //已经发完
        if(bytes_to_send <= bytes_have_send)
        {
            unmap();
            //判断是否需要保持连接
            if(m_linger)
            {
                //回到初始连接状态
                init();
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return true;
            }
            else
            {
                //不需要保持连接
                //重新注册一下epolloneshot
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return false;
            }

        }
    }


}


bool http_conn::add_response(const char* format, ...)
{
    //写缓冲区暂时不足
    if(m_write_bytes >= WRITE_BUFFER_SIZE)
    {
        return false;
    }

    //获得可变参数中第一个参数的地址
    va_list arg_list;
    va_start(arg_list, format);

    int len = vsnprintf(m_write_buf + m_write_bytes, 
        WRITE_BUFFER_SIZE - 1 - m_write_bytes, format, arg_list);

    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_bytes)) 
    {
        return false;
    }

    m_write_bytes += len;
    va_end(arg_list);

    return true;



}

//添加响应状态行
bool http_conn::add_status_line(int status, const char* title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
//添加响应头部
bool http_conn::add_headers(int content_len) 
{
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len) 
{
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}



// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
/*
只解析了以下请求状态
INTERNAL_ERROR
BAD_REQUEST
NO_RESOURCE
FORBIDDEN_REQUEST
FILE_REQUEST
*/
bool http_conn::process_write(HTTP_CODE ret) 
{
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form)) 
            {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_400_form)) 
            {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form)) 
            {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form)) 
            {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title);
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_bytes;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        default:
            return false;
    }
    
    //执行到这里说明是发生错误，要发送错误相关信息
    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_bytes;
    m_iv_count = 1;

    return true;
    
}




//任务处理函数
void http_conn::process()
{
    //解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    //准备好响应数据
    bool write_ret = process_write(read_ret);
    if(!write_ret)
    {
        close_conn();

        //关闭连接后直接退出
        return;
    }

    //如果process_write成功, 注册监听可写事件以及重新注册EPOLLONESHOT
    modfd(m_epollfd, m_sockfd, EPOLLOUT);


}