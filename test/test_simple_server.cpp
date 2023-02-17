#include <unistd.h>
#include <string>
#include <cstring>

#include "test/test_simple_server.h"
#include "rdma_communication.h"
#include "configuration.h"
#include "log.h"
#include "inner_scope.h"

/**
 * ------------------------------------------------------------------------------------------
 * TestWorkerThreadpool
 * ------------------------------------------------------------------------------------------
 */
TestWorkerThreadpool::TestWorkerThreadpool(uint32_t _worker_num, uint32_t _max_msg_num)
    : worker_num(_worker_num)
{
    LOG_DEBUG("TestWorkerThreadpool start to construct TestWorkerThreadpool");
    this->msg_queue = new MsgQueue(_max_msg_num);
    this->worker_threads = new pthread_t *[this->worker_num];
    for (int i = 0; i < this->worker_num; ++i)
    {
        this->worker_threads[i] = nullptr;
    }
    LOG_DEBUG("TestWorkerThreadpool success to construct TestWorkerThreadpool");
}

TestWorkerThreadpool::~TestWorkerThreadpool()
{
    LOG_DEBUG("TestWorkerThreadpool start to deconstruct TestWorkerThreadpool");
    this->Stop();
    delete this->msg_queue;
    LOG_DEBUG("TestWorkerThreadpool success to deconstruct TestWorkerThreadpool");
}

void TestWorkerThreadpool::Start(void *request, uint32_t node_idx, uint32_t slot_idx)
{
    while (!this->stop)
    {
        (void) pthread_spin_lock(&(this->msg_queue->lock));
        if (this->msg_queue->queue.size() < this->msg_queue->max_msg_num)
        {
            Msg msg;
            msg.request = request;
            msg.node_idx = node_idx;
            msg.slot_idx = slot_idx;
            this->msg_queue->queue.push_back(std::move(msg));
            (void) pthread_spin_unlock(&(this->msg_queue->lock));
            break;
        }
        else
        {
            (void) pthread_spin_unlock(&(this->msg_queue->lock));
            usleep(100);
            continue;
        }
    }
}

void TestWorkerThreadpool::SetSimpleServer(RdmaServer<TestWorkerThreadpool> *server) {
    this->simple_server = server;
}

int TestWorkerThreadpool::Run() {
    uint32_t i = 0;
    SCOPEEXIT([&]() {
        if (i < this->worker_num) {
            LOG_DEBUG("TestWorkerThreadpool failed to run server, start to release resources");
            this->stop = true;
            for (int j = 0; j < i; ++j) {
                (void) pthread_join(*this->worker_threads[j], nullptr);
                delete this->worker_threads[j];
                this->worker_threads[j] = nullptr;
            }
            delete[] this->worker_threads;
            this->worker_threads = nullptr;
        }
    });

    LOG_DEBUG("TestWorkerThreadpool start to run server");

    for (i = 0; i < this->worker_num; ++i) {
        Args *args = new Args();
        args->test_worker_threadpool = this;
        this->worker_threads[i] = new pthread_t();
        if (pthread_create(this->worker_threads[i], nullptr, this->workerThreadFunEntry, args)
                != 0)
        {
            return -1;
        }
    }

    LOG_DEBUG("TestWorkerThreadpool success to run server, launch %d worker threads", this->worker_num);
    return 0;
}

void TestWorkerThreadpool::Stop()
{
    LOG_DEBUG("TestWorkerThreadpool start to stop server");
    this->stop = true;
    if (this->worker_threads == nullptr)
    {
        return;
    }
    for (int i = 0; i < this->worker_num; ++i)
    {
        (void) pthread_join(*this->worker_threads[i], nullptr);
        delete this->worker_threads[i];
        this->worker_threads[i] = nullptr;
    }
    delete[] this->worker_threads;
    this->worker_threads = nullptr;
    LOG_DEBUG("TestWorkerThreadpool success to stop server");
}

void TestWorkerThreadpool::workerThreadFun() {
    uint64_t req_cnt = 0;   // 接收的总请求的个数。
    while (!this->stop)
    {
        pthread_spin_lock(&(this->msg_queue->lock));
        Msg msg;
        bool find = false;
        if (this->msg_queue->queue.size() > 0)
        {
            msg = std::move(this->msg_queue->queue.front());
            this->msg_queue->queue.erase(msg_queue->queue.begin());
            find = true;
        }
        pthread_spin_unlock(&(this->msg_queue->lock));
        
        if (find) {
            req_cnt++;
            char *buf = (char *)msg.request;
            int length = msg.parseLength(buf);
            buf += sizeof(int);
            std::string content = msg.parseContent(buf);
            LOG_DEBUG("TestWorkerThreadpool worker thread, received msg length: %d, content: %s",
                    length, content.c_str());
            // 回复
            if (this->simple_server->PostResponse(msg.node_idx, msg.slot_idx) != 0) {
                break;
            }
        }
    }
    LOG_DEBUG("TestWorkerThreadpool worker thread will retire, have processed %lld requests", req_cnt);
}

void *TestWorkerThreadpool::workerThreadFunEntry(void *arg) {
    Args *args = (Args *)arg;
    args->test_worker_threadpool->workerThreadFun();
    return args;
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
    } 
    if (worker_threadpool != nullptr) {
        delete worker_threadpool;
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

    sleep(1000);

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

int main() {
    TestSimpleServerClass test;
    test.TestSimpleServer(IS_SERVER);
}