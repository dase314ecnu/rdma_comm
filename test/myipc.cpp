#include <strings.h>
#include <errno.h>

#include "test/myipc.h"
#include "pgrac_log.h"

MySharedMemory::MySharedMemory(size_t size) {
  CreateSharedMemory(size);
}

MySharedMemory::~MySharedMemory() {
  DestroySharedMemory();
}

char* MySharedMemory::CreateSharedMemory(size_t size) {
  LOG_DEBUG("MySharedMemory will create shared memory of %lu kB", size / 1024);
  
  this->size = size;

  if ((this->key = ftok("./", 1000)) == -1) {
    LOG_DEBUG("MySharedMemory failed to ftok, errno is %d", errno);
    return nullptr;
  }
  // 创建共享内存
  if ((this->shmid = shmget(this->key, size, IPC_CREAT | 0666)) < 0) {
    LOG_DEBUG("MySharedMemory failed to shmget, errno is %d", errno);
    return nullptr;
  }
  // 映射
  if ((this->shmadd = (char *)shmat(shmid, nullptr, 0)) == nullptr) {
    LOG_DEBUG("MySharedMemory failed to shmat, errno is %d", errno);
    return nullptr;
  }

  bzero((void *)this->shmadd, size);
  return this->shmadd;
}

void MySharedMemory::DestroySharedMemory() {
  shmdt((const void *)this->shmadd);
  shmctl(this->shmid, IPC_RMID, nullptr);
}

char* MySharedMemory::GetSharedMemory() {
  return this->shmadd;
}