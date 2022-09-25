#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
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
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:
    // 设置读取文件的名称 m_real_file 大小
    static const int FILENAME_LEN = 200;
    // 设置读缓冲区 m_read_buf 大小
    static const int READ_BUFFER_SIZE = 2048;
    // 设置写缓冲区 m_write_buf 大小
    static const int WRITE_BUFFER_SIZE = 1024;
    // 报文的请求方法，目前本项目只会用到 GET 和 POST
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    // 主状态机
    // 解析客户端请求时，主状态机的状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,  // 当前正在分析请求行
        CHECK_STATE_HEADER,           // 当前正在分析头部字段
        CHECK_STATE_CONTENT           // 当前正在解析请求体
    };
    /*
        服务器处理 HTTP 请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完整的客户请求
        BAD_REQUEST         :   表示客户请求语法错误或请求资源为目录
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    // 从状态机
    // 解析客户端请求时，从状态机的三种可能状态，即行的读取状态，分别表示
    enum LINE_STATUS
    {
        LINE_OK = 0,   // 1.读取到一个完整的行
       LINE_BAD,      // 2.行出错
       LINE_OPEN      // 3.行数据尚且不完整
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    // 同步线程初始化数据库读取表
    void initmysql_result(connection_pool *connPool);
    // map 映射
    int timer_flag;
    int improv;


private:
    void init();
    //从 m_read_buf 读取，并处理请求报文
    HTTP_CODE process_read();
    // 向 m_write_buf 写入响应报文数据
    bool process_write(HTTP_CODE ret);
    // 下面这一组函数被 process_read 调用以分析 HTTP 请求
    // 主状态机解析报文中的请求行数据
    HTTP_CODE parse_request_line(char *text);
    // 主状态机解析报文中的请求头数据
    HTTP_CODE parse_headers(char *text);
    // 主状态机解析报文中的请求内容
    HTTP_CODE parse_content(char *text);
    //生成响应报文
    HTTP_CODE do_request();

    // m_start_line 是已经解析的字符
    // get_line 用于将指针向后偏移，指向未处理的字符
    char *get_line() { return m_read_buf + m_start_line; };
    // 从状态机 负责读取一行，分析是请求报文的哪一部分
    LINE_STATUS parse_line();

    void unmap();
    // 根据响应报文格式，生成对应 8 个部分
    // 以下函数均由 process_write do_request 调用以填充 HTTP 应答
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    // 所有的 socket 上的事件都被注册到同一个 epoll 对象中
    static int m_epollfd;
    // 统计用户的数量
    static int m_user_count;
    MYSQL *mysql;
    int m_state;  // 读为 0, 写为 1

private:
    int m_sockfd;
    sockaddr_in m_address;
    // 读缓冲区 其实就是一个数组 存储读取的请求报文数据
    char m_read_buf[READ_BUFFER_SIZE];
    // 标识读缓冲区 m_read_buf 中已经读入的客户端数据的最后一个字节的下一个位置
    int m_read_idx;
    // 当前正在分析的字符在读缓冲区 m_read_buf 的位置
    int m_checked_idx;
    // 当前 m_read_buf 正在解析的行的起始位置
    int m_start_line;
    // 写缓冲区 存储发出的响应报文数据
    char m_write_buf[WRITE_BUFFER_SIZE];
    // 写缓冲区中待发送的字节数 buffer 中的长度
    int m_write_idx;
    // 主状态机当前所处的状态
    CHECK_STATE m_check_state;
    // HTTP 请求方法 get post
    METHOD m_method;

    // 以下为解析请求报文中对应的 6 个变量
    // 客户请求的目标文件的完整路径，
    // 其内容等于 doc_root + m_url, doc_root是网站根目录
    char m_real_file[FILENAME_LEN];
    // 客户请求目标文件的文件名
    char *m_url;
    // 协议版本，只支持 HTTP1.1
    char *m_version;
    // 主机名
    char *m_host;
    // HTTP 请求的消息总长度
    int m_content_length;
    // HTTP 请求是否要保持长连接
    bool m_linger;
    // 客户请求的目标文件被 mmap 到内存中的起始位置 == 读取服务器上的文件地址
    char *m_file_address;
    // 目标文件的状态
    // 通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct stat m_file_stat;
    // io 向量机制 iovec
    // 将采用 writev 来执行写操作，所以定义下面两个成员
    // 其中 m_iv_count 表示被写内存块的数量。
    struct iovec m_iv[2];
    int m_iv_count;
    
    int cgi;        // 是否启用的 POST
    char *m_string; // 存储请求头数据
    int bytes_to_send;  // 剩余发送字节数
    int bytes_have_send;  // 已发送字节数
    char *doc_root;

    map<string, string> m_users;
    int m_TRIGMode;  // 0 -> LT 模式  1 -> ET 模式 默认 LT
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
