#include <assert.h>
#include <sys/wait.h>
#include <random>

#include "test/test_shared_client.h"
#include "test/test_worker_threadpool.h"
#include "pgrac_rdma_communication.h"
#include "pgrac_inner_scope.h"
#include "pgrac_configuration.h"

void TestSharedClientClass::runClient() {
    std::string remote_ip = SERVER_IP; 
    int remote_port = 21001;
    SharedRdmaClient *rdma_client = nullptr;
    MySharedMemory *myshm = nullptr;
    bool is_father = true;
    
    SCOPEEXIT([&]() {
        if (is_father) {
            if (rdma_client != nullptr) {
                rdma_client->Destroy();
            }
            if (myshm != nullptr) {
                delete myshm;
                myshm = nullptr;
            }

            printf("TestSharedClient pass\n");
        }
    });
    
    myshm = new MySharedMemory(SharedRdmaClient::GetSharedObjSize(_slot_size, 
            _slot_num, _node_num) + sizeof(int) * 2 * _node_num);
    _shared_memory = myshm->GetSharedMemory();
    if (_shared_memory == nullptr) {
        LOG_DEBUG("TestSharedClient failed to create shared memory");
        return;
    }
    // 初始化socketpair
    int *listen_fd = (int *)_shared_memory;
    for (int i = 0; i < _node_num; ++i) {
      int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, &(listen_fd[i * 2]));
      if (ret != 0) {
        LOG_DEBUG("TestSharedClient failed to make socketpair");
        return;
      }
    }

    _shared_memory = _shared_memory + sizeof(int) * 2 * _node_num;
    
    // 子进程向对应的node中写入消息，并通过client.listend_fd通知对应的线程
    // auto func = [&] (uint32_t test_process_idx) {
    //     char content[20] = "zhouhuahui";
    //     int length = sizeof(int) + strlen(content) + 1;
    //     char send_buf[1000];
    //     char *pointer = send_buf;
    //     memcpy(pointer, reinterpret_cast<char *>(&length), sizeof(int));
    //     pointer += sizeof(int);
    //     memcpy(pointer, content, strlen(content) + 1);
        
    //     for (int j = 0; j < this->_reqs_per_test_thread; ++j) {
    //         // LOG_DEBUG("test_process of %u will send %dth(from 0) msg", test_process_idx, j);
    //         // void *response = nullptr;
    //         // // rdma_client->PostRequest((void *)send_buf, length, &response);
    //         // int rc = 0;
    //         // auto wait = rdma_client->AsyncPostRequest((void *)send_buf, length, &rc); 
    //         // wait(&response);
    //         // LOG_DEBUG("test_process of %u has sent %dth(from 0) msg, get response length: %d", 
    //         //         test_process_idx, j, MessageUtil::parseLength(response));
    //         // free(response);

    //         if (j % 10 == 9) {
    //             int ret;
    //             rdma_client->AsyncPostRequestNowait((void *)send_buf, length, &ret);
    //         } else {
    //             auto callback = [&](void *response) {
    //                 LOG_DEBUG("test_process of %u has sent %dth(from 0) msg, get response length: %d", 
    //                     test_process_idx, j, MessageUtil::parseLength2(response));
    //             };
    //             rdma_client->PostRequest((void *)send_buf, length, callback); 
    //         }
    //     }
    // };

    auto func = [&] (uint32_t test_process_idx) {
        char content[1000000];
        int *length = (int *)content;
        // std::uniform_int_distribution<int> uniform(5, 50);
        std::uniform_int_distribution<int> uniform(100 *this-> _slot_size, 300 *this-> _slot_size);
        std::default_random_engine rand_eng; 
        
        for (int j = 0; j < this->_reqs_per_test_thread; ++j) {
            LOG_DEBUG("test_process of %u will send %dth(from 0) msg", test_process_idx, j);
            
            // 产生随机消息
            *length = uniform(rand_eng);
            char *buf = content + sizeof(int);
            for (int i = 0; i < *length - 1; ++i) {
                buf[i] = 'a' + i % 20;
            }
            buf[*length - 1] = '\0';
            *length += sizeof(int);

            if (j % 10 == 9) {
                int ret;
                rdma_client->AsyncPostRequestNowait((void *)content, *length, &ret);
            } else if (j % 10 == 8) {
                void *response = nullptr;
                int rc = 0;
                auto wait = rdma_client->AsyncPostRequest((void *)content, *length, &rc);
                wait(&response);
                LOG_DEBUG("test_process of %u has sent %dth(from 0) msg, get response length: %d", 
                        test_process_idx, j, MessageUtil::parseLength2(response));
                free(response);
            } else {
                auto callback = [&](void *response) {
                    LOG_DEBUG("test_process of %u has sent %dth(from 0) msg, get response length: %d", 
                        test_process_idx, j, MessageUtil::parseLength2(response));
                };
                rdma_client->PostRequest((void *)content, *length, callback); 
            }
        }
    };
    
    for (uint32_t i = 0; i < _num_test_thread; ++i) {
        int ret = fork();
        assert(ret >= 0);
        if (ret == 0) {
            is_father = false;
            sleep(25);  // 等待SharedRdmaClient在_shared_memory上初始化好
            rdma_client = (SharedRdmaClient *)_shared_memory;
            func(i);
            return;
        }
    }

    try {
        rdma_client = new (_shared_memory)SharedRdmaClient(_slot_size, _slot_num, remote_ip, remote_port, 
                _node_num, _shared_memory + sizeof(SharedRdmaClient), listen_fd);
    } catch (...) {
        LOG_DEBUG("TestSharedClient failed to new SharedRdmaClient");
        return;
    }
    if (rdma_client->Run() != 0) {
        LOG_DEBUG("TestSharedClient failed, failed to run SharedRdmaClient");
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
        test.TestSimpleServer2(IsServer{}, 3, 50, 10, 1024, 500); 
    } else {
        // node_num: 5
        // slot_size: 64
        // slot_num: 500
        // num_test_thread: 100  有num_test_thread个线程同时来发送请求
        // reqs_per_test_thread: 100 每个线程发送reqs_per_test_thread个请求
        test.TestSimpleServer2(IsClient{}, 10, 1024, 500, 30, 1000);
    }
}

#endif