#ifndef TEST_SHARED_MEMORY_H
#define TEST_SHARED_MEMORY_H

/** 
 * 测试SharedRdmaClient能否实现：
 *   1. 共享内存
 *   2. 进程/线程间信号量同步机制
 */

#include "pgrac_rdma_communication.h"
#include "myipc.h"

class SharedRdmaClient;

class TestSharedMemoryClass {
private:
  MySharedMemory   *mem = nullptr;
  SharedRdmaClient *client = nullptr;
  uint64_t          slot_size = 0;
  uint64_t          slot_num = 0;
  uint32_t          node_num = 0;
public:
  /** 
   * 创建一个共享内存。
   * 在共享内存上创建SharedRdmaClient对象，并开启它的多线程服务。
   * 开启多个进程，分别向SharedRdmaClient对象的多个发送线程发送消息，并在对应的信号量上等待。
   */
  void TestSharedMemory();
  /** 
   * 一个线程监听一个文件描述符，如果有消息到，则输出出来
   */
  void Worker(uint32_t node_idx);
};

/** 
 * 线程运行的输入函数，最终调用TestSharedMemoryClass.Worker()
 */
void* TestSharedMemoryWorker(void* arg);

#endif