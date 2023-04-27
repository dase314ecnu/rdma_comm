
#include "test/test_worker_threadpool.h"

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
    // 回复，指定响应的长度是20
    char res_buf[100];
    int length = 20;
    char *pointer = res_buf;
    memcpy(pointer, reinterpret_cast<char *>(&length), sizeof(int));
    if (this->simple_server->PostResponse(node_idx, slot_idx, res_buf) != 0) {
        LOG_DEBUG("TestWorkerThreadpool::workerThreadFun(): failed to post send, ret is %d, errno is %d", rc, errno);
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
    int rc = 0;

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
            LOG_DEBUG("TestWorkerThreadpool worker thread, received msg length: %d",
                    length);

            // 回复，指定响应的长度是20
            char res_buf[100];
            length = 20;
            char *pointer = res_buf;
            memcpy(pointer, reinterpret_cast<char *>(&length), sizeof(int));
            if ((rc = this->simple_server->PostResponse(msg.node_idx, msg.slot_idx, res_buf)) != 0) {
                LOG_DEBUG("TestWorkerThreadpool::workerThreadFun(): failed to post send, ret is %d, errno is %d", rc, errno);
                break;
            }
        }
    }
    LOG_DEBUG("TestWorkerThreadpool worker thread will retire, have processed %lu requests", req_cnt);
}

void *TestWorkerThreadpool::workerThreadFunEntry(void *arg) {
    Args *args = (Args *)arg;
    args->test_worker_threadpool->workerThreadFun();
    return args;
}