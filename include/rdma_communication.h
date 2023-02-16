#ifndef RDMA_COMMUNICATION_H
#define RDMA_COMMUNICATION_H

#include <infiniband/verbs.h>
#include <string>
#include <pthread.h>
#include <semaphore.h>
#include <thread>
#include <exception>
#include <vector>
#include <stdint.h>
#include <sys/socket.h>
#include <assert.h>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <errno.h>
#include <map>
#include <fcntl.h>

#include "configuration.h"
#include "test/test_shared_memory.h"
#include "rdma_communication.h"
#include "inner_scope.h"
#include "waitset.h"
#include "log.h"

typedef struct QueuePairMeta
{
    uintptr_t registered_memory;
    uint32_t registered_key;
    uint32_t qp_num;
    uint32_t qp_psn;
    uint16_t lid;
    union ibv_gid gid;
} QueuePairMetaData;

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
    SlotState *states = nullptr;  /** 一个QP的所有slot的状态 */
    uint64_t   front = 0;  /** 最旧的还处于SLOT_INPROGRESS的slot */
    uint64_t   rear = 0;       /** 最新的还处于SLOT_INPROGRESS的slot的后面 */
    uint64_t   notsent_front = 0;    /** front到rear中还未发送的slot的最旧的那个 */
    uint64_t   notsent_rear  = 0;    /** front到rear中还未发送的slot的最新的那个的后面 */
    pthread_spinlock_t spinlock;     /** 使用自旋锁保护这个ZSend */
    
    ZSend() {}
    /** 
     * @param _shared_memroy: ZSend是否是进程间共享的，如果是的话，
     * 则_shared_memory不为nullptr, 并且ZSend和status要床在_shared_memroy中
     */
    ZSend(void *_shared_memory, uint64_t _slot_num) {
        int shared = (_shared_memory == nullptr ? 0 : 1);
        // 初始化spinlock
        if (pthread_spin_init(&this->spinlock, shared) != 0) {
            throw std::bad_exception();
        }
        // 给this->states分配空间并初始化
        if (shared != 0) {
            char *scratch = (char *)_shared_memory;
            this->states  = (SlotState *)scratch;
        } else {
            this->states = new SlotState[_slot_num + 1];
            if (this->states == nullptr) {
                throw std::bad_exception();
            }
        }
        for (int i = 0; i < _slot_num + 1; ++i) {
            this->states[i] = SlotState::SLOT_IDLE;
        }
    }
} ZSend;

typedef struct ZAwake {
    /** 每个slot都对应一个sem_t，每当进程/线程要等待响应时，就使用这里的sems */
    sem_t          *sems = nullptr;  
    ZAwake() {}
    ZAwake(void *_shared_memory, uint64_t _slot_num) {
        int shared = (_shared_memory == nullptr ? 0 : 1);
        int i = 0;
        SCOPEEXIT([&]() {
            if (i < _slot_num + 1) {
                // sems并没有完全初始化完成
                if (this->sems != nullptr) {
                    for (int j = 0; j < i; ++j) {
                        (void) sem_destroy(&(this->sems[j]));
                    }
                    if (shared == 0) {
                        delete[] this->sems;
                    }
                    this->sems = nullptr;
                }
            }
        });

        if (shared != 0) {
            char *scratch = (char *)_shared_memory;
            this->sems = (sem_t *)scratch;
        } else {
            this->sems = new sem_t[_slot_num + 1];
            if (this->sems == nullptr) {
                throw std::bad_exception();
            }
        }
        for (i = 0; i < _slot_num + 1; ++i) {
            if (sem_init(&(this->sems[i]), shared, 0) != 0) {
                throw std::bad_exception();
            }
        }
    }
} ZAwake;

/**
 * 负责建立QP连接，在QP上发布发送和接收任务。
 * 线程不安全。
 */
