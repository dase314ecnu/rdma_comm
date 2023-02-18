#ifndef TEST_WORKER_THREADPOOL_H
#define TEST_WORKER_THREADPOOL_H

#include <stdint.h>
#include <pthread.h>
#include <vector>
#include <string>

#include "rdma_communication.h"

/** 
 * 一个简单的线程池，每个线程竞争同一个消息队列来获得消息来处理。
 * 这个线程池写的很简单，原因是不考虑性能，只是为了测试RdmaServer和RdmaClient的功能。
 */
class TestWorkerThreadpool {
  struct Args {
    TestWorkerThreadpool *test_worker_threadpool = nullptr;
  };

  struct Msg{
    /** 
     * 规定reqeust的协议是：
     * length of 4bytes, string content of (length - 4)bytes
     */
    void *request = nullptr;
    uint32_t node_idx = 0;
    uint32_t slot_idx = 0;
    
    static int parseLength(char *buf) {
      int length;
      char *c = reinterpret_cast<char *>(&length);
      memcpy((void *)c, buf, sizeof(int));
      return length;
    }
    static std::string parseContent(char *buf) {
      return std::string(buf);
    }
  };

  struct MsgQueue {
    MsgQueue(int _max_msg_num) : max_msg_num(_max_msg_num) {
        pthread_spin_init(&(this->lock), 0);
    }
    uint32_t max_msg_num = 0;
    std::vector<Msg> queue;
    pthread_spinlock_t lock;
  };

public:
  TestWorkerThreadpool(uint32_t _worker_num, uint32_t _max_msg_num);
  ~TestWorkerThreadpool();
  /** 
   * 选择一个线程来处理请求。Start()是RdmaServer访问工作线程池的接口。
   */
  void Start(void *request, uint32_t node_idx, uint32_t slot_idx);
  void SetSimpleServer(RdmaServer<TestWorkerThreadpool> *server);
  /** 
   * 启动线程池。
   */
  int  Run();
  /** 
   * 将所有工作线程终止掉
   */
  void Stop();

private:
  /** 
   * 不断竞争msg_queue，如果得到一个消息，则将消息内容通过simple_server回复回去
   */
  void workerThreadFun();
  static void *workerThreadFunEntry(void *arg);

private:
  /** 
   * 接收线程池，聚合关联方式。
   */
  RdmaServer<TestWorkerThreadpool> *simple_server = nullptr; 
  
  pthread_t **worker_threads = nullptr;  // 工作线程池
  uint32_t    worker_num = 0;            // 工作线程池的数量
  MsgQueue   *msg_queue = nullptr;       // 很多工作线程都要竞争的消息队列
  volatile bool    stop = false;              // 工作线程是否要停止工作
};

#endif