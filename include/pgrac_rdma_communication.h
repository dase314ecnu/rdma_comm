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
#include <functional>
#include <atomic>

#include "pgrac_configuration.h"
#include "test/test_shared_memory.h"
#include "pgrac_rdma_communication.h"
#include "pgrac_inner_scope.h"
#include "pgrac_waitset.h"
#include "pgrac_log.h"

class MemoryAllocator {
public:
  static size_t add_size(size_t s1, size_t s2, size_t align);
  void init(char *memory, size_t size);
  void reset();
  /**
   * size表示要分配空间的对象的大小
   * align表示对象以多少字节对齐，align必须是2的次方，且要大于等于4
   * 如果是128，则说明要使用cacheline优化
   */
  void *alloc(size_t size, size_t align = 4);

private:
  /* 获得size对齐后的大小 */
  static size_t alignedSize(size_t size, size_t align);

private:
  char *_memory = nullptr;
  size_t _size = 0;   // 内存总的大小
  size_t _cur_free_offset = 0;  // 需要从_cur_scratch处开始分配对象
};

typedef struct QueuePairMeta
{
  uintptr_t registered_memory;
  uint32_t registered_key;
  uint32_t qp_num;
  uint32_t qp_psn;
  uint16_t lid;
  union ibv_gid gid;
} QueuePairMetaData;

/** 
 * slot的状态转换是这个样子的：
 *   1. 首先slot是SLOT_IDLE状态
 *   2. 如果slot已经填充好数据，则是SLOT_INPROGRESS
 *   3. 如果slot上的响应已经被上层处理，则再转为SLOT_IDLE
 */
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
  /** 
   * 是否具有还未被发送的数据。如果has_unsent_data == 0，则说明没有数据填充到slot中，
   * 但并不意味着没有backend意图要在slot中填充数据。has_unsent_data == 1，则说明
   * 有backend已经将数据填充到slot中，亟待发送线程去发送。
   */
  union {
    std::atomic<int> has_notification{0};  
    char   pad_for_has_notification[CACHE_LINE_SIZE];
  } __attribute__ ((aligned(CACHE_LINE_SIZE)));

  /** 剩余free_slot_num个slot没有使用，是available的 */
  union {
    std::atomic<int> free_slot_num{0}; 
    char   pad_for_free_slot_num[CACHE_LINE_SIZE];
  } __attribute__ ((aligned(CACHE_LINE_SIZE)));

  union {
    std::atomic<int> notwrite_rear{0};  /** 所有还未被填充数据的slot的后面 */
    char   pad_for_notwrite_rear[CACHE_LINE_SIZE];
  } __attribute__ ((aligned(CACHE_LINE_SIZE)));
  
  /** 
   * front <= notsent_front <= rear <= notwrite_rear
   * 如果front == notwrite_rear，则意味着没有进程要发送数据
   * 如果notsent_front == rear，则意味着填充好的数据都发送了
   * 如果notsent_front == notwrite_rear，则意味着所有注定要发送的数据都发送出去了
   */
  int       front = 0;       /** 最旧的还处于SLOT_INPROGRESS的slot */
  int       rear = 0;       /** 最新的还处于SLOT_INPROGRESS的slot的后面 */
  int       notsent_front = 0;    /** front到rear中还未发送的slot的最旧的那个 */

  /** 
   * 表示每个slot上的请求者是否需要不同步等待最终的结果，如果不等待，则由发送线程来
   * 负责更新slot的状态，否则，应该由请求进程来更新slot的状态，因为请求进程需要先
   * 拿走结果之后，才能推进对应slot的状态。如果是发送线程来推进slot的状态，则有可能
   * 这个slot已经可用，并且数据被别的进程覆盖了，则当前进程无法获得正确的数据。
   * 正常情况下nowait是false，如果为true，则表示请求者希望直接异步发送请求，并且之后也不会等待结果
   * 数组要cache line对齐
   */
  bool      *nowait = nullptr;  
  /** 
   * 由于分片机制，所以有可能是某些slot组成一个消息，那么segment_nums[i]表示从i号slot
   * 开始的segment_nums[i]个slot是组成一个消息
   * 数组要cache line对齐
   */
  int       *segment_nums = nullptr;
  /** 一个QP的所有slot的状态。数组要cache line对齐 */
  std::atomic<SlotState> *states = nullptr;  
  
  ZSend() {}
  
  ZSend(int slot_num, MemoryAllocator *allocator) {
    this->free_slot_num.store(slot_num);
    try {
      size_t size = sizeof(bool) * (slot_num + 1);
      this->nowait = (bool *)(allocator->alloc(size, CACHE_LINE_SIZE));
      memset(this->nowait, 0, size);
      
      size = sizeof(int) * (slot_num + 1);
      this->segment_nums = (int *)(allocator->alloc(size, CACHE_LINE_SIZE));

      size = sizeof(std::atomic<SlotState>) * (slot_num + 1);
      this->states = (std::atomic<SlotState> *)(allocator->alloc(size, CACHE_LINE_SIZE));
      for (int i = 0; i < slot_num + 1; ++i) {
        this->states[i].store(SlotState::SLOT_IDLE);
      }
    } catch (...) {
      throw std::bad_exception();
    }
  }

  static size_t GetSharedObjSize(int slot_num) {
    size_t size = 0;
    size += MemoryAllocator::add_size(size, sizeof(bool) * (slot_num + 1), CACHE_LINE_SIZE);
    size += MemoryAllocator::add_size(size, sizeof(int) * (slot_num + 1), CACHE_LINE_SIZE);
    size += MemoryAllocator::add_size(size, sizeof(std::atomic<SlotState>) * (slot_num + 1), CACHE_LINE_SIZE);
    return size;
  }
} ZSend;