class RdmaQueuePair{
private:
    bool                     use_shared_memory = false;  // use_shared_memory为true时，一些变量是在
                                                         // 共享内存上进行分配的: local_memory
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
    uint32_t                 qp_psn;
    
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
    uint32_t   remote_qp_psn = 0;
    uint16_t   remote_lid = 0;

private:
    /**
     * 使用ibverbs基础库，来初始化本机的RDMA资源：
     * device_name, ibv_context, ibv_port_attr, ibv_comp_channel, ibv_cq
     * ibv_qp, ibv_gid。 
     * 其中，ibv_qp只是初始化，并未到达RTR, RTS状态（QP的状态）
     * 
     * @return 0表示正常执行，-1表示出现异常
     */
    int initializeLocalRdmaResource();
    /**
     * 创建一个QP, type是IBV_QPT_RC
     * @return 0表示正常执行，-1表示出现异常
     */
    int createQueuePair();
    int modifyQPtoInit();
    int modifyQPtoRTR();
    int modifyQPtoRTS();

public:
    RdmaQueuePair(uint64_t _local_slot_num, uint64_t _local_slot_size, 
            const char *_device_name = "mlx5_0", uint32_t _rdma_port = 1);
    // 注册内存需要在共享内存上
    RdmaQueuePair(uint64_t _local_slot_num, uint64_t _local_slot_size, 
            void *_shared_memory, const char *_device_name = "mlx5_0", uint32_t _rdma_port = 1);
    ~RdmaQueuePair();
    /* 销毁RdmdaQueuePair中创建所有的资源 */
    void Destroy();
    void*    GetLocalMemory();
    uint64_t GetLocalMemorySize();
    /* 得到ibv_comp_channel */
    ibv_comp_channel  *GetChannel();
    /* 在slot_idx的slot处赋值长度为size的send_content */
    void SetSendContent(void *send_content, uint64_t size, uint64_t slot_idx);
    /* 将slot_idx处的slot发送给对端 */
    int PostSend(uint32_t imm_data, uint64_t slot_idx);
    /* 向RNIC发送一个recv wr */
    int PostReceive();
    /** 
     * 从CQ中取出多个WC，非阻塞
     * @return 1: 成功取出 0：没有WC -1：出现异常
     */
    int  PollCompletionsFromCQ(std::vector<struct ibv_wc> &wcs); 
    int ReadyToUseQP();
    void GetLocalQPMetaData(QueuePairMetaData &local_data);
    void SetRemoteQPMetaData(QueuePairMetaData &remote_data);
};

/** 
 * 负责对同一个计算节点的多个QP的RdmaQueuePair进行维护；对同步信息，比如ZSend和ZAwake进行维护
 */
class RdmaClient {
protected:
    bool               use_shared_memory = false;
    uint32_t           node_num = 0;   /** 可以通过多少个node（发送器）向一个计算节点发送消息 */
    uint64_t           slot_size = 0;
    uint64_t           slot_num = 0;
    /** 
     * 用于发送任务请求和接收响应的QP。
     * node_num长度
     */
    RdmaQueuePair     **rdma_queue_pairs = nullptr;
    std::string remote_ip;    //连接远程服务器的ip
    uint32_t    remote_port = 0;  //连接远程服务器的端口
    ZSend      *sends = nullptr;        // node_num长度
    ZAwake     *awakes = nullptr;       // node_num长度

