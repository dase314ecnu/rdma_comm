#include <fcntl.h> 
#include <unistd.h>
#include <exception>

#include "pgrac_waitset.h"

WaitSet::WaitSet() {
  /** 我们的系统使用epoll，应该不会监听太多fd，最多2个 */
  this->epollfd = epoll_create(MAX_EPOLL_SPACE);
  if (this->epollfd < 0) {
    throw std::bad_exception();
  }
  this->created = true;
}

WaitSet::~WaitSet() {
  if (this->created) {
    close(this->epollfd);
    this->epollfd = 0;
    this->created = false;
  }
}

int WaitSet::addFd(int fd) {
  epoll_event event;
  event.data.fd = fd;
  // 使用边缘触发
  event.events = EPOLLIN | EPOLLET; 
  // 将fd设置为non-blocking
  int old_option = fcntl(fd, F_GETFL);
  int new_option = old_option | O_NONBLOCK;
  if (fcntl(fd, F_SETFL, new_option) != 0) {
    return -1;
  }
  
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event) != 0) {
    return -1;
  }
  return 0;
}

int WaitSet::waitSetWait(epoll_event *event) {
  return epoll_wait(this->epollfd, event, 1, 500);
}