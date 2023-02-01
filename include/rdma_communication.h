#ifndef RDMA_COMMUNICATION_H
#define RDMA_COMMUNICATION_H

#include <infiniband/verbs.h>
#include <string>
#include <pthread.h>
#include <semaphore.h>
#include <thread>

#include "configuration.h"
#include "test/test_shared_memory.h"

typedef struct QueuePairMeta
{
    uint64_t registered_memory;
    uint32_t registered_key;
    uint32_t qp_num;
    uint16_t lid;
    uint8_t  gid[16];
}QueuePairMetaData;

enum SlotState {
    SLOT_IDLE,         /** 当前slot可以用于发送消息 */
    SLOT_INPROGRESS,   /** 当前slot正在发送消息和等待响应 */
    SLOT_OTHER
};

/** 
 * 当没有slot可以用时，空转轮询，而不是用条件变量等待。
 * 因为我们有个假设：slot都不可用的概率很小。如果使用条件变量，则我们
 * 必须使用pthread_mutext_t来保护ZSend，很显然，效率不好。
 */
typedef struct ZSend {
    SlotState *states;  /** 一个QP的所有slot的状态 */
    uint32_t   front;  /** 最旧的还处于SLOT_INPROGRESS的slot */
    uint32_t   rear;   /** 最新的还处于SLOT_INPROGRESS的slot的后面 */
    uint32_t   notsent_front;   /** front到rear中还未发送的slot的最旧的那个 */
    uint32_t   notsent_rear;    /** front到rear中还未发送的slot的最新的那个的后面 */
    pthread_spinlock_t spinlock;   /** 使用自旋锁保护这个ZSend */
} ZSend;

typedef struct ZAwake {
    /** 每个slot都对应一个sem_t，每当进程/线程要等待响应时，就使用这里的sems */
    sem_t          *sems;  
} ZAwake;


/**
 * 负责建立QP连接，在QP上发布发送和接收任务。
 * 线程不安全。
 */
class RdmaQueuePair{
private:
    /* local rdma queue pair resources */
    uint32_t                 rdma_port = 0;    //用于rdma查询的端口号: ibv_query_port。一般是1
    const char              *device_name = nullptr;  //设备名称。一般是"mlx5_0"
    struct ibv_context	    *ctx = nullptr; /* device handle */
    struct ibv_pd           *pd = nullptr;  /* Protection Domain handler */
    struct ibv_port_attr     port_attribute;
    struct ibv_comp_channel *channel = nullptr;
    struct ibv_cq           *cq = nullptr;             /* Completion Queue */
    struct ibv_qp           *qp = nullptr;             /* queue pair with remote qp */
    union  ibv_gid           local_gid;
    
    uint64_t          local_memory_size = 0;   /* QP中的mr的大小 */
    void             *local_memory = 0;   /* Memory begin address */
    struct ibv_mr    *local_mr = nullptr;       /* Memory registration handler */
    uint64_t          local_slot_size = 0;   /* local_memory包含多个slot，每个slot可以容纳一个消息大小 */
    uint64_t          local_slot_num = 0;    /* local_memory包含local_slot_num个slot，
                                            因此：local_memory_size == local_slot_size * local_slot_num 
                                            同时，qp也要能够发布最多local_slot_num个发送任务*/

    /* remote rdma queue pair resources */ 
    uintptr_t  remote_memory = 0;
    uint32_t   remote_mr_key = 0;
    uint32_t   remote_qp_num = 0;
    uint16_t   remote_lid = 0;
    uint8_t    remote_gid[16];

private:
    void initializeLocalRdmaResource();
    void createQueuePair();
    bool modifyQPtoInit();
    bool modifyQPtoRTR();
    bool modifyQPtoRTS();

public:
    RdmaQueuePair(uint64_t _local_slot_num, uint64_t _local_slot_size, 
            const char *_device_name = "mlx5_0", uint32_t _rdma_port = 1);
    // 注册内存需要在共享内存上
    RdmaQueuePair(uint64_t _local_slot_num, uint64_t _local_slot_size, 
            void *_shared_memory, const char *_device_name = "mlx5_0", uint32_t _rdma_port = 1);
    ~RdmaQueuePair();
    void*    GetLocalMemory();
    uint64_t GetLocalMemorySize();
    /** 在slot_idx的slot处赋值长度为size的send_content */
    void SetSendContent(void *send_content, uint64_t size, uint64_t slot_idx) {}
    void PostSend(uint32_t imm_data, uint64_t slot_idx) {}
    void PostReceive(uint64_t slot_idx) {}
    int  PullCompletionFromCQ(struct ibv_wc *wc) { return 0; }   
    void ReceiveRQCompletion() {}  
    void ReadyToUseQP() {}
    void GetLocalQPMetaData(QueuePairMetaData &local_data) {}
    void SetRemoteQPMetaData(QueuePairMetaData &remote_data) {}
};

/** 
 * 负责对同一个计算节点的多个QP的RdmaQueuePair进行维护；对同步信息，比如ZSend和ZAwake进行维护
 */