    std::thread *send_threads = nullptr; // node_num长度

protected:
    /** 
     * shared_memroy为nullptr，表示QP不创建在共享内存上，否则就创建在共享内存上
     * 不仅要创建QP，还要和对端的QP进行连接。
     * @param shared_memory: 从shared_memory开始创建QPs
     * @param end_memory: 从shared_memory到*end_memory创建好了对象，end_memory是输出参数
     */
    int createRdmaQueuePairs(void *shared_memory, void **end_memory = nullptr);
    /** 
     * 与对端的IP地址建立socket连接
     * @return >0: socket  -1: 出现异常
     */
    int  connectSocket();
    /** 
     * @param compute_id: 本节点的节点号
     * @param meta      : 本节点的QueuePairMeta信息
     * @param remote_compute_id : 对端节点的节点号
     * @param remote_meta       : 对端节点的QueuePairMeta信息
     * @return  0: 成功  -1: 出现异常
     * 交换信息的时候，要考虑到大小端转换的问题，虽然没有这一步也大概率没事。
     */
    int  dataSyncWithSocket(int sock, uint32_t compute_id, const QueuePairMeta& meta,
            uint32_t &remote_compute_id, QueuePairMeta &remote_meta);
 
public:
    /** 仅创建相关资源，不启动发送线程 */
    RdmaClient(uint64_t _slot_size, uint64_t _slot_num, std::string _remote_ip, uint32_t _remote_port, 
            uint32_t _node_num);
    /** 
     * 仅创建相关资源，不启动发送线程 
     * 注册内存需要在共享内存上
     */
    RdmaClient(uint64_t _slot_size, uint64_t _slot_num, std::string _remote_ip, uint32_t _remote_port, 
            uint32_t _node_num, void *_shared_memory);
    ~RdmaClient();
    /** Destroy all the resources of RdmaClient */
    void Destroy();
};

/** 
 * SharedCacheService中其他线程要发送消息，则通过CommonRdmaClient。
 */
class CommonRdmaClient : public RdmaClient {
private:
    struct Args {
        uint32_t node_idx = 0;
        CommonRdmaClient *client = nullptr;
    };
    
    /** 
     * stop为true，表示要终止所有发送线程，发送线程要循环检查这个变量
     */
    volatile bool stop = false;

protected:
    pthread_t *send_threads = nullptr;   // node_num长度
protected:
    /** 
     * 不断监听node_idx号的node，看是否有回复到来，并进行处理
     */
    void sendThreadFun(uint32_t node_idx);
    static void *sendThreadFunEntry(void *arg);

public:
    CommonRdmaClient(uint64_t _slot_size, uint64_t _slot_num, std::string _remote_ip, uint32_t _remote_port, 
            uint32_t _node_num);
    ~CommonRdmaClient();
    /** 
     * 启动node_num个线程，分别监听每个node
     * @return 0: 成功  -1: 出现异常
     */
    int Run();
    /** 
     * 将所有发送线程终止掉
     */
    void Stop();
    /** 发送消息，并等待响应
     *    1. 选择一个slot来发送数据或者等待空闲的slot，将这个slot设置为SLOT_INPROGRESS
     *    2. 在ZAwake上等待响应
     *    3. RdmaClient的发送线程检测到响应后，通过ZAwake通知
     *    4. 将这个slot设置为SLOT_IDLE
     */
    int PostRequest(void *send_content, uint64_t size);
};


/** 
 * 在计算节点中，每个backend进程和SharedRdmaClient交互。
 * 
 * 由于PG是多进程架构，如果PG的backend想要访问共享缓存服务，必须通过进程间通信机制，
 * 因此就有了RdmaClient的子类：SharedRdmaClient。
 * 如果一个backend想要访问SharedRdmaClient，大致流程如下：
 *   1. backend在共享内存中查看对应的发送器是否有空闲的slot，并在这个slot中填充要发送的数据，
 *      将它设置为SLOT_INPROGRESS
 *   2. backend向对应的发送线程的listend_fd发送一个消息，并在对应slot的信号量上等待
 *   3. SharedRdmaClient的发送线程从epoll返回，将这个slot发送出去
 *   4. 发送线程收到响应后，将对应的进程/线程唤醒。
 *   5. backend将这个slot设置为SLOT_IDLE
 * 以上逻辑在PostRequest()中实现
 * 
 * 由于这个类要创建在共享内存中，所以父类不应该有虚函数。
 */
class SharedRdmaClient : public RdmaClient {
    friend class TestSharedMemoryClass;
private:
    struct Args {
        uint32_t node_idx = 0;
        SharedRdmaClient *client = nullptr;
    };
    
