#include <strings.h>

#include "test/myipc.h"

MySharedMemory::MySharedMemory(size_t size) {
  CreateSharedMemory(size);
}

MySharedMemory::~MySharedMemory() {
  DestroySharedMemory();
}

char* MySharedMemory::CreateSharedMemory(size_t size) {
  if ((this->key = ftok("./", 2023)) == -1) {
    return nullptr;
  }
  // 创建共享内存
  if ((this->shmid = shmget(this->key, size, IPC_CREAT | 0666)) < 0) {
    return nullptr;
  }
  // 映射
  if ((this->shmadd = (char *)shmat(shmid, nullptr, 0)) < 0) {
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