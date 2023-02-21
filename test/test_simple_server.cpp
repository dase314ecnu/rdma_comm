#include <unistd.h>
#include <string>
#include <cstring>
#include <signal.h>

#include "test/test_simple_server.h"
#include "rdma_communication.h"
#include "configuration.h"
#include "log.h"
#include "inner_scope.h"

// 是否停止服务器
int stop_server = 0;

void sigint_handler(int sig) {
    printf("receive SIG_INT signal\n");
    stop_server = 1;
}

/**
 * ------------------------------------------------------------------------------------------
 * TestSimpleServerClass
 * --------------------------------------------------------------------------------------------
 */
void TestSimpleServerClass::TestSimpleServer(bool is_server)
{
    if (is_server)
    {
        this->runServer();
    }
    else
    {
        this->runClient();
    }
}

void TestSimpleServerClass::runServer()
{
    int rc = 0;

    uint32_t worker_num = 5;
    uint32_t max_msg_num = 5;
    TestWorkerThreadpool *worker_threadpool = nullptr;

    uint32_t node_num = 2;
    uint32_t slot_size = 64;
    uint32_t slot_num = 5;
    int listen_port = 21001;
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

    worker_threadpool = new TestWorkerThreadpool(worker_num, max_msg_num);
    try
    {
        simple_server = new RdmaServer<TestWorkerThreadpool>(
            TOTAL_COMPUTE_NUM, node_num, slot_size, slot_num, listen_port, worker_threadpool);
    }
    catch (...)
    {
        LOG_DEBUG("TestSimpleServer failed: failed to new RdmaServer<TestWorkerThreadpool>");
        return;
    }
    // 启动RdmaServer
    if (simple_server->Run() != 0)
    {
        LOG_DEBUG("TestSimpleServer failed: failed to Run RdmaServer");
        return;
    }

    worker_threadpool->SetSimpleServer(simple_server);
    // 启动工作线程池
    if (worker_threadpool->Run() != 0) {
        LOG_DEBUG("TestSimpleServer failed: failed to Run TestWorkerThreadpool");
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

void TestSimpleServerClass::runClient()
{
    uint32_t node_num = 2;
    uint32_t slot_size = 64;
    uint32_t slot_num = 5;
    std::string remote_ip = SERVER_IP; 
    int remote_port = 21001;
    CommonRdmaClient *rdma_client = nullptr;

    SCOPEEXIT([&]()
              {
    if (rdma_client != nullptr) {
        delete rdma_client;
    } 
    });

    try
    {
        rdma_client = new CommonRdmaClient(slot_size, slot_num, remote_ip, remote_port, node_num);
    }
    catch (...)
    {
        LOG_DEBUG("TestSimpleServer failed, failed to new CommonRdmaClient");
        return;
    }

    if (rdma_client->Run() != 0) {
        LOG_DEBUG("TestSimpleServer failed, failed to run CommonRdmaClient");
    }

    // 发送消息
    char content[20] = "zhouhuahui";
    int length = sizeof(int) + strlen(content) + 1;
    char send_buf[100];
    char *pointer = send_buf;
    memcpy(pointer, reinterpret_cast<char *>(&length), sizeof(int));
    pointer += sizeof(int);
    memcpy(pointer, content, strlen(content) + 1);
    rdma_client->PostRequest((void *)send_buf, length);

    LOG_DEBUG("TestSimpleServer pass");
}

#ifdef TEST_SIMPLE_SERVER
int main() {
    TestSimpleServerClass test;
    test.TestSimpleServer(IS_SERVER);
}

#endif