    /** 
     * stop为true，表示要终止所有发送线程，发送线程要循环检查这个变量
     */
    volatile bool stop = false;

protected:
    /** 
     * 每个node（发送线程）监听一个listen_fd[i][1]，如果有消息来，
     * 说明某个slot中含有要发送的数据。
     * */
    int       **listen_fd = nullptr;     /* node_num长度 */
    pthread_t  *send_threads = nullptr;  /* node_num长度 */

protected:
    /** 发送线程执行的代码 */
    void sendThreadFun(uint32_t node_idx);
    static void *sendThreadFunEntry(void *arg);

public:
    /** 将RdmaQueuePair.local_memory、RdmaClient.sends、RdmaClient.awakes,  
     * 等拷贝到shared_memory共享内存中 */
    SharedRdmaClient(uint64_t _slot_size, uint64_t _slot_num, 
            std::string _remote_ip, uint32_t _remote_port, 
            uint32_t _node_num, void* _shared_memory);
    ~SharedRdmaClient();
    /** 启动所有发送线程 */
    int Run();
    /** 终止所有发送线程 */
    void Stop();
    /** Destroy all the resources of the SharedRdmaClient */
    void Destroy();
    /** 调用SharedRdmaClient的发送消息的接口 */
    int PostRequest(void *send_content, uint64_t size);

    /** 计算SharedRdmaClient需要多少字节的共享内存空间来创建对象，包括SharedRdmaClient本身
     * 以及需要共享的数据的总大小。
     */
    static uint64_t GetSharedObjSize(uint64_t _slot_size, uint64_t _slot_num, 
            uint32_t _node_num) 
    {
        return 0;
    }
};


/** 
 * 负责接收来自己多个计算节点的消息，并分配工作线程来处理。
 * 远程的RDMA Write的imm_data字段会保存slot的号，因此RdmaServer可以直接知道从哪个slot中读取消息
 * @parma T: 业务处理线程池的实现
 * 
 * @todo: RdmaServer要聚合T, T中要聚合RdmaClient和RdmaServer. T含有Start()接口，用于业务处理.
 */
template<typename T>
class RdmaServer {
    struct Args {
        uint32_t node_idx = 0;  // 小于compute_num * node_num
        RdmaServer<T> *server = nullptr;
    };

protected:
    uint32_t           compute_num = 0;  // 计算机器的个数
    uint32_t           node_num = 0;   // 对应每个来连接的计算节点，分配node_num个接收器
    uint64_t           slot_size = 0;
    uint64_t           slot_num = 0;
    /** 
     * 用于发送任务请求。
     * compute_num * node_num长度。rdma_queue_paris[i]负责和i / compute_num号机器
     * 上的i % compute_num号node进行通信。
     */
    RdmaQueuePair     **rdma_queue_pairs = nullptr; 
    /** 
     * locks[i]保护对rdma_queue_paris[i]的访问。
     * 监听线程要随时初始化rdma_queue_pairs[i]，同时i号工作线程要检查并坚挺rdma_queue_pairs[i]，
     * 因此需要锁的保护。
     * compute_num * node_num长度
     */
    pthread_spinlock_t *locks = nullptr;
    
    uint32_t    local_port = 0;   // 监听线程的监听端口
    std::thread **receive_threads = nullptr; // compute_num * node_num长度
    std::thread  *listen_thread = nullptr;   // 监听线程，用于处理来自其他计算节点的连接请求