/** 用于将ZSend占用空间pad到CACHE_LINE_SIZE字节对齐 */
typedef union ZSendPad {
  ZSend zsend;
  unsigned char _pad[(sizeof(zsend) + CACHE_LINE_SIZE - 1) & (~((size_t) (CACHE_LINE_SIZE - 1)))];
} ZSendPad;

typedef struct ZAwake {
    /** 每个slot都对应一个sem_t，每当进程/线程要等待响应时，就使用这里的sems */
    sem_t          *sems = nullptr;  
    /** 如果是忙等，也就是定义了USE_BACKEND_BUSY_POLLING，则需要原子变量来标识消息是否执行完成。*/
    volatile bool  *done = nullptr;

    ZAwake() {}
    ZAwake(int slot_num, MemoryAllocator *allocator) {
      int i = 0;
      size_t size = 0;
      SCOPEEXIT([&]() {
        if (i < slot_num + 1) {
          // sems并没有完全初始化完成
          if (this->sems != nullptr) {
            for (int j = 0; j < i; ++j) {
              (void) sem_destroy(&(this->sems[j]));
            }
            this->sems = nullptr;
          }
        }
      });
      
      try {
        size = sizeof(sem_t) * (slot_num + 1);
        this->sems = (sem_t *)(allocator->alloc(size, CACHE_LINE_SIZE));
        size = sizeof(volatile bool) * (slot_num + 1);
        this->done = (volatile bool *)(allocator->alloc(size, CACHE_LINE_SIZE));
      } catch (...) {
        throw std::bad_exception();
      }

      for (i = 0; i < slot_num + 1; ++i) {
        if (sem_init(&(this->sems[i]), 1, 0) != 0) {
          throw std::bad_exception();
        }
      }
      for (int i = 0; i < slot_num + 1; ++i) {
        this->done[i] = false;
      }
    }

    static size_t GetSharedObjSize(int slot_num) {
      size_t size = 0;
      size += MemoryAllocator::add_size(size, sizeof(sem_t) * (slot_num + 1), CACHE_LINE_SIZE);
      size += MemoryAllocator::add_size(size, sizeof(volatile bool) * (slot_num + 1), CACHE_LINE_SIZE);
      return size;
    }
} ZAwake;

/**
 * 负责建立QP连接，在QP上发布发送和接收任务，以及检查cq中是否有完成事件到来
 */
class RdmaQueuePair{
public:
  RdmaQueuePair(int local_slot_num, int local_slot_size, 
          const char *device_name = "mlx5_0", uint32_t rdma_port = 1);
  RdmaQueuePair(int local_slot_num, int local_slot_size, 
          void *shared_memory, const char *device_name = "mlx5_0", uint32_t rdma_port = 1);
  ~RdmaQueuePair();
  /* 销毁RdmdaQueuePair中创建所有的资源 */
  void Destroy();
  void*    GetLocalMemory();
  size_t GetLocalMemorySize();
  /** 得到ibv_comp_channel */
  ibv_comp_channel  *GetChannel();
  /** 在slot_idx的slot处赋值长度为size的send_content */
  void SetSendContent(void *send_content, int size, int slot_idx);
  /** 将slot_idx处的slot发送给对端 */
  int PostSend(uint32_t imm_data, int slot_idx, int length);
  /** 向RNIC发送一个recv wr */
  int PostReceive();
  /** 
   * 从CQ中取出多个WC，非阻塞
   * @return 1: 成功取出 0：没有WC -1：出现异常
   */
  int  PollCompletionsFromCQ(std::vector<struct ibv_wc> &wcs); 
  int ReadyToUseQP();
  void GetLocalQPMetaData(QueuePairMetaData &local_data);
  void SetRemoteQPMetaData(QueuePairMetaData &remote_data);

public:
// zhouhuahui test
// private:
  bool                    use_shared{false};
  /* local rdma queue pair resources */
  uint32_t                 rdma_port = 0;    //用于rdma查询的端口号: ibv_query_port。一般是1
  const char              *device_name = nullptr;  //设备名称。一般是"mlx5_0"
  struct ibv_context	    *ctx = nullptr;  // device handle 
  struct ibv_pd           *pd = nullptr;  // Protection Domain handler 
  struct ibv_port_attr     port_attribute;
  struct ibv_comp_channel *channel = nullptr;
  struct ibv_cq           *cq = nullptr;              // Completion Queue
  struct ibv_qp           *qp = nullptr;             // queue pair with remote qp 
  union  ibv_gid           local_gid;
  uint32_t                 qp_psn;
  
