#ifndef WAITSET_H
#define WAITSET_H

#include <sys/epoll.h>

// epoll_create时最大的监听fd个数
#define MAX_EPOLL_SPACE (5)

/** 对epoll的封装 */
class WaitSet {
private:
    int epollfd;

public:
    /** 创造epollfd */
    WaitSet();
    /** 将fd设置为非阻塞，然后在epollfd中监听fd的读事件 */
    void addFd(int fd);
    /** 返回值是epoll_wait()的返回值；一次只读取一个epoll_event
     * 不设置超时时间
     */
    int waitSetWait(epoll_event *event);
};

#endif