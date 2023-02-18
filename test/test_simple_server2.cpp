#include <signal.h>
#include <thread>

#include "test/test_simple_server2.h"
#include "test/test_worker_threadpool.h"
#include "configuration.h"
#include "log.h"

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
        LOG_DEBUG("TestSimpleServer2 failed: failed to new RdmaServer<TestWorkerThreadpool>");
        return;
    }
    // 启动RdmaServer
    if (simple_server->Run() != 0)
    {
        LOG_DEBUG("TestSimpleServer2 failed: failed to Run RdmaServer");
        return;
    }

    worker_threadpool->SetSimpleServer(simple_server);
    // 启动工作线程池
    if (worker_threadpool->Run() != 0) {
        LOG_DEBUG("TestSimpleServer2 failed: failed to Run TestWorkerThreadpool");
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
    std::string remote_ip = SERVER_IP; 
    int remote_port = 21001;
    CommonRdmaClient *rdma_client = nullptr;
    // test_threads用于发起请求
    std::thread **test_threads = nullptr;

    SCOPEEXIT([&]() {
        if (test_threads != nullptr) {
            for (int i = 0; i < this->_num_test_thread; ++i) {
                if (test_threads[i] != nullptr) {
                    test_threads[i]->join();
                }
                delete test_threads[i];
                test_threads[i] = nullptr;
            }
            delete[] test_threads;
            test_threads = nullptr;
        }
        if (rdma_client != nullptr) {
            delete rdma_client;
        }

        LOG_DEBUG("TestSimpleServer2 pass");
    });

    try
    {
        rdma_client = new CommonRdmaClient(_slot_size, _slot_num, remote_ip, remote_port, _node_num);
    }
    catch (...)
    {
        LOG_DEBUG("TestSimpleServer2 failed, failed to new CommonRdmaClient");
        return;
    }

    if (rdma_client->Run() != 0) {
        LOG_DEBUG("TestSimpleServer2 failed, failed to run CommonRdmaClient");
    }

    auto func = [&] () {
        char content[20] = "zhouhuahui";
        int length = sizeof(int) + strlen(content) + 1;
        char send_buf[1000];
        char *pointer = send_buf;
        memcpy(pointer, reinterpret_cast<char *>(&length), sizeof(int));
        pointer += sizeof(int);
        memcpy(pointer, content, strlen(content) + 1);
        
        for (int i = 0; i < this->_reqs_per_test_thread; ++i) {
            rdma_client->PostRequest((void *)send_buf, length);
        }
    };
    test_threads = new std::thread*[_num_test_thread];
    for (int i = 0; i < _num_test_thread; ++i) {
        test_threads[i] = new std::thread(func);
    }
}

int main() {
    TestSimpleServer2Class test;
    if (IS_SERVER) {
        test.TestSimpleServer2(IsServer{}, 5, 100, 5, 64, 50);
    } else {
        // node_num: 5
        // slot_size: 64
        // slot_num: 50
        // num_test_thread: 100  有num_test_thread个线程同时来发送请求
        // reqs_per_test_thread: 10 每个线程发送reqs_per_test_thread个请求
        test.TestSimpleServer2(IsClient{}, 5, 64, 50, 1000, 100);
    }
}