  int              local_memory_size = 0;   // QP中的mr的大小
  void             *local_memory = 0;   // Memory begin address
  struct ibv_mr    *local_mr = nullptr;       // Memory registration handler
  int             local_slot_size = 0;   // local_memory包含多个slot，每个slot可以容纳一个消息大小
  int             local_slot_num = 0;    /* local_memory包含local_slot_num个slot，
                                              因此：local_memory_size == local_slot_size * (local_slot_num + 1)
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
};

/** 
 * 
 */
class RdmaClient {
public:
  RdmaClient() {}
  /** 创建相关RDMA资源 */
  RdmaClient(std::string remote_ip, uint32_t remote_port, MemoryAllocator *allocator,
          int node_num, int slot_size, int slot_num);
  void init(std::string remote_ip, uint32_t remote_port, MemoryAllocator *allocator,
          int node_num, int slot_size, int slot_num);
  ~RdmaClient();
  /** Destroy all the resources of RdmaClient */
  void Destroy();

public:
// zhouhuahui test
// protected:
  /** 用于发送任务请求和接收响应的QP。node_num长度 */
  RdmaQueuePair     **_rdma_queue_pairs = nullptr;
  MemoryAllocator    *_allocator = nullptr;
  int                 _node_num = 0;
  int                 _slot_size = 0;
  int                 _slot_num = 0;

private:
  std::string         _remote_ip;    //连接远程服务器的ip
  uint32_t            _remote_port = 0;  //连接远程服务器的端口

private:
  int createRdmaQueuePairs();
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
  int  dataSyncWithSocket(int sock, int compute_id, const QueuePairMeta& meta,
          int &remote_compute_id, QueuePairMeta &remote_meta);
};


/** 
 * 分片机制：
 * 将一个消息数据保存到多个slot分片中。假如我们的slot的编号是[1, 100)，且rear是98，
 * 假如我们的数据大小是2个slot，则可以将98号slot设置为SLOT_SEGMENT_TYPE_FIRST，将
 * 99号slot设置为SLOT_SEGMENT_TYPE_LAST。如果我们的数据大小是3个slot，则可以将
 * 98, 99号slot设置为SLOT_SEGMENT_TYPE_NODATA，将0号slot设置为SLOT_SEGMENT_TYPE_FIRST,
 * 将1号slot设置为SLOT_SEGMENT_TYPE_MORE，将2号slot设置为SLOT_SEGMENT_TYPE_LAST，然后将
 * 5个slot发送出去。
 */
enum SlotSegmentType {
  /* 这个slot没有数据，也意味着这个slot不会有回复 */
  SLOT_SEGMENT_TYPE_NODATA,
  /* 这个slot是正常的slot，保存有数据，并且数据长度在一个slot内 */
  SLOT_SEGMENT_TYPE_NORMAL,
  /* 这个slot是一个分片slot，并且是第一个分片 */
  SLOT_SEGMENT_TYPE_FIRST,
  /* 这个slot是一个分片slot，并且是中间的一个分片 */
  SLOT_SEGMENT_TYPE_MORE,
  /* 这个slot是一个分片slot，并且是最后一个分片 */
  SLOT_SEGMENT_TYPE_LAST,
  /* 未定义 */
  SLOT_SEGMENT_OTHER
};
/* SlotMeta是保存在每个slot前面的元数据 */
typedef struct SlotMeta {
  /** 分片属性 */
  SlotSegmentType slot_segment_type;
  /* 整个packet的长度 */
  int size; 
} SlotMeta;

/** 
 * 从消息缓冲区中解析出4个字节的长度字段
 */
class MessageUtil {
public:
  static int parseLength(void *buf) {
    char *b = (char *)buf;
    b = b + sizeof(SlotMeta);
    int *length = reinterpret_cast<int *>(b);
    return *length;
  }

  static int parseLength2(void *buf) {
    char *b = (char *)buf;
    int *length = reinterpret_cast<int *>(b);
    return *length;
  }