    T   *worker_threadpool = nullptr;   // 业务处理线程池
    volatile bool stop = false;         // 线程是否停止接收请求

protected:
    /** 
     * 不断监听local_port端口，看是否有连接请求到来，并进行处理。
     * 第i个机器的第j个请求到来时，这第j个请求的远程node就被标记为j号node，
     * 同时rdma_queue_pairs[i * compute_num + j]进行初始化，以负责和这个远程node通信。
     */
    void listenThreadFun();
    static void *listenThreadFunEntry(void *arg);
    /** 
     * 不断监听rdma_queue_pairs[i]，看是否有请求到来，并进行处理。
     * @param node_idx: 小于compute_num * node_num
     */
    void receiveThreadFun(uint32_t node_idx);
    static void *receiveThreadFunEntry(void *arg);
    /** 
     * @param compute_id: 本节点的节点号
     * @param meta      : 本节点的QueuePairMeta信息
     * @param remote_compute_id : 对端节点的节点号
     * @param remote_meta       : 对端节点的QueuePairMeta信息
     * @return  0: 成功  -1: 出现异常
     * 交换信息的时候，要考虑到大小端转换的问题，虽然没有这一步也大概率没事。
     */
    int  dataSyncWithSocket(int sock, uint32_t compute_id, const QueuePairMeta& meta,
            uint32_t &remote_compute_id, QueuePairMeta &remote_meta);

public:
    /** 
     * 初始化各个类成员属性。
     * 为rdma_queue_pairs和receive_threads分配内存空间。
     * 初始化locks
     */
    RdmaServer(uint32_t _compute_num, uint32_t _node_num, uint64_t _slot_size, uint64_t _slot_num, 
            uint32_t _port, T *_worker_threadpool);
    ~RdmaServer();
    /** 
     * 开启一个监听线程和多个接收线程, 监听线程接收到RdmaClient的连接请求后，
     * 就可以初始化rdma_queue_pairs[i]，然后i号接收线程就可以直接从rdma_queue_pairs[i]
     * 中监听请求了。
     */
    int Run();
    /** 
     * 终止所有线程
     */
    void Stop();
    /** 
     * 向node_idx号node发送响应。
     * PostResponse()不会与receiveThreadFun()冲突，这是因为RdmaClient和RdmaServer之间的协议：
     * 服务器发送响应之后，客户端才可以释放对应node的锁，因此PostResponse()时，必定不会在同一个node
     * 中到来消息，因此不用考虑并发的问题。
     * @param node_idx: 小于compute_num * node_num
     */
    int PostResponse(uint64_t node_idx, uint64_t slot_idx);
};

/** 
 * ---------------------------------------------------------------------------------------------
 * RdmaServer 实现
 * ----------------------------------------------------------------------------------------------
 */

template<typename T>
void RdmaServer<T>::listenThreadFun() {
  std::map<int, int> comp_counter;  // 记录每个机器已经到来的连接数
  for (int i = 0; i <this->compute_num; ++i) {
    comp_counter[i] = 0;
  }
  int rc;
  int sock;
  struct sockaddr_in my_address;
  int on = 1;
  int flags;

  memset(&my_address, 0, sizeof(my_address));
  my_address.sin_family      = AF_INET;
  my_address.sin_addr.s_addr = htonl(INADDR_ANY);
  my_address.sin_port        = htons(this->local_port);

  assert((sock = socket(PF_INET, SOCK_STREAM, IPPROTO_IP)) >= 0);
  assert(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) >= 0);
  flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock , F_SETFL , flags | O_NONBLOCK);
  assert(bind(sock, (struct sockaddr*)&my_address, sizeof(struct sockaddr)) >= 0);
  assert(listen(sock, this->compute_num * this->node_num) >= 0);  // 最多只会有这么多个连接到来

  LOG_DEBUG("RdmaServer listen thread, is listening connection, listen_port=%d", local_port);

  // 开始接收连接
  while (!this->stop) {
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    int connfd = accept(sock, (struct sockaddr*)&client_address, 
                &client_addrlength);
    if (connfd < 0 && errno != EWOULDBLOCK) {
      LOG_DEBUG("RdmaServer listen thread, failed to accept, connect file descriptor is invalid");
      return;
    }
    if (connfd < 0) {
      usleep(1000 * 500);
      continue;
    }

    // 初始化一个QP
    RdmaQueuePair *qp = nullptr;
    try {
      qp = new RdmaQueuePair(this->slot_num, this->slot_size, DEVICE_NAME, RDMA_PORT);
    } catch (...) {
      LOG_DEBUG("RdmaServer listen thread, failed to new RdmaQueuPair(aka. create qp resource)");
      return;
    }

    QueuePairMeta meta;
    QueuePairMeta remote_meta;
    uint32_t      compute_id = MY_COMPUTE_ID;
    uint32_t      remote_compute_id;     
    qp->GetLocalQPMetaData(meta);
    rc = this->dataSyncWithSocket(connfd, compute_id, meta, remote_compute_id, 
            remote_meta);
    // 交换信息后，关闭连接
    close(connfd);
    connfd = 0;
    if (rc != 0) {
      LOG_DEBUG("RdmaServer listen thread, failed to dataSyncWith Socket");
      return;
    }

    qp->SetRemoteQPMetaData(remote_meta);
    // 准备进行Send/Receive操作
    rc = qp->ReadyToUseQP();
    if (rc != 0) {
      LOG_DEBUG("RdmaServer listen thread, failed to make qp ready to send");
      return;
    }

    // 赋值到this->rdma_queue_pairs[]
    int idx = this->compute_num * remote_compute_id + comp_counter[remote_compute_id];
    comp_counter[remote_compute_id]++;
    pthread_spin_lock(&(this->locks[idx]));
    this->rdma_queue_pairs[idx] = qp;
    pthread_spin_unlock(&(this->locks[idx]));

    LOG_DEBUG("RdmaServer listen thread, success to make qp ready to send, receive thread of %d"
            "will handle the qp connection", idx);
  }
  
  close(sock);
}

