#ifndef TEST_SHARED_CLIENT_H
#define TEST_SHARED_CLIENT_H

#include "test/test_simple_server2.h"
#include "myipc.h"

/** 
 * TestSharedClientClass
 * 在一个客户端，一个服务器的场景下，客户端发送大量请求给服务器，服务器返回响应。
 * 观察客户端和服务器的线程池是否出现故障，是否会使得线程池负载均衡中。
 *   1. 客户端的发送线程池是否工作正常，且负载均衡
 *   2. 服务器的接收线程池是否工作正常，且负载均衡
 *   3. 服务器的工作线程池是否工作正常，且负载均衡
 *   4. 改变参数：node_num, slot_num, slot_size, 请求队列长度等，看上述要求是否依然符合
 * 
 * 客户端是SharedRdmaClient实现
 */

class TestSharedClientClass : public TestSimpleServer2Class {
public:
    void TestSharedClient();
protected:
    void runClient() override;

protected:
    char *_shared_memory = nullptr;
};

#endif