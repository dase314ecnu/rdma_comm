#include <assert.h>
#include <sys/wait.h>
#include <random>
#include <atomic>
#include <chrono>
#include <ctime>
#include <algorithm>

#include "test/test_shared_client_metrics.h"
#include "test/test_worker_threadpool.h"
#include "pgrac_rdma_communication.h"
#include "pgrac_inner_scope.h"
#include "pgrac_configuration.h"

void TestSharedClientMetricsClass::runClient() {
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

    const int min_send_size = 5;
    const int max_send_size = 800;
    const int max_latencies_size = 100000;
    
    size_t shm_size = SharedRdmaClient::GetSharedObjSize(_slot_size, 
            _slot_num, _node_num) + sizeof(int) * 2 * _node_num + 100 * CACHE_LINE_SIZE;
    shm_size += sizeof(std::atomic<int>) +           // query_num
                sizeof(std::atomic<int>) +          // latencies_size
                sizeof(float) * max_latencies_size;              // latencies
    myshm = new MySharedMemory(shm_size);
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

    std::atomic<int> *query_num = new (_shared_memory)std::atomic<int>(0);
    _shared_memory += sizeof(std::atomic<int>);
    std::atomic<int> *latencies_size = new (_shared_memory)std::atomic<int>(0);
    _shared_memory += sizeof(std::atomic<int>);
    float *latencies = (float *)_shared_memory;
    _shared_memory += sizeof(float) * max_latencies_size;

    // 将_shared_memory对齐到CACHE_LINE_SIZE字节处
    uintptr_t scratch = reinterpret_cast<uintptr_t>(_shared_memory);
    uintptr_t scratch_new = MemoryAllocator::add_size(scratch, 0, CACHE_LINE_SIZE);
    _shared_memory += (scratch_new - scratch);

    auto func = [&] (uint32_t test_process_idx) {
        char content[1000000];
        int *length = (int *)content;
        std::uniform_int_distribution<int> uniform(min_send_size, max_send_size);
        std::default_random_engine rand_eng; 
        void *response = nullptr;
        
        char *buf = content + sizeof(int);
        for (int i = 0; i < max_send_size; ++i) {
            buf[i] = 'a' + i % 20;
        }

        auto before_time = std::chrono::steady_clock::now();
        auto after_time = std::chrono::steady_clock::now();
        double latency = 0.0;
        for (int j = 0; j < this->_reqs_per_test_thread; ++j) {
            // 产生随机消息
            *length = uniform(rand_eng) + sizeof(int);
            rdma_client->PostRequest((void *)content, *length, &response); 
            // free(response);
            (*query_num).fetch_add(1);
            if ((j + 1) % 10 == 0) {
                after_time = std::chrono::steady_clock::now();
                latency = std::chrono::duration<double, std::micro>(after_time - before_time).count();
                latency /= 10;
                if ((*latencies_size).load() < max_latencies_size) {
                    int idx = (*latencies_size).fetch_add(1);
                    if (idx < max_latencies_size) {
                        latencies[idx] = (float)latency;
                    }
                }
                before_time = after_time;
            }
            usleep(100);
        }
    };
    
    for (uint32_t i = 0; i < _num_test_thread + 1; ++i) {
        int ret = fork();
        assert(ret >= 0);
        if (ret == 0) {
            is_father = false;
            sleep(6);  // 等待SharedRdmaClient在_shared_memory上初始化好
            rdma_client = (SharedRdmaClient *)_shared_memory;
            if (i == _num_test_thread) {
                while (true) {
                    sleep(1);
                    int qps = (*query_num).load();
                    int latsize = (*latencies_size).load();
                    std::sort(latencies, latencies + latsize);
                    float min_lat = 0.0, avg_lat = 0.0, max_lat = 0.0, p99_lat = 0.0;
                    if (latsize > 0) {
                        min_lat = latencies[0];
                        max_lat = latencies[latsize - 1];
                        avg_lat = [&] () -> float {
                            float total = 0.0;
                            for (int i = 0; i < latsize; ++i) {
                                total += latencies[i];
                            }
                            return total / latsize;
                        }();
                        p99_lat = latencies[(int)(latsize * 0.99)];
                    }
                    (*query_num).store(0);
                    (*latencies_size).store(0);
                    printf("qps: %d, min_lat: %.2f, avg_lat: %.2f, p99_lat: %.2f, max_lat: %.2f\n",
                            qps, min_lat, avg_lat, p99_lat, max_lat);
                }
            } else {
                func(i);
            }
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

#ifdef TEST_SHARED_CLIENT_METRICS
int main() {
    TestSharedClientMetricsClass test;
    if (IS_SERVER) {
        // 由于是测试，所以我写的工作线程很简单，所有工作线程都从一个队列中取数据
        // 因此如果将工作线程的数量设置的太大，则会大大增加竞争，性能非常差
        test.TestSimpleServer2(IsServer{}, 3, 50, 6, 1024, 1000); 
    } else {
        // node_num: 6
        // slot_size: 1024
        // slot_num: 500
        // num_test_thread: 80  有num_test_thread个线程同时来发送请求
        // reqs_per_test_thread: 100000 每个线程发送reqs_per_test_thread个请求
        test.TestSimpleServer2(IsClient{}, 6, 1024, 1000, 80, 10000000);
    }
}

#endif