template<typename T>
void* RdmaServer<T>::listenThreadFunEntry(void *arg) {
  Args *args = (Args *)arg;
  args->server->listenThreadFun();
  return args;
}

// 这里node_idx
template<typename T>
void RdmaServer<T>::receiveThreadFun(uint32_t node_idx) {
  LOG_DEBUG("RdmaServer receive thread of %lld, start to wait rdma connection to be set", node_idx);

  // 等待qp建立连接
  while (!this->stop) {
    pthread_spin_lock(&(this->locks[node_idx]));
    if (this->rdma_queue_pairs[node_idx] == nullptr) {
      pthread_spin_unlock(&(this->locks[node_idx]));
      sleep(1);
      continue;
    }
    pthread_spin_unlock(&(this->locks[node_idx]));
    break;
  }

  if (this->stop) {
    return;
  }
  
  WaitSet *waitset = nullptr;
  try {
    waitset = new WaitSet();
  } catch (...) {
    return;
  }
  if (waitset->addFd(this->rdma_queue_pairs[node_idx]->GetChannel()->fd) != 0) {
    LOG_DEBUG("RdmaServer receive thread of %lld, failed to add channel fd of %d to waitset", 
            node_idx, this->rdma_queue_pairs[node_idx]->GetChannel()->fd);
    return;
  }

  LOG_DEBUG("RdmaServer receive thread of %lld, success to add channel fd of %d to waitset"
          ", start to wait for requests", node_idx, this->rdma_queue_pairs[node_idx]->GetChannel()->fd);
  
  // 循环等待消息到来，并交给处理线程池中进行处理
  while (!this->stop) {
    int rc;
    epoll_event event;
    rc = waitset->waitSetWait(&event);
    if (rc < 0 && errno != EINTR) {
      return;
    }
    if (rc <= 0) {
      continue;
    }

    std::vector<struct ibv_wc> wcs;
    rc = this->rdma_queue_pairs[node_idx]->PollCompletionsFromCQ(wcs);
    if (rc < 0) {
      return;
    }

    for (int i = 0; i < rc; ++i) {
      struct ibv_wc &wc = wcs[i];
      if (wc.status != IBV_WC_SUCCESS) {
        return;
      }

      if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
        uint32_t  slot_idx = wc.imm_data;
        char    *buf = (char *)this->rdma_queue_pairs[node_idx]->GetLocalMemory() + 
                slot_idx * this->slot_size;
        this->worker_threadpool->Start(buf, node_idx, slot_idx);
      }
    }
  }

  LOG_DEBUG("RdmaServer receive thread of %lld, stop working", node_idx);
}

template<typename T>
void* RdmaServer<T>::receiveThreadFunEntry(void *arg) {
  Args *args = (Args *)arg;
  args->server->receiveThreadFun(args->node_idx);
  return args;
}

