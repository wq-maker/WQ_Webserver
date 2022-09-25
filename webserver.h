#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>  // 供对 POSIX 操作系统 API 的访问功能的头文件
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>  // 提供宏 assert 的定义
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

const int MAX_FD = 65536;           // 最大文件描述符
const int MAX_EVENT_NUMBER = 10000; // 最大事件数
const int TIMESLOT = 5;             // 最小超时单位

class WebServer
{
public:

    WebServer();
    ~WebServer();

    void init(int port , string user, string passWord, string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model);
    // 线程池
    void thread_pool();

    // 数据库
    void sql_pool();

    // 日志
    void log_write();

    // 更改触发模式 LT ET
    void trig_mode();

    // 创建 listenfd 循环监听
    void eventListen();

    // 当服务器处于非关闭状态 用于处理事件
    void eventLoop();

    // 定时器
    void timer(int connfd, struct sockaddr_in client_address);
    void adjust_timer(util_timer *timer);
    void deal_timer(util_timer *timer, int sockfd);

    // 处理用户数据
    bool dealclientdata();

    // 信号
    bool dealwithsignal(bool& timeout, bool& stop_server);

    // 读事件
    void dealwithread(int sockfd);
    // 写事件
    void dealwithwrite(int sockfd);

public:
    //基础 监听端口
    int m_port;
    char *m_root;

    // 日志
    int m_log_write;
    int m_close_log;

    // 模型切换 Reactor Proactor 默认 Proactor
    int m_actormodel;

    // 管道通信
    int m_pipefd[2];

    // epoll 树根
    int m_epollfd;

    // 用于接受 HTTP 用户连接
    http_conn *users;

    //数据库相关
    connection_pool *m_connPool;  // 数据库连接池
    string m_user;         //登陆数据库用户名
    string m_passWord;     //登陆数据库密码
    string m_databaseName; //使用数据库名
    int m_sql_num;         // 数据库连接数

    // 线程池相关
    threadpool<http_conn> *m_pool;
    int m_thread_num;

    // epoll_event相关  注册节点事件 
    epoll_event events[MAX_EVENT_NUMBER];

    int m_listenfd;  // 监听 fd 申请一次
    int m_OPT_LINGER;  // 是否长链接
    int m_TRIGMode;  // 触发模式 ET+LT LT+LT LT+ET  ET+ET 
    int m_LISTENTrigmode;  // 监听触发模式 ET/LT
    int m_CONNTrigmode;    // 连接 ET/LT

    //定时器相关
    client_data *users_timer;
    Utils utils;
};
#endif
