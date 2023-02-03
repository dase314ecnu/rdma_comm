#ifndef MYIPC_H
#define MYIPC_H

/** 
 * 对进程间通信的封装，如共享内存
 */

#include "sys/shm.h"

class MySharedMemory {
private:
  int     shmid;
  key_t   key;
  char   *shmadd;
  size_t  size;

public:
  MySharedMemory(size_t size = 1024);
  ~MySharedMemory();
  char* CreateSharedMemory(size_t size);
  void DestroySharedMemory();
  char* GetSharedMemory();
};

#endif