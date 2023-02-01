#include <fcntl.h> 

#include "waitset.h"

WaitSet::WaitSet() {
  /** 我们的系统使用epoll，应该不会监听太多fd，最多2个 */
  this->epollfd = epoll_create(MAX_EPOLL_SPACE);
}

void WaitSet::addFd(int fd) {
  epoll_event event;
  event.data.fd = fd;
  // 使用边缘触发
  event.events = EPOLLIN | EPOLLET; 
  epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
  
  // 将fd设置为non-blocking
  int old_option = fcntl(fd, F_GETFL);
  int new_option = old_option | O_NONBLOCK;
  fcntl(fd, F_SETFL, new_option);
}

int WaitSet::waitSetWait(epoll_event *event) {
  return epoll_wait(this->epollfd, event, 1, -1);
}