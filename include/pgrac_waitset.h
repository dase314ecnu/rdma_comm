#ifndef WAITSET_H
#define WAITSET_H

#include <sys/epoll.h>

// epoll_create时最大的监听fd个数
#define MAX_EPOLL_SPACE (5)

/** 对epoll的封装 */
class WaitSet {
private:
    int epollfd = 0;
    bool created = false; // epollfd是否被创建出来

public:
    /** 创造epollfd */
    WaitSet();
    ~WaitSet();
    /** 
     * 将fd设置为非阻塞，然后在epollfd中监听fd的读事件 
     * @return 0: 成功 -1: 出现异常
     */
    int addFd(int fd);
    /** 
     * 返回值是epoll_wait()的返回值；一次只读取一个epoll_event
     * 超时时间是500ms
     */
    int waitSetWait(epoll_event *event);
};

#endif