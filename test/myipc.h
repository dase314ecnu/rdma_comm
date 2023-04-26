#ifndef MYIPC_H
#define MYIPC_H

/** 
 * 对进程间通信的封装，如共享内存
 */

#include "sys/shm.h"

class MySharedMemory {
public:
// zhouhuahui test
// private:
  int     shmid = 0;
  key_t   key = 0;
  char   *shmadd = nullptr;
  size_t  size = 0;

public:
  MySharedMemory(size_t size = 1024);
  ~MySharedMemory();
  char* CreateSharedMemory(size_t size);
  void DestroySharedMemory();
  char* GetSharedMemory();
};

#endif