#ifndef TEST_SIMPLE_SERVER2_H
#define TEST_SIMPLE_SERVER2_H

#include <stdint.h>

/** 
 * TestSimpleServerClass
 * 在一个客户端，一个服务器的场景下，客户端发送大量请求给服务器，服务器返回响应。
 * 观察客户端和服务器的线程池是否出现故障，是否会使得线程池负载均衡中。
 *   1. 客户端的发送线程池是否工作正常，且负载均衡
 *   2. 服务器的接收线程池是否工作正常，且负载均衡
 *   3. 服务器的工作线程池是否工作正常，且负载均衡
 *   4. 改变参数：node_num, slot_num, slot_size, 请求队列长度等，看上述要求是否依然符合
 */

struct IsServer {};
struct IsClient {};

class TestSimpleServer2Class {
public:
    // 服务器运行这个函数来启动
    void TestSimpleServer2(IsServer, uint32_t worker_num, uint32_t max_msg_num, int32_t node_num, 
            uint32_t slot_size, uint32_t slot_num);
    // 客户端运行这个函数来启动
    void TestSimpleServer2(IsClient, uint32_t node_num, uint32_t slot_size, uint32_t slot_num, 
            uint32_t num_test_thread, uint32_t reqs_per_test_thread);

private:
    void runServer();
    void runClient();

private:
    uint32_t _worker_num = 0;
    uint32_t _max_msg_num = 0;
    uint32_t _node_num = 0;
    uint32_t _slot_size = 0;
    uint32_t _slot_num = 0;

    uint32_t _num_test_thread;  // 有多少个线程同时发送请求
    uint32_t _reqs_per_test_thread; // 每个线程发送多少个请求
};

#endif