class RdmaClient {
protected:
    uint32_t           node_num = 0;   /** 可以通过多少个node（发送器）向一个计算节点发送消息 */
    //用于发送任务请求
    RdmaQueuePair     **rdma_queue_pairs = nullptr;   /** node_num长度 */
    std::string remote_ip;    //连接远程服务器的ip
    uint32_t    remote_port = 0;  //连接远程服务器的端口
    ZSend      *sends = nullptr;        /** node_num长度 */
    ZAwake     *awakes = nullptr;       /** node_num长度 */

    std::thread *send_threads = nullptr; /** node_num长度 */

protected:
    void createRdmaQueuePairs() {}
    int  connectSocket() { return 0; }
    int  dataSyncwithSocket(int sock, int size, char *local_data, char *remote_data) {}
    // /** 发送线程执行的代码 */
    // void sendThreadFun();
 
public:
    /** 仅创建相关资源，不启动发送线程 */
    RdmaClient(uint64_t _slot_size, uint64_t _slot_num, std::string _remote_ip, uint32_t _remote_port, 
            uint32_t _node_num);
    /** 仅创建相关资源，不启动发送线程 
     * 注册内存需要在共享内存上
     */
    RdmaClient(uint64_t _slot_size, uint64_t _slot_num, std::string _remote_ip, uint32_t _remote_port, 
            uint32_t _node_num, void *_shared_memory);
    ~RdmaClient() {}
    // /** 启动发送线程 */
    // void Run(); 
    // /** 发送消息，并等待响应
    //  *    1. 选择一个slot来发送数据或者等待空闲的slot，将这个slot设置为SLOT_INPROGRESS
    //  *    2. 在ZAwake上等待响应
    //  *    3. RdmaClient的发送线程检测到响应后，通过ZAwake通知
    //  *    4. 将这个slot设置为SLOT_IDLE
    //  */
    // void PostRequest(void *send_content, uint64_t size);
};



/** 
 * 在计算节点中，每个backend进程和SharedRdmaClient交互。
 * 
 * 由于PG是多进程架构，如果PG的backend想要访问共享缓存服务，必须通过进程间通信机制，
 * 因此就有了RdmaClient的子类：SharedRdmaClient。
 * 如果一个backend想要访问SharedRdmaClient，大致流程如下：
 *   1. backend在共享内存中查看对应的发送器是否有空闲的slot，并在这个slot中填充要发送的数据，
 *      将它设置为SLOT_INPROGRESS
 *   2. backend向统一处理信号的线程发送信号，并在对应slot的信号量上等待
 *   3. 信号处理线程遍历所有的sends来查看哪些发送线程需要被通知去发送，然后向对应的线程的listen_fd写一个字节
 *   4. SharedRdmaClient的发送线程从epoll返回，将这个slot发送出去
 *   5. 发送线程收到响应后，将对应的进程/线程唤醒。
 *   6. backend将这个slot设置为SLOT_IDLE
 * 以上逻辑在PostRequest()中实现
 * 
 * 由于这个类要创建在共享内存中，所以父类不应该有虚函数。
 */
class SharedRdmaClient : public RdmaClient{
    friend class TestSharedMemoryClass;
private:
    /** 每个node（发送线程）监听一个listen_fd[i][1]，如果有消息来，说明某个slot中含有要发送的数据。*/
    int       **listen_fd = nullptr;  /** node_num长度 */
    
    // /** 默认的slot_size, slot_num, node_num */
    // static const int kSlotSize = 64;
    // static const int kSlotNum = 10;
    // static const int kNodeNum = 5;

private:
    /** 发送线程执行的代码 */
    void sendThreadFun();

public:
    /** 将RdmaQueuePair.local_memory、RdmaClient.sends、RdmaClient.awakes,  
     * RdmaClient.send_threads等拷贝到shared_memory共享内存中 */
    SharedRdmaClient(uint64_t _slot_size, uint64_t _slot_num, 
            std::string _remote_ip, uint32_t _remote_port, 
            uint32_t _node_num, void* _shared_memory);
    /** 启动发送线程 */
    void Run() {}
    /** 调用SharedRdmaClient的发送消息的接口 */
    void PostRequest(void *send_content, uint64_t size) {}

    /** 计算SharedRdmaClient需要多少字节的共享内存空间来创建对象，包括SharedRdmaClient本身
     * 以及需要共享的数据的总大小。
     */
    static uint64_t GetSharedObjSize(uint64_t _slot_size, uint64_t _slot_num, uint32_t _node_num) {
        return 0;
    }
};


/** 
 * 负责接收来自己多个计算节点的消息，并分配工作线程来处理。
 */
class RdmaServer {
protected:
    uint32_t           node_num;   /** 对应每个来连接的计算节点，分配node_num个接收器 */
    //用于发送任务请求
    RdmaQueuePair     *rdma_queue_pairs;   /** node_num长度 */
    uint32_t    local_port;   /** 监听线程的监听端口 */

    std::thread *receive_threads; /** node_num长度 */
    std::thread  listen_thread;   /** 监听线程，用于处理来自其他计算节点的连接请求 */

protected:

public:
    
};


#endif