# websever
一个轻量http服务器项目，采用线程池+非阻塞socket+epoll同步IO模拟Proactor事件处理模式，使用状态机处理http请求。

# 功能
处理httpGET请求，返回请求数据。

# 压力测试
将服务器运行在腾讯云轻量应用服务器上，在本地用webbench进行压力测试，3000并发量持续30s测试通过。
