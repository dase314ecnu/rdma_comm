#ifndef TEST_SIMPLE_SERVER_H
#define TEST_SIMPLE_SERVER_H

#include <stdint.h>
#include <pthread.h>
#include <vector>
#include <cstring>
#include <string>

#include "rdma_communication.h"
#include "test/test_worker_threadpool.h"

/**  
 * 实现一个简单的服务器。
 * RdmaServer端接收请求，将请求发给测试工作线程池，测试工作线程池会处理，然后再通过RdmaServer发送回复。
 * RdmaClient端发送请求，并等待接收回复。
 * 测试RdmaClient和RdmaServer的基本功能。
 * 
 * 至少需要两台机器来测试这个用例。一台机器执行RdmaServer和TestWorkerThreadpool，另一台机器执行
 * RdmaClient的逻辑。
 */

class TestSimpleServerClass {
public:
  void TestSimpleServer(bool is_server);

private:
  void runServer();
  void runClient();
};

#endif