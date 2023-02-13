#include <unistd.h>
#include <string>
#include <cstring>

#include "test/test_simple_server.h"
#include "rdma_communication.h"
#include "configuration.h"
#include "log.h"

/**
 * ------------------------------------------------------------------------------------------
 * TestWorkerThreadpool
 * ------------------------------------------------------------------------------------------
 */
TestWorkerThreadpool::TestWorkerThreadpool(uint32_t _worker_num, uint32_t _max_msg_num)
    : worker_num(_worker_num)
{
    this->msg_queue = new MsgQueue(_max_msg_num);
    this->worker_threads = new pthread_t *[this->worker_num];
    for (int i = 0; i < this->worker_num; ++i)
    {
        this->worker_threads[i] = nullptr;
    }
}

TestWorkerThreadpool::~TestWorkerThreadpool()
{
    this->Stop();
    delete this->msg_queue;
}

void TestWorkerThreadpool::Start(void *request, uint32_t node_idx, uint32_t slot_idx)
{
    while (true)
    {
        pthread_spin_lock(&(this->msg_queue->lock));
        if (this->msg_queue->queue.size() < this->msg_queue->max_msg_num)
        {
            Msg msg;
            msg.request = request;
            msg.node_idx = node_idx;
            msg.slot_idx = slot_idx;
            this->msg_queue->queue.push_back(std::move(msg));
            pthread_spin_unlock(&(this->msg_queue->lock));
            break;
        }
        else
        {
            pthread_spin_unlock(&(this->msg_queue->lock));
            usleep(100);
            continue;
        }
    }
}

void TestWorkerThreadpool::SetSimpleServer(RdmaServer<TestWorkerThreadpool> *server) {
    this->simple_server = server;
}

int TestWorkerThreadpool::Run() {
//     this->send_threads = new pthread_t[this->node_num];
//   uint32_t i = 0;
//   SCOPEEXIT([&]() {
//     if (i < this->node_num) {
//       this->stop = true;
//       for (int j = 0; j < i; ++j) {
//         (void) pthread_join(this->send_threads[j], nullptr);
//       }
//       delete[] this->send_threads;
//       this->send_threads = nullptr;
//     }
//   });

//   for (; i < node_num; ++i) {
//     Args *args = new Args();
//     args->client = this;
//     args->node_idx = i;
//     if (pthread_create(send_threads + i, nullptr, this->sendThreadFunEntry, args) != 0) {
//       return -1;
//     }
//   }
//   return 0;
    
    for (int i = 0; i < this->worker_num; ++i) {
        
    }
}

void TestWorkerThreadpool::Stop()
{
    this->stop = true;
    if (this->worker_threads == nullptr)
    {
        return;
    }
    for (int i = 0; i < this->worker_num; ++i)
    {
        (void)pthread_join(*this->worker_threads[i], nullptr);
        delete this->worker_threads[i];
        this->worker_threads[i] = nullptr;
    }
    delete[] this->worker_threads;
    this->worker_threads = nullptr;
}

void TestWorkerThreadpool::workerThreadFun() {

}

void *TestWorkerThreadpool::workerThreadFunEntry(void *arg) {

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
    if (worker_threadpool != nullptr) {
        delete worker_threadpool;
    }
    if (simple_server != nullptr) {
        delete simple_server;
    } });

    worker_threadpool = new TestWorkerThreadpool(worker_num, max_msg_num);
    try
    {
        simple_server = new RdmaServer<TestWorkerThreadpool>(
            TOTAL_COMPUTE_NUM, node_num, slot_size, slot_num, listen_port, worker_threadpool);
    }
    catch (...)
    {
        LOG_DEBUG("TestSimpleServer failed");
        return;
    }
    // 启动RdmaServer
    if (simple_server->Run() != 0)
    {
        LOG_DEBUG("TestSimpleServer failed");
        return;
    }

    worker_threadpool->SetSimpleServer(simple_server);
    // 启动工作线程池
    worker_threadpool->Run();

    sleep(120);
}

void TestSimpleServerClass::runClient()
{
    uint32_t node_num = 2;
    uint32_t slot_size = 64;
    uint32_t slot_num = 5;
    std::string remote_ip = ""; // @todo
    int remote_port = 21001;
    CommonRdmaClient *rdma_client = nullptr;

    SCOPEEXIT([&]()
              {
    if (rdma_client != nullptr) {
        delete rdma_client;
    } });

    try
    {
        rdma_client = new CommonRdmaClient(slot_size, slot_num, remote_ip, remote_port, node_num);
    }
    catch (...)
    {
        return;
    }

    // 发送消息
    char send_buf[100] = "zhouhuahui";
    rdma_client->PostRequest((void *)send_buf, strlen(send_buf) + 1);
}