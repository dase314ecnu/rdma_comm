#ifndef TEST_SHARED_CLIENT_H
#define TEST_SHARED_CLIENT_H

#include "test/test_simple_server2.h"
#include "myipc.h"

/** 
 * TestSharedClientMetricsClass
 * 基本和TestSharedClientClass的实现是一样的，不过这里主要关注性能评测：
 *   1. QPS 
 *   2. AvgLatency
 *   3. MaxLatency
 *   4. P99Latency
 *   5. MinLatency
 * 
 * 共享内存中有query_num, latencies数组以及它的大小latencies_size。每当backend执行完一个query之后，就会让query_num加1；
 * 每当backend执行完10个query之后，就计算总共的耗时，它除以10就是latency，这个latency会追加到
 * latencies数组中，latencies_size设置为最大100000，当它达到100000时，就不再追加了。
 * 有一个主进程会每隔1s钟计算query_num的变化，以及统计这一秒内的latencies的情况，并输出出来。
 * 
 * requirements: 
 *   1. 需要指定消息大小的区间
 *   2. USE_BUSY_POLL和USE_BACKEND_BUSY_POLL
 *   3. node_num, slot_num
 */

class TestSharedClientMetricsClass : public TestSimpleServer2Class {
protected:
    void runClient() override;

protected:
    char *_shared_memory = nullptr;
};

#endif