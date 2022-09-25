#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

// 添加定时器，内部调用私有成员 add_timer
void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    // 若当前链表中只有头尾节点，直接插入
    if (!head)
    {
        head = tail = timer;
        return;
    }
    // 如果新的定时器超时时间小于当前头部结点 直接将当前定时器结点作为头部结点
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    // 否则调用私有成员，调整内部结点
    add_timer(timer, head);
}

// 当定时任务发生变化，调整对应定时器在链表中的位置
//    客户端在设定时间内有数据收发，则当前时刻对该定时器重新设定时间，这里只是往后延长超时时间
//    被调整的目标定时器在尾部，或定时器新的超时值仍然小于下一个定时器的超时，不用调整
//    否则先将定时器从链表取出，重新插入链表
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    // a. 被调整的定时器在链表尾部
    //    或者 定时器超时值仍然小于下一个定时器超时值，则不调整
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    // b. 被调整定时器是链表头结点，则将定时器取出，重新插入
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        // 调用私有成员，调整内部结点
        add_timer(timer, head);
    }
    // c. 被调整定时器在内部，则将定时器取出，重新插入
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}


void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    // a. 链表中只有一个定时器，需要删除该定时器
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    // b. 被删除的定时器为头结点
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    // c. 被删除的定时器为尾结点
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    // d. 被删除的定时器在链表内部，常规链表结点删除
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

// 定时任务处理函数
// 使用统一事件源，SIGALRM 信号每次被触发，
// 主循环中调用一次定时任务处理函数，处理链表容器中过期的定时器。
// 具体的逻辑如下
// a. 遍历定时器升序链表容器，从头结点开始依次处理每个定时器，直到遇到尚未过期的定时器
// b. 若当前时间小于定时器超时时间，跳出循环，即未找到过期的定时器
// c. 若当前时间大于定时器超时时间，即找到了过期的定时器，执行回调函数，
//    然后将它从链表中删除，然后继续遍历
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }
    
    time_t cur = time(NULL);
    util_timer *tmp = head;
    // a. 遍历定时器链表
    while (tmp)
    {
        // 链表容器为升序排列 
        // b. 如果当前时间小于定时器的超时时间，则后面的定时器也没有到期
        if (cur < tmp->expire)
        {
            break;
        }
        // c. 当前定时器到期，则调用回调函数，执行定时事件
        tmp->cb_func(tmp->user_data);
        // 将处理后的定时器从链表容器中删除，并重置头结点
        head = tmp->next;
        // 重置头结点不为空
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

// 私有成员, 被公有成员 add_timer 和 adjust_time 调用, 主要用于调整链表内部结点
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    // 遍历当前结点之后的链表，按照超时时间找到目标定时器对应的位置
    // 常规双向链表插入操作
    while (tmp)
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    // 遍历完发现，目标定时器需要放到尾结点处
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}


void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

// 对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，ET 模式，选择开启 EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    // 1 -> ET 模式
    if (1 == TRIGMode)
    {
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    }
    // 0 -> LT 模式
    else 
    {
        event.events = EPOLLIN | EPOLLRDHUP;
    }
    // 重置 EPOLLONESHOT 确保这个 socket 下一次可读
    if (one_shot)
        event.events |= EPOLLONESHOT; 
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 信号处理函数 信号处理函数中仅仅通过管道发送信号值，不处理信号对应的逻辑
// 缩短异步执行时间，减少对主程序的影响。
void Utils::sig_handler(int sig)
{
    // 为保证函数的可重入性，保留原来的 errno
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

// 设置信号函数 仅关注 SIGTERM (软件终止信号) 和 SIGALRM (计时器到时) 两个信号
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

// 静态变量类外初始化
int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
void cb_func(client_data *user_data)
{
    // 删除非活动连接在 socket 上的注册事件
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    // 关闭文件描述符
    close(user_data->sockfd);
    // 连接数减 1
    http_conn::m_user_count--;
}
