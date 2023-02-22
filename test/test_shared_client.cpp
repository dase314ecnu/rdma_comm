#include <assert.h>
#include <sys/wait.h>

#include "test/test_shared_client.h"
#include "test/test_worker_threadpool.h"
#include "rdma_communication.h"
#include "inner_scope.h"
#include "configuration.h"

void TestSharedClientClass::runClient() {
    std::string remote_ip = SERVER_IP; 
    int remote_port = 21001;
    SharedRdmaClient *rdma_client = nullptr;
    bool is_father = true;
    
    SCOPEEXIT([&]() {
        if (is_father) {
            if (rdma_client != nullptr) {
                rdma_client->Destroy();
            }

            LOG_DEBUG("TestSharedClient pass");
        }
    });
    
    MySharedMemory myshm(SharedRdmaClient::GetSharedObjSize(_slot_size, 
            _slot_num, _node_num));
    _shared_memory = myshm.GetSharedMemory();
    if (_shared_memory == nullptr) {
        LOG_DEBUG("TestSharedClient failed to create shared memory");
        return;
    }
    try
    {
        rdma_client = new (_shared_memory)SharedRdmaClient(_slot_size, _slot_num, remote_ip, remote_port, 
                _node_num, _shared_memory + sizeof(SharedRdmaClient));
    }
    catch (...)
    {
        LOG_DEBUG("TestSharedClient failed to new SharedRdmaClient");
        return;
    }
    
    // 子进程向对应的node中写入消息，并通过client.listend_fd通知对应的线程
    auto func = [&] (uint32_t test_process_idx) {
        char content[20] = "zhouhuahui";
        int length = sizeof(int) + strlen(content) + 1;
        char send_buf[1000];
        char *pointer = send_buf;
        memcpy(pointer, reinterpret_cast<char *>(&length), sizeof(int));
        pointer += sizeof(int);
        memcpy(pointer, content, strlen(content) + 1);
        
        for (int j = 0; j < this->_reqs_per_test_thread; ++j) {
            // zhouhuahui test
            LOG_DEBUG("test_process of %u will send %dth(from 0) msg", test_process_idx, j);
            rdma_client->PostRequest((void *)send_buf, length);
            LOG_DEBUG("test_process of %u has sent %dth(from 0) msg", test_process_idx, j);
        }
    };

    if (rdma_client->Run() != 0) {
        LOG_DEBUG("TestSharedClient failed, failed to run SharedRdmaClient");
    }
    
    for (uint32_t i = 0; i < _num_test_thread; ++i) {
        int ret = fork();
        assert(ret >= 0);
        if (ret == 0) {
            is_father = false;
            func(i);
            return;
        }
    }

    for (uint32_t i = 0; i < _num_test_thread; ++i) {
        int status;
        wait(&status);
    }
}

#ifdef TEST_SHARED_CLIENT
int main() {
    TestSharedClientClass test;
    if (IS_SERVER) {
        // 由于是测试，所以我写的工作线程很简单，所有工作线程都从一个队列中取数据
        // 因此如果将工作线程的数量设置的太大，则会大大增加竞争，性能非常差
        test.TestSimpleServer2(IsServer{}, 3, 10, 5, 64, 20);
    } else {
        // node_num: 5
        // slot_size: 64
        // slot_num: 50
        // num_test_thread: 1000  有num_test_thread个线程同时来发送请求
        // reqs_per_test_thread: 1000 每个线程发送reqs_per_test_thread个请求
        test.TestSimpleServer2(IsClient{}, 5, 64, 20, 10, 10);
    }
}

#endif