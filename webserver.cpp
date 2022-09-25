#include "webserver.h"

WebServer::WebServer()
{
    // new http_conn 类对象数组 最大值为 65536
    users = new http_conn[MAX_FD];

    // root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    // 定时器
    // 创建客户端连接  clientfd 数组 最大值为 65536
    // 这意味着每个 user（http 连接） 都对应这其一个 定时器 client_data
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users; // 释放 http_conn 对象数组
    delete[] users_timer;  // 释放 client_data 对象数组
    delete m_pool;  // 释放 http 线程池
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;                  // 监听端口
    m_user = user;                  // 登陆数据库用户名
    m_passWord = passWord;          // 登陆数据库密码
    m_databaseName = databaseName;  // 使用数据库名
    m_sql_num = sql_num;            // 数据库连接数
    m_log_write = log_write;        // 写日志
    m_close_log = close_log;        // 是否关闭日志
    m_OPT_LINGER = opt_linger;      // 是否长链接
    m_TRIGMode = trigmode;          // 触发模式 ET+LT LT+LT LT+ET  ET+ET 
    m_thread_num = thread_num;      // 线程池数量
    m_actormodel = actor_model;     // 模型切换 
}

// 不同组合模式,默认listenfd LT + connfd LT
void WebServer::trig_mode()
{
    // LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    // LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    // ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    // ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

// 写日志
void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        //初始化日志
        if (1 == m_log_write)
        {
            // 如果设置了 max_queue_size, 则设置为异步写
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        }
        else
        {
            // 如果设置 max_queue_size 为 0 则设置为同步写
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
        }
    }
}

// 数据库连接池
void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //初始化数据库读取表
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    //线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

// cfd 需要 one_shot 而 lfd 不需要
// 统一事件源 -> 因为正常情况下，信号处理与 IO 处理不走一条路
// 这里的信号主要是指超时问题
// 具体的做法
// a. 信号处理函数使用管道将信号传递给主循环
// b. 信号处理函数往管道的写端写入信号值，主循环则从管道的读端读出信号值
// c. 使用 I/O 复用系统调用来监听管道读端的可读事件
// d. 这样信号事件与其他文件描述符都可以通过 epoll 来监测，从而实现统一事件源
void WebServer::eventListen()
{
    // 网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    // TCP 连接断开的时候调用 closesocket 函数，有优雅的断开和强制断开两种方式
    // 优雅关闭连接   opt_linger -> HTTP 请求是否要保持连接 默认不保持链接
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        // SO_LINGER 选项用来设置当调用 closesocket 时是否马上关闭 socket
        // 底层会将未发送完的数据发送完成后再释放资源，也就是优雅的退出
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        // 这种方式下，在调用 closesocket 的时候不会立刻返回，内核会延迟一段时间，
        // 这个时间就由 l_linger 的值来决定。
        // 如果超时时间到达之前，发送完未发送的数据(包括 FIN 包)并得到另一端的确认，
        // closesocket 会返回正确，socket 描述符优雅性退出。
        // 否则，closesocket 会直接返回 错误值，未发送数据丢失，
        // socket 描述符被强制性退出。需要注意的时，
        // 如果 socket 描述符被设置为非堵塞型，则 closesocket 会直接返回值。
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    // 允许重用本地地址和端口
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);
    // 设置定时器 最小超时时间 TIMESLOT = 5
    utils.init(TIMESLOT);

    // epoll 创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];  // MAX_EVENT_NUMBER = 10000; 最大事件数
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);
    // 将 lfd 上树 这里的上树封装为了 addfd 目的是为了可以更改模式
    // cfd 需要 one_shot 而 lfd 不需要
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;
    // 创建管道套接字 注册 pipefd[0]上的可读事件
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    // 设置管道写端为非阻塞
    utils.setnonblocking(m_pipefd[1]);
    // 设置管道读端为 非阻塞 并添加到 epoll 内核事件表 统一事件源 
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);
    // 对一个对端已经关闭的 socket 调用两次 write, 第二次将会生成 SIGPIPE 信号, 
    // 该信号默认结束进程
    // 因为 TCP 协议的限制, 一个端点无法获知对端的 socket 是调用了 close 还是 shutdown
    // 为了避免进程退出， 可以捕获 SIGPIPE 信号， 
    // 或者 忽略它， 给它设置 SIG_IGN 信号处理函数： signal(SIGPIPE, SIG_IGN);
    utils.addsig(SIGPIPE, SIG_IGN);
    // 传递给主循环的信号值，这里只关注 SIGALRM 和 SIGTERM
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT);

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    // 初始化 client_data 数据
    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;  // 3 倍阈值的超时
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

