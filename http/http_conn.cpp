#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

// 定义 http 响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

// 全局变量
// 锁 用户名和密码
locker m_lock;
// 将数据库中的用户名和密码载入到服务器的 map 中来，map 中的 key 为用户名，value 为密码
map<string, string> users;

// 初始化 mysql 载入数据库表
void http_conn::initmysql_result(connection_pool *connPool)
{
    // 先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    // 在 user 表中检索 username，passwd 数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行，将对应的用户名和密码，存入 map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，ET 模式，选择开启 EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    // 1 -> ET 模式
    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    // 0 -> LT 模式
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    // 重置 EPOLLONESHOT 确保这个 socket 下一次可读
    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);  // 设置文件描述符非阻塞 
}

// 从 epoll 内核事件表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// epoll 重置 socket 上 EPOLLONESHOT 事件，以确保下一次可读时， EPOLLIN 事件能触发
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    // op: EPOLL_CTL_MOD 修改已经注册的 fd 的监听事件
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 静态变量初始化
// 所有的客户数 初始化为 0
int http_conn::m_user_count = 0;
// 所有 socket 上的事件都被注册到同一个 epoll 内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;

// 关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;  
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);  // 添加到 epoll 对象中
    m_user_count++;  // 总的用户数 +1

    // 当浏览器出现连接重置时
    // 可能是网站根目录出错或 http 响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

// 初始化新接受的连接
// check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;  // 主状态机初始化状态为解析请求首行
    m_linger = false;  // HTTP 请求是否要保持连接 默认不保持链接  Connection : keep-alive 保持连接
    m_method = GET;  // HTTP 请求方法 get post
    m_url = 0;  // 客户请求目标文件的文件名
    m_version = 0;  // 协议版本，只支持 HTTP1.1
    m_content_length = 0;  // HTTP 请求的消息总长度
    m_host = 0;  // 主机名
    m_start_line = 0;  // 当前m_read_buf正在解析的行的起始位置
    m_checked_idx = 0;  // 当前正在分析的字符在读缓冲区 m_read_buf 的位置
    m_read_idx = 0;  // 标识读缓冲区 m_read_buf 中已经读入的客户端数据的最后一个字节的下一个位置
    m_write_idx = 0;  // 写缓冲区中待发送的字节数 buffer 中的长度
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);  // 读缓冲区
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);  // 写缓冲区
    memset(m_real_file, '\0', FILENAME_LEN);  // 客户请求的目标文件的完整路径
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 循环读取客户数据，直到无数据可读或对方关闭连接
// 非阻塞 ET 工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;  // 已经读取到的字节

    // LT 读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    // ET 读取数据
    else
    {
        while (true)
        {
            // 从套接字接收数据，存储在 m_read_buf 缓冲区
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                // 非阻塞 ET 工作模式下，需要一次性将数据读完
                if (errno == EAGAIN || errno == EWOULDBLOCK)  // 没有数据 阻塞在那里了
                    break;
                return false;
            }
            else if (bytes_read == 0)  // 对方关闭连接
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

// 主状态机的初始状态 CHECK_STATE_REQUESTLINE，解析请求行
// a. 调用 parse_request_line 函数解析请求行
// b. 解析函数从 m_read_buf 中解析 HTTP 请求行，获得请求方法、目标 URL 及 HTTP 版本号
// c. 解析完成后主状态机的状态变为 CHECK_STATE_HEADER
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    // 取出数据，并通过与 GET 和 POST 比较，以确定请求方式
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    // 当 url 为 / 时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析 http 请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 判断是空行还是请求头
    if (text[0] == '\0')
    {
        // 判断是 GET 还是 POST 请求
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    // 解析请求头部连接字段
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;  // 如果是长连接，则将 linger 标志设置为 true
        }
    }
    // 解析请求头部内容长度字段
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    // 解析请求头部 HOST 字段
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