  static int parsePacketLength(void *buf) {
    SlotMeta *meta = (SlotMeta *)buf;
    return meta->size;
  }
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
 * 
 * 每个slot的前面都是一个元数据结构体，这个元数据过后才是真正要传输的数据；元数据是为了某些特性而使用，比如分片机制。
 * 
 * 分片机制：由于一个slot可能无法保存很大的请求消息，因此我们需要将一个消息分片，放在多个slot中进行发送，接收端将
 * 这个多个分片的slot组合成一个完整的消息。
 */
class SharedRdmaClient : public RdmaClient {
    friend class TestSharedMemoryClass;
public:
  /** 
   * 将RdmaQueuePair.local_memory、RdmaClient.sends、RdmaClient.awakes,  
   * 等拷贝到shared_memory共享内存中 
   * listen_fd: node_num个socket pair
   * */
  SharedRdmaClient(int slot_size, int slot_num, std::string remote_ip, uint32_t remote_port, 
          int node_num, void* shared_memory, int *listend_fd);
  ~SharedRdmaClient();
  /** 启动所有发送线程 */
  int Run();
  /** 终止所有发送线程 */
  void Stop();
  /** Destroy all the resources of the SharedRdmaClient */
  void Destroy();

  void WaitForResponse(int node_idx, int rear, void **response);

  /** 
   * 调用SharedRdmaClient的发送消息的接口，返回内容放在response中。
   * PostRequest()负责申请响应的存储空间，然后将响应复制到这个空间中，然后将这个空间的地址复制给*response
   */
  int PostRequest(void *send_content, int size, void **response);

  template<class T>
  int PostRequest(void *send_content, int size, T callback) {
    return postRequest(send_content, size, nullptr, callback, false);
  }
  
  /** 
   * 这个是PostRequest()的半同步版本。它在调用ibv_post_send()之后不会等待响应到来，而是直接返回。
   * 但是最终调用者需要使用返回的wait对象来等待响应
   */
  auto AsyncPostRequest(void *send_content, int size, int* ret) 
      -> decltype(std::bind(&SharedRdmaClient::WaitForResponse, (SharedRdmaClient *)(nullptr), 
      int(0), (int)(0), std::placeholders::_1));
  
  /** 
   * 和AsyncPostRequest()类似，是个纯异步的api。但是上层不需要等待结果
   */
  void AsyncPostRequestNowait(void *send_content, int size, int *ret);

  // /** 
  //  * 和AsyncPostRequest()类似，是个纯异步的api。但是上层不需要等待结果，只需要传递callback到下层
  //  */
  // template<class T>
  // void AsyncPostRequestNowait(void *send_content, int size, int *ret, T callback);
  
  /** 计算SharedRdmaClient需要多少字节的共享内存空间来创建对象，包括SharedRdmaClient本身
   * 以及需要共享的数据的总大小。
   * 这个函数和SharedRdmaClient(), RdmaClient(), createRdmaQueuePairs()函数紧耦合了。
   */
  static size_t GetSharedObjSize(int slot_size, int slot_num, int node_num);

private:
    struct Args {
        int node_idx = 0;
        SharedRdmaClient *client = nullptr;
    };

public:
// zhouhuahui test
// private:
    /** 
     * 用于RoundRobin算法
     * 每次从start_idx号的node找可以用于发送的node，每次PostRequest后start_idx = (start_idx + 1) % node_num
     */
    union {
      std::atomic<int> _start_idx{0};
      char   pad_for_start_idx[CACHE_LINE_SIZE];
    } __attribute__ ((aligned(CACHE_LINE_SIZE)));
    
    /** 
     * stop为true，表示要终止所有发送线程，发送线程要循环检查这个变量
     */
    volatile bool _stop = false;

    /** 
     * 每个node（发送线程）监听一个listen_fd[i][1]，如果有消息来，
     * 说明某个slot中含有要发送的数据。
     * */
    int       *_listen_fd = nullptr;     /* node_num * 2长度 */
    pthread_t  *_send_threads = nullptr;  /* node_num长度 */

    ZSendPad      *_sends = nullptr;        // node_num长度
    ZAwake     *_awakes = nullptr;       // node_num长度

public:
// zhouhuahui test
// private:
    /** 发送线程执行的代码 */
    void sendThreadFun(int node_idx);
    static void *sendThreadFunEntry(void *arg);

    /** 
     * 负载均衡策略为round robin的负载均衡算法。
     * 找到一个合适的node和合适的slot，然后将send_content中的内容复制过去
     * out_node_idx: 输出参数，找到的需要进行数据发送的node的编号
     * out_rear: 输出参数，node_idx号node上，在rear号开始的几个slot上存放要发送的数据
     */
    int rrLoadBalanceStrategy(void *send_content, int size, bool nowait, 
            int *out_node_idx, int *out_rear);
    
    /* 由于分片机制，需要确定一个消息需要多少个slot分片来发送 */
    int getNeededSegmentNum(int size);

    /** 
     * 由于分片机制，检查node_idx号node是否可以用于发送数据。若slot有100个，是0 - 99，
     * 若rear是99，segment_num是2，则需要用99, 0号slot来发送，只是消息有些不连续了，服务端需要处理这个情况
     * 若可以的话，则将send_content中的内容填充到对应的slot中。
     * 
     * start_rear: 从start_rear开始填充的数据
     * rear2: 输出参数。如果成功check，则更新后的rear是什么
     */
    bool checkNodeCanSend(int node_idx, void *send_content, int size, 
            int *start_rear, int *rear2);
    
