#include <unistd.h>
#include <sys/types.h>
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <sys/socket.h>
#include <iostream>
#include <semaphore.h>

#include "test/test_shared_memory.h"
#include "waitset.h"
#include "log.h"

/** 
 * TestSharedMemoryWorker的参数
 */
struct Args {
  TestSharedMemoryClass *test_obj;
  uint32_t               node_idx;  
};

void TestSharedMemoryClass::TestSharedMemory () {
  this->mem = new MySharedMemory(1024 * 1024);
  char *buffer = mem->GetSharedMemory();
  this->slot_size = 64;
  this->slot_num = 5;
  this->node_num = 5;
  this->client = (SharedRdmaClient *)buffer;
  buffer       += sizeof(SharedRdmaClient);
  // *(this->client) = SharedRdmaClient(this->slot_size, this->slot_num, 
  //     "", 0, this->node_num, buffer);
  new (this->client)SharedRdmaClient(this->slot_size, this->slot_num,
          "", 0, this->node_num, buffer);

  // 开启node_num个线程进行监听
  pthread_t *threads = new pthread_t[node_num];
  for (uint32_t i = 0; i < node_num; ++i) {
    Args *args = new Args();
    args->test_obj = this;
    args->node_idx = i;
    if (pthread_create(threads + i, nullptr, TestSharedMemoryWorker, args) != 0) {
      return;
    }
  }
  
  char msg = 'y';   // 要发送到缓冲区中的消息是一个字符
  for (uint64_t i = 0; i < slot_num * node_num; ++i) {
    int ret = fork();
    assert(ret >= 0);
    if (ret == 0) {
      // 子进程向对应的node中写入消息，并通过client.listend_fd通知对应的线程
      uint32_t node_idx = i / slot_num;
      uint64_t slot_idx = i % slot_num;
      close(this->client->listen_fd[node_idx][1]);

      char *content = (char*)this->client->rdma_queue_pairs[node_idx]->GetLocalMemory();
      content += this->slot_size * slot_idx;
      *content = msg;
      char *buf = reinterpret_cast<char*>(&slot_idx);
      send(this->client->listen_fd[node_idx][0], buf, 8, 0);

      sem_t *sem = &(this->client->awakes[node_idx].sems[slot_idx]);
      sem_wait(sem);

      return;
    }
  }

  for (uint32_t i = 0; i < node_num; ++i) {
    pthread_join(threads[i], nullptr);
  }
  
  if (this->client != nullptr) {
    this->client->Destroy();
    this->client = nullptr;
  }
  this->mem->DestroySharedMemory();
  std::cout << "TestSharedMemory: pass" << std::endl;
}

void TestSharedMemoryClass::Worker(uint32_t node_idx) {
  WaitSet waitset;
  (void) waitset.addFd(this->client->listen_fd[node_idx][1]);

  int remain = this->slot_num;
  while (remain > 0) {
    epoll_event event;
    int ret = waitset.waitSetWait(&event);
    if (ret < 0 && errno != EINTR) {
      return;
    }
    int fd = event.data.fd;
    if (fd == this->client->listen_fd[node_idx][1]) {
      char buf[8];
      while (true) {
        int number = 0;
        int r = 0;
        while (number < 8) {
          r = recv(fd, buf, 8 - number, 0);
          if (r <= 0) {
            if (number > 0) {
              continue;
            } else {
              break;
            }
          }
          number += r;
          if (number == 8) {
            uint64_t *slot_idx = reinterpret_cast<uint64_t*>(buf);
            char *content = (char*)this->client->rdma_queue_pairs[node_idx]->GetLocalMemory();
            content += this->slot_size * (*slot_idx);
            // std::cout << node_idx << ": " << *slot_idx << ": " << *content << std::endl;
            assert(*content == 'y');
            remain--;

            sem_t *sem = &(this->client->awakes[node_idx].sems[*slot_idx]);
            sem_post(sem);
          }
        }
        if (r <= 0) {
          break;
        }
      }
    }
  }
}

void* TestSharedMemoryWorker(void* arg) {
  Args* args = (Args*)arg;
  args->test_obj->Worker(args->node_idx);
  return args;
}

int main() {
  TestSharedMemoryClass test;
  test.TestSharedMemory();
}

/** 
 * 问题:
 * 1. 设计模式问题，如何设计类继承
 * 2. 异常处理如何做
 * 3. 鲁棒性和错误处理如何做
 */