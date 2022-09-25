#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>  // 内存映射 mmap() 函数的头文件
#include <stdarg.h>  // 让函数能够接收不定量参数
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>  // readv() writev() 函数的头文件

#include <time.h>
#include "../log/log.h"

// 连接资源结构体成员需要用到定时器类
// 这里需要前向声明
class util_timer;

// 开辟用户 socket 结构 对应于最大处理 fd
// 连接资源
struct client_data
{
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};

class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    // 超时时间
    time_t expire;

    // 回调函数
    void (*cb_func) (client_data *);

    // 连接资源
    client_data * user_data;

    // 前向定时器
    util_timer * prev;

    // 后继定时器
    util_timer * next;
};

class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();
    // 添加定时器，内部调用私有成员 add_timer
    // 若当前链表中只有头尾节点, 直接插入, 否则, 将定时器按升序插入
    void add_timer(util_timer *timer);
    // adjust_timer 函数，当定时任务发生变化,调整对应定时器在链表中的位置
    void adjust_timer(util_timer *timer);
    // del_timer 函数将超时的定时器从链表中删除
    void del_timer(util_timer *timer);
    // 定时任务处理函数
    void tick();

private:
// 添加定时器，私有成员 add_timer
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head;// 头节点
    util_timer * tail;  // 尾节点
};

class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    // 对文件描述符设置非阻塞
    int setnonblocking(int fd);

    // 将内核事件表注册读事件，ET 模式，选择开启 EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    // 信号处理函数
    static void sig_handler(int sig);

    // 设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    // 定时处理任务，重新定时以不断触发 SIGALRM 信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;
    sort_timer_lst m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT;
};

void cb_func(client_data *user_data);

#endif