    // @todo
    /** 
     * callback是处理响应的可调用对象，它的参数只能是一个：void *。也就是需要这样调用它：callback(buf)
     * 如果callback为空，则需要将buf复制到上层，让上层去处理。
     * 建议传递binder对象到callback中
     * rear: backend从rear开始的很多slot存储着要发送的消息，因此rear也是响应到来的那个slot
     */
    template<class T>
    void waitForResponse(int node_idx, int rear, void **response, T callback) {
      char c;
      int rc;
      ZSend *zsend = &(_sends[node_idx].zsend);
      ZAwake *zawake = &(_awakes[node_idx]);
      char *buf = (char *)_rdma_queue_pairs[node_idx]->GetLocalMemory() 
                  + rear * _slot_size;
      
      if (!USE_BACKEND_BUSY_POLLING) {
        (void) sem_wait(&(zawake->sems[rear]));
      } else {
        while (zawake->done[rear] == false);
        zawake->done[rear] =false;
      }
      
      if constexpr (std::is_same<T, decltype(nullptr)>::value) {
        /** 此时response必不能为空
         * 响应内容得到后，需要将它传递给上层，然后便更新zsend中的一些元信息
         * 响应中的前四个字节必定是长度字段
         */
        int length = MessageUtil::parseLength2(buf);
        *response = malloc(length);
        memcpy(*response, buf, length);
      } else {
        callback(buf);
      }

      /** 更新zsend中的一些元信息 
       * 这里修理了一个很不起眼的bug：必须要先把zsend->segment_nums[rear]取到msg_num中，
       * 不能这样用：for (int k = 0; k < zsend->segment_nums[rear]; ++k)。
       * 因为在更新zsend->states的过程中，zsend->front可能也会同步更新，进而zsend->free_slot_num
       * 也会更新，最终有新的请求会看到有足够的zsend->free_slot_num了，进而在rear处放置消息，进而
       * 更新了zsend->segment_nums[rear]，而我们这里就会读到错误的值。
       */
      int msg_num = zsend->segment_nums[rear];
      for (int k = 0; k < msg_num; ++k) {
        int p = (rear + k) % (_slot_num + 1);
        zsend->states[p].store(SlotState::SLOT_IDLE);
      }
      std::atomic_thread_fence(std::memory_order_seq_cst);
      zsend->has_notification.store(1);
      if (!USE_BUSY_POLLING) {
        rc = send(_listen_fd[node_idx * 2], &c, 1, 0); 
      }
    }
    
    /** 
     * nowait: 表示是否不需要等待结果，false是默认情况，见ZSend中关于nowait的注释
     */
    template<class T>
    int postRequest(void *send_content, int size, void **response, T callback, bool nowait) {
      int ret = 0;
      int node_idx;
      int rear;
      ret = this->rrLoadBalanceStrategy(send_content, size, nowait, &node_idx, &rear);
      if (ret != 0) {
        return -1;
      }
      ZSend  *zsend = &(_sends[node_idx].zsend);
      ZAwake *zawake = &_awakes[node_idx];
      char *buf = (char *)_rdma_queue_pairs[node_idx]->GetLocalMemory() 
                  + rear * _slot_size;
      this->waitForResponse(node_idx, rear, response, callback);
      return 0;
    }
};


/** 
 * 负责接收来自己多个计算节点的消息，并分配工作线程来处理。
 * 远程的RDMA Write的imm_data字段会保存slot的号，因此RdmaServer可以直接知道从哪个slot中读取消息
 * @parma T: 业务处理线程池的实现
 */
template<typename T>
class RdmaServer {
public:
    /** 
     * 初始化各个类成员属性。
     * 为rdma_queue_pairs和receive_threads分配内存空间。
     * 初始化locks
     */
    RdmaServer(int compute_num, int node_num, int slot_size, int slot_num, 
            uint32_t port, T *_worker_threadpool);
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
     * 服务器发送响应之后，客户端才可以释放对应slot的锁，因此PostResponse()时，必定不会在同一个slot
     * 中到来消息，因此不用考虑并发的问题。
     * @param node_idx: 小于compute_num * node_num
     * @param response: reponse的长度必定小于等于slot_size
     */
    int PostResponse(int node_idx, int slot_idx, void *response);

private:
    struct Args {
        int node_idx = 0;  // 小于compute_num * node_num
        RdmaServer<T> *server = nullptr;
    };

private:
    int           compute_num = 0;  // 计算机器的个数
    int           node_num = 0;   // 对应每个来连接的计算节点，分配node_num个接收器
    int           slot_size = 0;
    int           slot_num = 0;
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
    
    /** shadow_pool的长度是compute_num*node_num，每个元素是一个长度为slot_size*slot_num的内存池
     * 这里用于存储在注册内存中不连续的大消息
     */
    char **shadow_pool = nullptr;
    