// 若有数据传输，则将定时器往后延迟 3 个单位
// 并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;  // 3 倍阈值的超时
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    // cb_func() 回调函数
    // 定时事件为回调函数，将其封装起来由用户自定义，
    // 这里是删除非活动 socket 上的注册事件，并关闭
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealclientdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    // LT
    if (0 == m_LISTENTrigmode)
    {
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        // 在 accept 得到 cfd 的时候 调用 timer()
        timer(connfd, client_address);
    }

    else
    {
        // ET 模式
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

// 与读写不同的是，这里的 signal 是处理函数，它不需要上队列
// 这里是通过管道的方式来告知 WebServer。
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    // 从管道读端读出信号值，成功返回字节数，失败返回 -1
    // 正常情况下，这里的 ret 返回值总是 1，只有 14 和 15 两个 ASCII 码对应的字符
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
                timeout = true;
                break;
            }
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

// 对于整个并发模式的思路，存在两个模式的切换：reactor 与 preactor（同步 io 模拟出）
// 它们的区别是对于数据的读取者是谁
// a. 对于 reactor 是同步线程来完成，整个读就绪放在请求列表上；
// b. 而对于 preactor 则是由主线程，也就是当前的 WebServer 进行一次调用，
//    读取后将读完成纳入请求队列上
// 同样，对于当前的 fd 我们要对他进行时间片的调整
// 同样的，当时间到期时，在定时器对象中，会有对应的下树操作
void WebServer::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    // 并发模型,默认是 proactor m_actormodel = 0;
    // reactor 模式
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        // 若监测到读事件，将该事件放入请求队列
        // append: 把新任务放到 list 的尾部，然后所有线程争夺 list 中的任务
        // append(T *request, int state) cfd
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        // proactor 模式  m_actormodel == 0
        // 主线程从这一 sockfd 循环读取数据, 直到没有更多数据可读
        // 这里就体现出，主线程来传递的是一个完成事件
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            // 若监测到读事件，将该事件放入请求队列
            // append_p(T *request) lfd
            // 然后将读取到的数据封装成一个请求对象并插入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    // reactor 模式
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        // proactor 模式
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        // 监测发生事件的文件描述符
        // 主线程调用 epoll_wait 等待一组文件描述符上的事件，
        // 并将当前所有就绪的 epoll_event 复制到 events 数组中
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }
        // 轮询有事件产生的文件描述符 
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            // 处理新到的客户连接
            // 当 listen 到新的用户连接，listenfd 上则产生就绪事件
            if (sockfd == m_listenfd)
            {
                bool flag = dealclientdata();
                if (false == flag)
                    continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 服务器端关闭连接，移除对应的定时器
                // 如有异常，则直接关闭客户连接，并删除该用户的 timer
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            // 处理信号
            // 管道读端对应文件描述符发生读事件
            // 因为统一了事件源，信号处理当成读事件来处理
            // 怎么统一？就是信号回调函数那里不立即处理而是写到：pipe 的写端
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            // 处理客户连接上接收到的数据
            // 当这一 sockfd 上有可读事件时，epoll_wait 通知主线程。
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT)
            {
                // 当这一 sockfd 上有可写事件时，epoll_wait 通知主线程。
                // 主线程往 socket 上写入服务器处理客户请求的结果
                dealwithwrite(sockfd);
            }
        }
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}