// 判断 http 请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    // 判断 buffer 中是否读取了消息体
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        // POST 请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 通过 while 循环 封装主状态机 对每一行进行循环处理
// 此时 从状态机已经修改完毕 主状态机可以取出完整的行进行解析
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        // 主状态机的三种状态转移逻辑
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:  // 解析请求行
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:  // 解析请求头
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            // 作为 get 请求 则需要跳转到报文响应函数
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:  // 解析消息体 仅用于 POST 请求
        {
            ret = parse_content(text);
            // 完整解析 POST 请求后 跳转到报文响应函数
            if (ret == GET_REQUEST)
                return do_request();
            // 解析完消息体即完成报文解析，避免再次进入循环，更新 line_status
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    // 将初始化的 m_real_file 赋值为网站根目录
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    // 处理 cgi
    // 实现登录和注册校验
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        // 根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);  // 将 m_url + 2 加到 m_url_real 后
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        // 将用户名和密码提取出来
        // user=123&passwd=123
        char name[100], password[100];
        int i;
        // 以 & 为分隔符，前面的为用户名
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];  // m_string 存储请求头数据
        name[i - 5] = '\0';

        int j = 0;
        // 以 & 为分隔符，后面的是密码
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';
        // 同步线程注册校验
        if (*(p + 1) == '3')
        {
            // 如果是注册，先检测数据库中是否有重名的
            // 没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");
            // 判断 map 中能否找到重复的用户名
            if (users.find(name) == users.end())
            {
                // 向数据库中插入数据时，需要通过锁来同步数据
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();
                // 校验成功，跳转登录页面
                if (!res)
                    strcpy(m_url, "/log.html");
                // 校验失败，跳转注册失败页面
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        // 如果是登录，直接判断
        // 若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }
    // 如果请求资源为 /0，表示跳转注册界面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        // 将网站目录和 /register.html 进行拼接，更新到 m_real_file 中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 如果请求资源为 /1，表示跳转登录界面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        // 将网站目录和 /log.html 进行拼接，更新到 m_real_file 中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 如果请求资源为 /5，表示图片请求页面
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        // 将网站目录和 /picture.html 进行拼接，更新到 m_real_file 中
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 如果请求资源为 /6，表示视频请求页面
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        // 将网站目录和 /video.html 进行拼接，更新到 m_real_file 中
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 如果请求资源为 /7，表示关注页面
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        // 将网站目录和 /fans.html 进行拼接，更新到 m_real_file 中
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 如果以上均不符合，即不是登录和注册，直接将 url 与网站目录拼接
    // 这里的情况是 welcome 界面，请求服务器上的一个图片
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    // 通过 stat 获取请求资源文件信息，成功则将信息更新到 m_file_stat 结构体
    // 失败返回 NO_RESOURCE 状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;
    // 判断文件的权限，是否可读，不可读则返回 FORBIDDEN_REQUEST 状态
    if (!(m_file_stat.st_mode & S_IROTH))  // S_IROTH：其他组读权限
        return FORBIDDEN_REQUEST;
    // 判断文件类型，如果是目录，则返回 BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))  // S_ISDIR() 函数的作用是判断一个路径是不是目录
        return BAD_REQUEST;
    // 以只读方式获取文件描述符，通过 mmap 将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    // 表示请求文件存在，且可以访问
    return FILE_REQUEST;  // FILE_REQUEST: 文件请求，获取文件成功
}

// 另外，为了提高访问速度，通过 mmap 进行映射，将普通文件映射到内存逻辑地址。
// 之后需对内存映射区执行 munmap 操作  释放映射
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// write() 函数
// a. 服务器子线程调用 process_write 完成响应报文，随后注册 epollout 事件。
// b. 服务器主线程检测写事件，并调用 http_conn::write 函数将响应报文发送给浏览器端。
// 该函数具体逻辑如下：
// 在生成响应报文时初始化 byte_to_send，包括头部信息和文件数据大小。
// 通过 writev 函数循环发送响应报文数据，根据返回值更新 byte_have_send 和 iovec 结构体的指针和长度，
// 并判断响应报文整体是否发送成功。
// a. 若 writev 单次发送成功，更新 byte_to_send 和 byte_have_send 的大小，
//    若响应报文整体发送成功，则取消 mmap 映射，并判断是否是长连接.
//    长连接重置 http 类实例，注册读事件，不关闭连接，
//    短连接直接关闭连接
// b. 若 writev 单次发送不成功，判断是否是写缓冲区满了。
//    若不是因为缓冲区满了而失败，取消 mmap 映射，关闭连接
//    若 eagain 则满了，更新 iovec 结构体的指针和长度，并注册写事件，等待下一次写事件触发
//    (当写缓冲区从不可写变为可写，触发 epollout)
//    因此在此期间无法立即接收到同一用户的下一请求，但可以保证连接的完整性。
bool http_conn::write()
{
    int temp = 0;
    // 若要发送的数据长度为 0 表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        // 将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char *format, ...)
{
    // 如果写入内容超出 m_write_buf 大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    // 定义可变参数列表
    va_list arg_list;// 首先在函数里定义一个 VA_LIST 型的变量，这个变量是指向参数的指针;
    // 将变量 arg_list 初始化为传入参数 
    va_start(arg_list, format);  // 然后用 VA_START 宏初始化变量刚定义的 VA_LIST 变量;

    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    
    // 如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    // 更新 m_write_idx 位置
    m_write_idx += len;
    // 清空可变参列表
    va_end(arg_list);  // 最后用 VA_END 宏结束可变参数的获取。

 
    LOG_INFO("request:%s", m_write_buf);

    return true;
}

// 添加状态行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 添加消息报头，具体的添加文本长度、连接状态和空行
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}

// 添加 Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

// 添加文本类型，这里是 html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

// 添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

// 添加文本 content
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}


bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    // 除 FILE_REQUEST 状态外，其余状态只申请一个 iovec，指向响应报文写缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// HTTP 报文解析部分
/*
服务器接收 http 请求的流程 简单来讲
a. 浏览器端发出 http 连接请求
b. 服务器端主线程创建 http 对象接收请求并将所有数据读入对应 buffer，
c. 将该对象插入任务队列后，工作线程从任务队列中取出一个任务进行处理。
d. 各子线程通过 process 函数对任务进行处理，
e. 调用 process_read 函数和 process_write 函数分别完成报文解析与报文响应两个任务。
*/
// process() 函数 子线程调用此函数完成报文解析与报文响应两个任务
// 由线程池中的工作线程（子线程）调用，这是处理 HTTP 请求的入口函数
void http_conn::process()
{
    // 1. 解析 HTTP 请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    // 2. 生成响应
    // 响应其实有两块数据 
    // a. 一块是响应首行、响应头、响应空行 
    // b. 还有一块是真正的请求的数据 所以用分散写的方式写出去
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    // 注册并监听写事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);  // 重置 socket 上 EPOLLONESHOT 事件
}