    uint32_t    local_port = 0;   // 监听线程的监听端口
    std::thread **receive_threads = nullptr; // compute_num * node_num长度
    std::thread  *listen_thread = nullptr;   // 监听线程，用于处理来自其他计算节点的连接请求

    T   *worker_threadpool = nullptr;   // 业务处理线程池
    volatile bool stop = false;         // 线程是否停止接收请求

private:
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
    void receiveThreadFun(int node_idx);
    static void *receiveThreadFunEntry(void *arg);
    /** 
     * @param compute_id: 本节点的节点号
     * @param meta      : 本节点的QueuePairMeta信息
     * @param remote_compute_id : 对端节点的节点号
     * @param remote_meta       : 对端节点的QueuePairMeta信息
     * @return  0: 成功  -1: 出现异常
     * 交换信息的时候，要考虑到大小端转换的问题，虽然没有这一步也大概率没事。
     */
    int  dataSyncWithSocket(int sock, int compute_id, const QueuePairMeta& meta,
            int &remote_compute_id, QueuePairMeta &remote_meta);
    
    /** 
     * 由于分片机制：
     * 将last_head到slot_idx号slot之间的所有分片都合并在一起，合并后的消息的首地址在
     * last_head号slot的sizeof(SlotMeta)偏移处。
     * 注意：确保last_head到slot_idx号slot确实是一个消息的分片
     */
    void mergeMultipleSegments(int last_head, int slot_idx, int node_idx);
    void mergeMultipleSegments2(int last_head, int slot_idx, int node_idx, char *dest_buf);
    void mergeAndMoveMultipleSegments(int last_head, int slot_idx, int node_idx);
};

/** 
 * ---------------------------------------------------------------------------------------------
 * RdmaServer 实现
 * ----------------------------------------------------------------------------------------------
 */

template<typename T>
RdmaServer<T>::RdmaServer(int compute_num, int node_num, int slot_size, int slot_num, 
        uint32_t port, T *worker_threadpool) 
        : compute_num(compute_num), node_num(node_num), slot_size(slot_size), slot_num(slot_num), 
          local_port(port), worker_threadpool(worker_threadpool)
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

  this->shadow_pool = (char **)malloc(sizeof(char *) * this->compute_num * this->node_num);
  for (int i = 0; i < this->compute_num * this->node_num; ++i) {
    this->shadow_pool[i] = (char *)malloc(this->slot_num * this->slot_size);
  }

  LOG_DEBUG("RdmaServer Success to construct RdmaServer: compute_num=%d, node_num=%d, "
          "slot_size=%d, slot_num=%d, listen_port=%u", this->compute_num, this->node_num,
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
  }
  if (this->locks != nullptr) {
    delete[] this->locks;
    this->locks = nullptr;
  }

  for (int i = 0; i < this->compute_num * this->node_num; ++i) {
    free(this->shadow_pool[i]);
    this->shadow_pool[i] = nullptr;
  } 
  free(this->shadow_pool);
  this->shadow_pool = nullptr;

  LOG_DEBUG("RdmaServer success to deconstruct, all resources including RDMA have been released");
}

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
int RdmaServer<T>::PostResponse(int node_idx, int slot_idx, void *response) {
  if (response == nullptr) {
    return -1;
  }
  RdmaQueuePair *qp = this->rdma_queue_pairs[node_idx];
  int length = MessageUtil::parseLength2(response);
  if (length < 4) {
    return -1;
  }
  qp->SetSendContent(response, length, slot_idx);
  return qp->PostSend(slot_idx, slot_idx, length);
}

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