template<typename T>
int RdmaServer<T>::dataSyncWithSocket(int sock, uint32_t compute_id, const QueuePairMeta& meta,
            uint32_t &remote_compute_id, QueuePairMeta &remote_meta)
{
  LOG_DEBUG("RdmaServer, compute id of %lu, Start to dataSyncWithSocket, remote socket is %d, "
          "local_registered_memory=%llu, local_registered_key=%lu, local_qp_num=%lu, "
          "local_qp_psn=%lu, local_lid=%d", compute_id, sock, meta.registered_memory, 
          meta.registered_key, meta.qp_num, meta.qp_psn, meta.lid);
  
  size_t length = sizeof(uint32_t) + sizeof(QueuePairMeta) - sizeof(ibv_gid) + 6; // 6个分隔符
  char *send_buf  = nullptr;
  char *recv_buf  = nullptr;
  send_buf        = (char *)malloc(length);
  recv_buf        = (char *)malloc(length);
  char  *pointer  = send_buf;
  int    rc       = 0;
  size_t write_bytes = 0;
  size_t read_bytes  = 0;

  SCOPEEXIT([&]() {
    if (send_buf != nullptr) {
      free(send_buf);
    }
    if (recv_buf != nullptr) {
      free(recv_buf);
    }
  });
  
  // 先接收
  pointer = recv_buf;
  while (read_bytes < length) {
    rc = read(sock, pointer, length - read_bytes);
    if (rc <= 0) {
      return -1;
    } else {
      read_bytes += rc;
      pointer    += rc;
    }
  }

  sscanf(recv_buf, "%08x:%016lx:%08x:%08x:%08x:%04x:", &remote_compute_id, 
          &remote_meta.registered_memory, &remote_meta.registered_key,
          &remote_meta.qp_num, &remote_meta.qp_psn, &remote_meta.lid);
  remote_compute_id = be32toh(remote_compute_id);
  remote_meta.registered_memory = be64toh(remote_meta.registered_memory);
  remote_meta.registered_key    = be32toh(remote_meta.registered_key);
  remote_meta.qp_num            = be32toh(remote_meta.qp_num);
  remote_meta.qp_psn            = be32toh(remote_meta.qp_psn);
  remote_meta.lid               = be16toh(remote_meta.lid);

  LOG_DEBUG("RdmaServer, compute id of %lu, received sync data, remote_compute_id=%lu, "
          "remote_registered_memory=%llu, remote_registered_key=%lu, remote_qp_num=%lu, "
          "remote_qp_psn=%lu, remote_lid=%d", compute_id, remote_compute_id, remote_meta.registered_memory,
          remote_meta.registered_key, remote_meta.qp_num, remote_meta.qp_psn, remote_meta.lid);
  
  // 再发送
  sprintf(pointer, "%08x:", htobe32(compute_id));
  pointer += sizeof(uint32_t);
  sprintf(pointer, "%016lx:", htobe64(meta.registered_memory));
  pointer += sizeof(uintptr_t);
  sprintf(pointer, "%08x:", htobe32(meta.registered_key));
  pointer += sizeof(uint32_t);
  sprintf(pointer, "%08x:", htobe32(meta.qp_num));
  pointer += sizeof(uint32_t);
  sprintf(pointer, "%08x:", htobe32(meta.qp_psn));
  pointer += sizeof(uint32_t);
  sprintf(pointer, "%04x:", htobe16(meta.lid));
  pointer += sizeof(uint16_t);
  
  pointer = send_buf;
  while (write_bytes < length) {
    rc = write(sock, pointer, length);
    if (rc <= 0) {
      return -1;
    } else {
      write_bytes += rc;
      pointer     += rc;
    }
  }

  LOG_DEBUG("RdmaServer, compute id of %lu, success to dataSyncWithSocket, remote socket is %d, "
          "remote compute_id is %lu", compute_id, sock, remote_compute_id);

  return 0;
}


/** 
 * @todo: 检查各个输入是否合法，比如_port
 */
