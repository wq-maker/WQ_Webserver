#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

using namespace std;

class Log
{
public:
    //C++11以后,使用局部变量懒汉不用加锁
    static Log *get_instance()
    {
        static Log instance;
        return &instance;
    }
    // 异步写日志公有方法，调用私有方法 async_write_log 异步写日志
    static void *flush_log_thread(void *args)
    {
        Log::get_instance()->async_write_log();
    }
    // 可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);
    // 将输出内容按照标准格式整理
    void write_log(int level, const char *format, ...);
    // 强制刷新缓冲区
    void flush(void);

private:
    Log();  // 单例模式 私有化构造
    virtual ~Log();
    void *async_write_log()
    {
        string single_log;
        // 从阻塞队列中取出一个日志 string，写入文件
        // pop(T &item) 其中 item = m_array[m_front]; 通过引用返回取出队头元素 写入文件
        while (m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
    }

private:
    char dir_name[128]; // 路径名
    char log_name[128]; // log 文件名
    int m_split_lines;  // 日志最大行数
    int m_log_buf_size; // 日志缓冲区大小
    long long m_count;  // 日志行数记录
    int m_today;        // 因为按天分类,记录当前时间是那一天
    FILE *m_fp;         // 打开 log 的文件指针
    char *m_buf;
    block_queue<string> *m_log_queue; // 阻塞队列
    bool m_is_async;                  // 是否同步标志位
    locker m_mutex;
    int m_close_log; //关闭日志
};
// 这四个宏定义在其他文件中使用，主要用于不同类型的日志输出
#define LOG_DEBUG(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_INFO(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_WARN(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}
// 日志类中的方法都不会被其他程序直接调用，末尾的四个可变参数宏提供了其他程序的调用方法。
// 前述方法对日志等级进行分类，包括 DEBUG，INFO，WARN 和 ERROR 四种级别的日志。
#endif