  LOG_DEBUG("RdmaServer listen thread, is listening connection, listen_port=%u", local_port);

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
    int      compute_id = MY_COMPUTE_ID;
    int      remote_compute_id;     
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
    int idx = this->node_num * remote_compute_id + comp_counter[remote_compute_id];
    comp_counter[remote_compute_id]++;
    pthread_spin_lock(&(this->locks[idx]));
    this->rdma_queue_pairs[idx] = qp;
    pthread_spin_unlock(&(this->locks[idx]));
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
void RdmaServer<T>::receiveThreadFun(int node_idx) {
  LOG_DEBUG("RdmaServer receive thread of %u, start to wait rdma connection to be set", node_idx);
  
  // 统计信息，接收请求的数量
  uint64_t receive_cnt = 0;
  uint64_t send_cnt = 0;
  WaitSet *waitset = nullptr;
  SCOPEEXIT([&]() {
    this->stop = true;
    if (waitset != nullptr) {
      delete waitset; 
      waitset = nullptr;
    }
    LOG_DEBUG("RdmaServer receive thread of %d, stop working, have received %lu requests", 
          node_idx, receive_cnt);
  });
  
  try {
    waitset = new WaitSet();
  } catch (...) {
    return;
  }

  // 等待qp建立连接
  while (!this->stop) {
    pthread_spin_lock(&(this->locks[node_idx]));
    if (this->rdma_queue_pairs[node_idx] == nullptr) {
      pthread_spin_unlock(&(this->locks[node_idx]));
      sleep(2);
      continue;
    }
    pthread_spin_unlock(&(this->locks[node_idx]));
    break;
  }

  if (this->stop) {
    return;
  }

  if (waitset->addFd(this->rdma_queue_pairs[node_idx]->GetChannel()->fd) != 0) {
    LOG_DEBUG("RdmaServer receive thread of %d, failed to add channel fd of %d to waitset", 
            node_idx, this->rdma_queue_pairs[node_idx]->GetChannel()->fd);
    return;
  }

  LOG_DEBUG("RdmaServer receive thread of %d, success to add channel fd of %d to waitset"
          ", start to wait for requests", node_idx, this->rdma_queue_pairs[node_idx]->GetChannel()->fd);
  
  /** 
   * 由于分片机制。
   * 如果遇到了一个slot，他的meta中显示是SLOT_SEGMENT_TYPE_FIRST，则将这个slot的号赋值到last_head中。
   * 如果遇到了一个slot，他的meta中显示是SLOT_SEGMENT_TYPE_LAST，则将这个last_head到这个slot是一个完整的消息。
   */
  int last_head;
  // 循环等待消息到来，并交给处理线程池中进行处理
  while (!this->stop) {
    int rc;
    epoll_event event;
    // 如果不使用忙等，则需要注册wait set来使用epoll等待
    if (!USE_BUSY_POLLING) {
      rc = waitset->waitSetWait(&event);
      if (rc < 0 && errno != EINTR) {
        return;
      }
      if (rc <= 0) {
        continue;
      }
    }

    std::vector<struct ibv_wc> wcs;
    rc = this->rdma_queue_pairs[node_idx]->PollCompletionsFromCQ(wcs);
    if (rc < 0) {
      LOG_DEBUG("RdmaServer recieve thread of %u, failed to poll completions from CQ", node_idx);
      return;
    }

    for (int i = 0; i < rc; ++i) {
      struct ibv_wc &wc = wcs[i];
      if (wc.status != IBV_WC_SUCCESS) {
        LOG_DEBUG("RdmaServer recieve thread of %u, failed to get a IBV_WC_SUCCESSed wc, wc.status is %d"
                , node_idx, wc.status);
        return;
      }

      if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
        receive_cnt++;
        
        int slot_idx, msg_num = 1;
        if (!USE_GROUP_POST_SEND) {
          slot_idx = wc.imm_data;
        } else {
          /** 使用了组发送机制，则imm_data中包含了起始的slot_idx，以及消息个数msg_num */
          slot_idx = GET_SLOT_IDX_FROM_IMM_DATA(wc.imm_data);
          msg_num = GET_MSG_NUM_FROM_IMM_DATA(wc.imm_data);
        }
        if (this->rdma_queue_pairs[node_idx]->PostReceive() != 0) {
          LOG_DEBUG("RdmaServer receive thread of %u, failed to post receive");
          return;
        }
        for (int j = 0; j < msg_num; ++j) {
          char    *buf = (char *)this->rdma_queue_pairs[node_idx]->GetLocalMemory() + 
                  slot_idx * this->slot_size;
          SlotMeta *meta = (SlotMeta *)buf;
          if (meta->slot_segment_type == SlotSegmentType::SLOT_SEGMENT_TYPE_NORMAL) {
            buf = buf + sizeof(SlotMeta);
            /** 调用工作线程池的接口，将请求发给工作线程池进行处理 */
            this->worker_threadpool->Start(buf, node_idx, slot_idx);
          } else if (meta->slot_segment_type == SlotSegmentType::SLOT_SEGMENT_TYPE_FIRST) {
            last_head = slot_idx;
          } else if (meta->slot_segment_type == SlotSegmentType::SLOT_SEGMENT_TYPE_MORE) {
            // ....
          } else if (meta->slot_segment_type == SlotSegmentType::SLOT_SEGMENT_TYPE_LAST) {
            /** 如果slot_idx > last_head，则说明这个大消息的分片在缓冲区中是连续的，否则的话，
             * 这个大消息的分片在last_head到缓冲区末尾，缓冲区头到slot_idx
             */
            char *buf;
            if (slot_idx > last_head) {
              this->mergeMultipleSegments(last_head, slot_idx, node_idx);
              buf = (char *)this->rdma_queue_pairs[node_idx]->GetLocalMemory() +
                      last_head * this->slot_size;
            } else {
              this->mergeAndMoveMultipleSegments(last_head, slot_idx, node_idx);
              buf = this->shadow_pool[node_idx];
            }
            buf += sizeof(SlotMeta);
            /** 调用工作线程池的接口，将请求发给工作线程池进行处理 */
            this->worker_threadpool->Start(buf, node_idx, last_head);
          }
          slot_idx++;
        }
      } else if (wc.opcode == IBV_WC_RDMA_WRITE) {
        // ......
      }
    }
  }
}

