#include <signal.h>
#include <thread>

#include "test/test_simple_server2.h"
#include "test/test_worker_threadpool.h"
#include "pgrac_configuration.h"
#include "pgrac_log.h"

// 是否停止服务器
int stop_server = 0;

void sigint_handler(int sig) {
    printf("receive SIG_INT signal\n");
    stop_server = 1;
}

/** 
 * ---------------------------------------------------------------------------------------------
 * TestSimpleServer2Class
 * ---------------------------------------------------------------------------------------------
 */
void TestSimpleServer2Class::TestSimpleServer2(IsServer tag, uint32_t worker_num, uint32_t max_msg_num, 
        int32_t node_num, uint32_t slot_size, uint32_t slot_num)
{
    _worker_num = worker_num;
    _max_msg_num = max_msg_num;
    _node_num = node_num;
    _slot_size = slot_size;
    _slot_num = slot_num;

    runServer();
}

void TestSimpleServer2Class::TestSimpleServer2(IsClient tag, uint32_t node_num, uint32_t slot_size, 
        uint32_t slot_num, uint32_t num_test_thread, uint32_t reqs_per_test_thread) 
{
    _node_num = node_num;
    _slot_size = slot_size;
    _slot_num = slot_num;
    _num_test_thread = num_test_thread;
    _reqs_per_test_thread = reqs_per_test_thread;

    runClient();
}

void TestSimpleServer2Class::runServer() {
    int rc = 0;
    int listen_port = 21001;
    TestWorkerThreadpool *worker_threadpool = nullptr;
    RdmaServer<TestWorkerThreadpool> *simple_server = nullptr;

    SCOPEEXIT([&]()
              {
    if (simple_server != nullptr) {
        delete simple_server;
        simple_server = nullptr;
    } 
    if (worker_threadpool != nullptr) {
        delete worker_threadpool;
        worker_threadpool = nullptr;
    }
    });

    worker_threadpool = new TestWorkerThreadpool(_worker_num, _max_msg_num);
    try
    {
        simple_server = new RdmaServer<TestWorkerThreadpool>(
            TOTAL_COMPUTE_NUM, _node_num, _slot_size, _slot_num, listen_port, worker_threadpool);
    }
    catch (...)
    {
        LOG_DEBUG("server failed: failed to new RdmaServer<TestWorkerThreadpool>");
        return;
    }
    // 启动RdmaServer
    if (simple_server->Run() != 0)
    {
        LOG_DEBUG("server failed: failed to Run RdmaServer");
        return;
    }

    worker_threadpool->SetSimpleServer(simple_server);
    // 启动工作线程池
    if (worker_threadpool->Run() != 0) {
        LOG_DEBUG("server failed: failed to Run TestWorkerThreadpool");
    }

    // 注册一个信号处理函数，当CTRL+C到来时，先将simple_server销毁，再将worker_threadpool销毁
    {
        struct sigaction act, oldact;
        act.sa_handler = sigint_handler;
        sigaddset(&act.sa_mask, SIGQUIT);
        act.sa_flags = SA_NODEFER; 
        sigaction(SIGINT, &act, &oldact);
    }

    int timer = 10000;
    while (timer-- > 0) {
        if (stop_server != 0) {
            break; // 退出
        } 
        sleep(1);
    }
}

void TestSimpleServer2Class::runClient() {
    
}

#ifdef TEST_SIMPLE_SERVER2
int main() {
    TestSimpleServer2Class test;
    if (IS_SERVER) {
        // 由于是测试，所以我写的工作线程很简单，所有工作线程都从一个队列中取数据
        // 因此如果将工作线程的数量设置的太大，则会大大增加竞争，性能非常差
        test.TestSimpleServer2(IsServer{}, 3, 1000, 5, 64, 500);
    } else {
        // node_num: 5
        // slot_size: 64
        // slot_num: 500
        // num_test_thread: 500  有num_test_thread个线程同时来发送请求
        // reqs_per_test_thread: 100 每个线程发送reqs_per_test_thread个请求
        test.TestSimpleServer2(IsClient{}, 5, 64, 500, 500, 100);
    }
}
#endif

/**  
 * @todo: 为什么slot_num设置的过大，比如500，会造成wc.status出现错误码8和10
 * @todo: 实现批量发送，批量响应，批量将请求加入到请求队列
 */