template<typename T>
RdmaServer<T>::RdmaServer(uint32_t _compute_num, uint32_t _node_num, uint64_t _slot_size, uint64_t _slot_num, 
        uint32_t _port, T *_worker_threadpool) 
        : compute_num(_compute_num), node_num(_node_num), slot_size(_slot_size), slot_num(_slot_size), 
          local_port(_port), worker_threadpool(_worker_threadpool)
{
  LOG_DEBUG("RdmaServer Start to construct RdmaServer");

  int cnt = this->compute_num * this->node_num;
  this->rdma_queue_pairs = new RdmaQueuePair*[cnt];
  for (int i = 0; i < cnt; ++i) {
    this->rdma_queue_pairs[i] = nullptr;
  }
  this->locks = new pthread_spinlock_t[cnt];
  for (int i = 0; i < cnt; ++i) {
    if (pthread_spin_init(&(this->locks[i]), 0) != 0) {
      throw std::bad_exception();
    }
  }

  this->receive_threads = new std::thread*[cnt];
  for (int i = 0; i < cnt; ++i) {
    this->receive_threads[i] = nullptr;
  }

  LOG_DEBUG("RdmaServer Success to construct RdmaServer: compute_num=%lld, node_num=%lld, "
          "slot_size=%lld, slot_num=%lld, listen_port=%d", this->compute_num, this->node_num,
          this->slot_size, this->slot_num, this->local_port);
}

template<typename T>
RdmaServer<T>::~RdmaServer() {
  LOG_DEBUG("RdmaServer start to deconstruct");

  this->Stop();
  
  if (this->rdma_queue_pairs != nullptr) {
    for (int i = 0; i < this->compute_num * this->node_num; ++i) {
      if (this->rdma_queue_pairs[i] != nullptr) {
        delete this->rdma_queue_pairs[i];
        this->rdma_queue_pairs[i] = nullptr;
      }
    }
    delete[] this->rdma_queue_pairs;
    this->rdma_queue_pairs = nullptr;

    delete this->locks;
    this->locks = nullptr;

    LOG_DEBUG("RdmaServer success to deconstruct, all resources including RDMA have been released");
  }
}

/** 
 * @todo: 释放arg_listen等
 */
template<typename T>
int RdmaServer<T>::Run() {
  LOG_DEBUG("Start to run RdmaServer");

  int cnt = this->compute_num * this->node_num;

  Args *arg_listen = new Args();
  arg_listen->server = this;
  try {
    this->listen_thread = new std::thread(RdmaServer<T>::listenThreadFunEntry, arg_listen);
  } catch (...) {
    return -1;
  }

  for (int i = 0; i < cnt; ++i) {
    Args *arg_receive = new Args();
    arg_receive->node_idx = i;
    arg_receive->server = this;
    try {
      this->receive_threads[i] = new std::thread(RdmaServer<T>::receiveThreadFunEntry, arg_receive);
    } catch (...) {
      return -1;
    }
  }

  LOG_DEBUG("Success to run RdmaServer: launched one listen_thread, %d receive_threads", cnt);

  return 0;
}

template<typename T>
void RdmaServer<T>::Stop() {
  LOG_DEBUG("RdmaServer start to stop all threads");

  this->stop = true;
  this->listen_thread->join();
  delete this->listen_thread;
  this->listen_thread = nullptr;

  for (int i = 0; i < this->compute_num * this->node_num; ++i)
  {
    this->receive_threads[i]->join();
    delete this->receive_threads[i];
    this->receive_threads[i] = nullptr;
  }
  delete[] this->receive_threads;
  this->receive_threads = nullptr;

  LOG_DEBUG("RdmaServer success to stop all threads");
}

template<typename T>
int RdmaServer<T>::PostResponse(uint64_t node_idx, uint64_t slot_idx) {
  RdmaQueuePair *qp = this->rdma_queue_pairs[node_idx];
  qp->SetSendContent((void *)0, 0, slot_idx);
  return qp->PostSend(slot_idx, slot_idx);
}


#endif