template<typename T>
void* RdmaServer<T>::receiveThreadFunEntry(void *arg) {
  Args *args = (Args *)arg;
  args->server->receiveThreadFun(args->node_idx);
  return args;
}

template<typename T>
int RdmaServer<T>::dataSyncWithSocket(int sock, int compute_id, const QueuePairMeta& meta,
            int &remote_compute_id, QueuePairMeta &remote_meta)
{
  size_t length = 26;
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

  LOG_DEBUG("RdmaServer, compute id of %d, Start to dataSyncWithSocket, remote socket is %d, "
          "local_registered_memory=%lu, local_registered_key=%u, local_qp_num=%u, "
          "local_qp_psn=%u, local_lid=%hu", compute_id, sock, meta.registered_memory, 
          meta.registered_key, meta.qp_num, meta.qp_psn, meta.lid);
  
  // input: pointer, pointer
  auto addUint32 = [&](uint32_t x) {
    x = htobe32(x);
    memcpy(pointer, reinterpret_cast<char *>(&x), sizeof(uint32_t));
    pointer += sizeof(uint32_t);
  };
  auto addUint64 = [&](uint64_t x) {
    x = htobe64(x);
    memcpy(pointer, reinterpret_cast<char *>(&x), sizeof(uint64_t));
    pointer += sizeof(uint64_t);
  };
  auto addUint16 = [&](uint16_t x) {
    x = htobe16(x);
    memcpy(pointer, reinterpret_cast<char *>(&x), sizeof(uint16_t));
    pointer += sizeof(uint16_t);
  };

  auto getUint32 = [&]() -> uint32_t {
    uint32_t x;
    memcpy(reinterpret_cast<char *>(&x), pointer, sizeof(uint32_t));
    x = be32toh(x);
    pointer += sizeof(uint32_t);
    return x;
  };
  auto getUint64 = [&]() -> uint64_t {
    uint64_t x;
    memcpy(reinterpret_cast<char *>(&x), pointer, sizeof(uint64_t));
    x = be64toh(x);
    pointer += sizeof(uint64_t);
    return x;
  };
  auto getUint16 = [&]() -> uint16_t {
    uint16_t x;
    memcpy(reinterpret_cast<char *>(&x), pointer, sizeof(uint16_t));
    x = be16toh(x);
    pointer += sizeof(uint16_t);
    return x;
  };

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

  pointer = recv_buf;
  remote_compute_id = getUint32();
  remote_meta.registered_memory = getUint64();
  remote_meta.registered_key = getUint32();
  remote_meta.qp_num = getUint32();
  remote_meta.qp_psn = getUint32();
  remote_meta.lid = getUint16();

  LOG_DEBUG("RdmaServer, compute id of %d, received sync data, remote_compute_id=%d, "
          "remote_registered_memory=%lu, remote_registered_key=%u, remote_qp_num=%u, "
          "remote_qp_psn=%u, remote_lid=%hu", compute_id, remote_compute_id, remote_meta.registered_memory,
          remote_meta.registered_key, remote_meta.qp_num, remote_meta.qp_psn, remote_meta.lid);
  
  pointer = send_buf;
  addUint32(compute_id);
  addUint64(meta.registered_memory);
  addUint32(meta.registered_key);
  addUint32(meta.qp_num);
  addUint32(meta.qp_psn);
  addUint16(meta.lid);

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

  return 0;
}

template<typename T>
void RdmaServer<T>::mergeMultipleSegments(int last_head, int slot_idx, int node_idx) {
  char *dest_buf = (char *)this->rdma_queue_pairs[node_idx]->GetLocalMemory() +
          (last_head + 1) * this->slot_size;
  this->mergeMultipleSegments2(last_head, slot_idx, node_idx, dest_buf);
}

template<typename T>
void RdmaServer<T>::mergeMultipleSegments2(int last_head, int slot_idx, int node_idx, char *dest_buf) {
  int k = (last_head + 1) % (this->slot_num + 1);
  for (; ; k = (k + 1) % (this->slot_num + 1)) {
    char *src_buf = (char *)this->rdma_queue_pairs[node_idx]->GetLocalMemory() +
          k * this->slot_size;
    int len = MessageUtil::parsePacketLength(src_buf) - sizeof(SlotMeta);

    src_buf += sizeof(SlotMeta);
    memmove(dest_buf, src_buf, len);
    dest_buf += len;
    if (k == slot_idx) {
      break;
    }
  }
}

template<typename T>
void RdmaServer<T>::mergeAndMoveMultipleSegments(int last_head, int slot_idx, int node_idx) {
  char *dest_buf = this->shadow_pool[node_idx];
  /** 把last_head号的slot中的内存复制到dest_buf中 */
  char *src_buf = (char *)this->rdma_queue_pairs[node_idx]->GetLocalMemory()
          + last_head * this->slot_size;
  memmove(dest_buf, src_buf, this->slot_size);
  dest_buf += this->slot_size;
  this->mergeMultipleSegments2(last_head, slot_idx, node_idx, dest_buf);
}


#endif