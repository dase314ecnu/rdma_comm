#include <sys/socket.h>
#include <assert.h>
#include <semaphore.h>
#include <infiniband/verbs.h>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <exception>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <errno.h>
#include <vector>
#include <map>
#include <stdint.h>
#include <algorithm>

#include "pgrac_rdma_communication.h"
#include "pgrac_inner_scope.h"
#include "pgrac_waitset.h"
#include "pgrac_log.h"

/** 
 * ---------------------------------------------------------------------------------------------
 * MemoryAllocator
 * ---------------------------------------------------------------------------------------------
 */
size_t MemoryAllocator::add_size(size_t s1, size_t s2, size_t align) {
  return alignedSize(s1, align) + s2;
}

void MemoryAllocator::init(char *memory, size_t size) {
  uintptr_t scratch = reinterpret_cast<uintptr_t>(memory);
  uintptr_t scratch_aligned = this->alignedSize(scratch, CACHE_LINE_SIZE);
  uintptr_t offset = scratch_aligned - scratch;
  if (offset >= _size) {
    throw std::bad_exception();
  }
  _memory = memory + offset;
  _size = size - offset;
}

void MemoryAllocator::reset() {
  _memory = nullptr;
  _size = 0;
  _cur_free_offset = 0;
}

void *MemoryAllocator::alloc(size_t size, size_t align) {
  uintptr_t new_offset = this->alignedSize(_cur_free_offset, align);
  if (new_offset + size > _size) {
    throw std::bad_exception();
  }
  char *res = _memory + new_offset;
  _cur_free_offset = new_offset + size;
  return (void *)(res);
}

size_t MemoryAllocator::alignedSize(size_t size, size_t align) {
  return (((size_t) size + (align - 1)) & ~((size_t) (align - 1)));
}


/**
 * ----------------------------------------------------------------------
 * RdmaQueuePair
 * ----------------------------------------------------------------------
 */

int RdmaQueuePair::initializeLocalRdmaResource() {
  struct ibv_device **device_list = nullptr;
  int rc = 0;
  int mr_flags = 0;

  if (this->ctx == nullptr) {
    struct ibv_device *dev = nullptr;
    int devices_num;

    device_list = ibv_get_device_list(&devices_num);
    if (device_list == nullptr) { 
      return -1;
    }
    if (devices_num <= 0) {
      return -1;
    }
    /* search for the specific device we want to work with */
    for (int i = 0; i < devices_num; ++i) {
      if (this->device_name == nullptr) {
        this->device_name = strdup(ibv_get_device_name(device_list[i]));
      }
      if (strcmp(ibv_get_device_name(device_list[i]), this->device_name) == 0) {
        dev = device_list[i];
        break;
      }
    }
    if (dev == nullptr) {
      return -1;
    }
    /* get device handle */
    this->ctx = ibv_open_device(dev);
    if (this->ctx == nullptr) {
      return -1;
    }
    ibv_free_device_list(device_list);
    device_list = nullptr;
  }

  /* query port attributes */
  if (ibv_query_port(this->ctx, this->rdma_port, &this->port_attribute) != 0) {
    return -1;
  }

  /* Get a GID table entry */
  ibv_query_gid(this->ctx, this->rdma_port, 0, &this->local_gid);
  
  /* Create a completion channel, so we can listen the QP */
  if (this->channel == nullptr) {
    this->channel = ibv_create_comp_channel(this->ctx);
    if (this->channel == nullptr) {
      return -1;
    }
  }

  if (this->cq == nullptr) {
    // CQ占满的情况是：发送了最大量的消息，接收了最大量的消息 
    this->cq = ibv_create_cq(this->ctx, this->local_slot_num * 2, nullptr, 
            this->channel, 0);
    if (this->cq == nullptr) {
      return -1;
    }
  }
  // 如果不使用忙等，则需要调用ibv_req_notify_cq()
  if (!USE_BUSY_POLLING) {
    if (ibv_req_notify_cq(this->cq, 0) != 0) {
      return -1;
    }
  }

  /* Allocate Protection Domain */
  if (this->pd == nullptr) {
    this->pd = ibv_alloc_pd(this->ctx);
    if (this->pd == nullptr) {
      return -1;
    }
  }
  
  /* Get random qp psn */
  srand48(getpid() * time(nullptr));
  this->qp_psn = lrand48() & 0xffffff;
  
  /* Create MR */
  mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE ;
  this->local_mr = ibv_reg_mr(this->pd, (void *)this->local_memory,
          this->local_memory_size, mr_flags);
  if (this->local_mr == nullptr) {
    return -1;
  }

  return this->createQueuePair();
}

int RdmaQueuePair::createQueuePair() {
  struct ibv_qp_init_attr attr;
  memset(&attr, 0, sizeof(attr));

  attr.qp_type = IBV_QPT_RC;
  attr.sq_sig_all = 0;
  attr.send_cq = this->cq;
  attr.recv_cq = this->cq;
  attr.cap.max_send_wr = this->local_slot_num;
  attr.cap.max_recv_wr = this->local_slot_num * 2;
  attr.cap.max_send_sge = 2;
  attr.cap.max_recv_sge = 1;
  attr.cap.max_inline_data = 0;

  this->qp = ibv_create_qp(this->pd, &attr);
  if (this->qp == nullptr) {
    return -1;
  } else {
    return 0;
  }
}

int RdmaQueuePair::modifyQPtoInit() {
  struct ibv_qp_attr attr;
  int flags;
  int rc;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_INIT;
  attr.port_num = this->rdma_port;
  attr.pkey_index = 0;
  attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
  flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
  rc = ibv_modify_qp(qp, &attr, flags);

  if (rc != 0) {
    return -1;
  }
  return 0;
}

int RdmaQueuePair::modifyQPtoRTR() {
  struct ibv_qp_attr attr;
  int flags;
  int rc;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = IBV_MTU_4096;
  attr.dest_qp_num = this->remote_qp_num;
  attr.rq_psn = this->remote_qp_psn;

  attr.ah_attr.is_global = 0;
  attr.ah_attr.dlid = this->remote_lid;
  attr.ah_attr.sl = 0;
  attr.ah_attr.src_path_bits = 0;
  attr.ah_attr.port_num = this->rdma_port;

  flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN;

  // attr.max_dest_rd_atomic = 16;
  attr.max_dest_rd_atomic = 1;
  attr.min_rnr_timer = 12;
  flags |= IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
  rc = ibv_modify_qp(qp, &attr, flags);
  if (rc != 0)
  {
    return -1;
  }

  return 0;
}

int RdmaQueuePair::modifyQPtoRTS() {
  struct ibv_qp_attr attr;
  int flags;
  int rc;
  memset(&attr, 0, sizeof(attr));

  attr.qp_state = IBV_QPS_RTS;
  attr.sq_psn = this->qp_psn;
  flags = IBV_QP_STATE | IBV_QP_SQ_PSN;

  attr.timeout = 14;
  attr.retry_cnt = 7;
  attr.rnr_retry = 7;
  // attr.max_rd_atomic = 16;
  attr.max_rd_atomic = 1;

  flags |= IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC;

  rc = ibv_modify_qp(qp, &attr, flags);
  if (rc != 0) {
    return -1;
  }
  return 0;
}

RdmaQueuePair::RdmaQueuePair(int local_slot_num, int local_slot_size, 
            void *_shared_memory, const char *device_name, uint32_t rdma_port)
            : local_slot_num(local_slot_num), local_slot_size(local_slot_size), 
              device_name(device_name), rdma_port(rdma_port)
{
  // Initialize local memory for the QP to register in MR
  this->local_memory_size = this->local_slot_size * (this->local_slot_num + 1);
  this->local_memory = _shared_memory;
  
  int rc = this->initializeLocalRdmaResource();
  if (rc != 0) {
    this->Destroy();
    throw std::bad_exception();
  }
}

RdmaQueuePair::~RdmaQueuePair() {
  this->Destroy();
}

void RdmaQueuePair::Destroy() {
  if (this->qp != nullptr) {
    ibv_destroy_qp(this->qp);
    this->qp = nullptr;
  }
  if (this->local_mr != nullptr) {
    ibv_dereg_mr(this->local_mr);
    this->local_mr = nullptr;
  }
  if (this->pd != nullptr) {
    ibv_dealloc_pd(this->pd);
    this->pd = nullptr;
  }
  if (this->cq != nullptr) {
    ibv_destroy_cq(this->cq);
    this->cq = nullptr;
  }
  if (this->channel != nullptr) {
    ibv_destroy_comp_channel(this->channel);
    this->channel = nullptr;
  }
  if (this->ctx != nullptr) {
    ibv_close_device(this->ctx);
    this->ctx = nullptr;
  }
  this->local_memory = nullptr;
}

void* RdmaQueuePair::GetLocalMemory() {
  return this->local_memory;
}

size_t RdmaQueuePair::GetLocalMemorySize() {
  return this->local_memory_size;
}

ibv_comp_channel* RdmaQueuePair::GetChannel() {
  return this->channel;
}

void RdmaQueuePair::SetSendContent(void *send_content, int size, int slot_idx) {
  if (size <= 0) {
    return;
  }
  void *buf = (void *)((char *)this->local_memory + slot_idx * this->local_slot_size);
  memcpy(buf, send_content, size);
}

int RdmaQueuePair::PostSend(uint32_t imm_data, int slot_idx, int length) {
  uintptr_t send_addr = (uintptr_t)((char *)this->local_memory + slot_idx * this->local_slot_size);
  uintptr_t recv_addr = this->remote_memory + slot_idx * this->local_slot_size;
  int rc = 0;

  struct ibv_sge sg;
  struct ibv_send_wr wr;
  struct ibv_send_wr *bad_wr;

  memset(&sg, 0, sizeof(sg));
  sg.addr = send_addr;
  // sg.length = this->local_slot_size;
  sg.length = length;
  sg.lkey = this->local_mr->lkey;

  memset(&wr, 0, sizeof(wr));
  wr.wr_id = slot_idx;
  wr.sg_list = &sg;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
  wr.wr.rdma.remote_addr = recv_addr;
  wr.wr.rdma.rkey = this->remote_mr_key;

  wr.send_flags = IBV_SEND_SIGNALED;
  wr.imm_data = imm_data;
  
  while ((rc = ibv_post_send(this->qp, &wr, &bad_wr)) != 0 && errno == EAGAIN);
  if (rc == 0) {
    return 0;
  } else {
    return rc;
  }
}

int RdmaQueuePair::PostReceive() {
  struct ibv_sge      sg;
  struct ibv_recv_wr  wr;
  struct ibv_recv_wr *bad_wr;

  memset(&sg, 0, sizeof(sg));
  sg.addr   = (uintptr_t)this->local_memory;
  sg.length = 0;   // because of IBV_WR_RDMA_WRITE_WITH_IMM,
                   // we don't need to worry about the recv buffer
  sg.lkey   = this->local_mr->lkey;

  memset(&wr, 0, sizeof(wr));
  wr.wr_id   = 0;
  wr.sg_list = &sg;
  wr.num_sge = 1;
  if (ibv_post_recv(this->qp, &wr, &bad_wr) != 0) {
    LOG_DEBUG("RdmaQueuePair::PostReceive() failed, errno is %d", errno);
    return -1;
  } else {
    return 0;
  }
}

/** 
 * @todo: 一次性poll多个wc，提高性能
 */
int RdmaQueuePair::PollCompletionsFromCQ(std::vector<struct ibv_wc> &wcs) {
  int ret = 0;
  struct ibv_cq *ev_cq;
  struct ibv_wc  wc;
  void          *ev_ctx;
  int            wc_num = 0;
  
  if (!USE_BUSY_POLLING) {
    while (true) {
      ret = ibv_get_cq_event(this->channel, &ev_cq, &ev_ctx);
      if (ret != 0) {
        return wc_num;
      }
      ibv_ack_cq_events(ev_cq, 1);
      ret = ibv_req_notify_cq(ev_cq, 0);
      if (ret != 0) {
        return -1;
      }

      while (true) {
        ret = ibv_poll_cq(ev_cq, 1, &wc);
        if (ret < 0) {
          return -1;
        }
        if (ret == 0) {
          break;
        }
        if (ret > 0) {
          wcs.push_back(wc);
          wc_num++;
        }
      }
    }
  } else {
    while (true) {
      ret = ibv_poll_cq(this->cq, 1, &wc);
      if (ret < 0) {
        return -1;
      }
      if (ret == 0) {
        return wc_num;
      }
      if (ret > 0) {
        wcs.push_back(wc);
        wc_num++;
      }
    }
  }
  
  // can not reach here
  return wc_num;
}

int RdmaQueuePair::ReadyToUseQP() {
  if (this->modifyQPtoInit() != 0) {
    LOG_DEBUG("RdmaQueuePair failed to modify qp to init");
    return -1;
  }
  for (int i = 0; i < this->local_slot_num; ++i) {
    if (this->PostReceive() != 0) {
      LOG_DEBUG("RdmaQueuePair failed to post %d receives in qp", i);
      return -1;
    }
  }
  if (this->modifyQPtoRTR() != 0) {
    LOG_DEBUG("RdmaQueuePair failed to modify qp to RTR");
    return -1;
  }
  if (this->modifyQPtoRTS() != 0) {
    LOG_DEBUG("RdmaQueuePair failed to modify qp to RTS");
    return -1;
  }
  return 0;
}

void RdmaQueuePair::GetLocalQPMetaData(QueuePairMetaData &local_data) {
  local_data.registered_memory = (uintptr_t)this->local_memory;
  local_data.registered_key    = this->local_mr->rkey;
  local_data.lid               = this->port_attribute.lid;
  local_data.qp_num            = this->qp->qp_num;
  local_data.qp_psn            = this->qp_psn;
  local_data.gid               = this->local_gid;
}

void RdmaQueuePair::SetRemoteQPMetaData(QueuePairMetaData &remote_data) {
  this->remote_memory = remote_data.registered_memory;
  this->remote_mr_key = remote_data.registered_key;
  this->remote_lid    = remote_data.lid;
  this->remote_qp_num = remote_data.qp_num;
  this->remote_qp_psn = remote_data.qp_psn;
}

/** ----------------------------------------------------------------------------------------------------
 * start impl RdmaClient
 * -----------------------------------------------------------------------------------------------------
 */
RdmaClient::RdmaClient(std::string remote_ip, uint32_t remote_port, MemoryAllocator *allocator,
            int node_num, int slot_size, int slot_num)
            : _remote_ip{remote_ip}, _remote_port{remote_port}, _allocator{allocator},
              _node_num{node_num}, _slot_size{slot_size}, _slot_num{slot_num}
{
  int rc = 0;
  size_t size = 0;

  rc = this->createRdmaQueuePairs();
  if (rc != 0) {
    throw std::bad_exception();
  }
}

RdmaClient::~RdmaClient() {
  this->Destroy();
}

void RdmaClient::Destroy() {
  if (this->_rdma_queue_pairs != nullptr) {
    for (int i = 0; i < this->_node_num; ++i) {
      if (this->_rdma_queue_pairs[i] != nullptr) {
        this->_rdma_queue_pairs[i]->Destroy();
        this->_rdma_queue_pairs[i] = nullptr;
      }
    }
    this->_rdma_queue_pairs = nullptr;
  }
}

int RdmaClient::createRdmaQueuePairs() {
  int rc = 0;
  size_t size = 0;
  
  size = sizeof(RdmaQueuePair *) * _node_num;
  try {
    this->_rdma_queue_pairs = (RdmaQueuePair **)(_allocator->alloc(size, 8));
  } catch (...) {
    throw std::bad_exception();
  }

  if (this->_rdma_queue_pairs == nullptr) {
    return -1;
  }
  for (int i = 0; i < _node_num; ++i) {
    this->_rdma_queue_pairs[i] = nullptr;
  }

  for (int i = 0; i < _node_num; ++i) {
    size = sizeof(RdmaQueuePair);
    try {
      this->_rdma_queue_pairs[i] = (RdmaQueuePair *)(_allocator->alloc(size, 8));
      size = (size_t)_slot_size * (_slot_num + 1);
      void *shared_memory = _allocator->alloc(size, CACHE_LINE_SIZE);
      this->_rdma_queue_pairs[i] = new RdmaQueuePair(_slot_num, _slot_size, shared_memory,
              DEVICE_NAME, RDMA_PORT);

    } catch (...) {
      throw std::bad_exception();
    }

    if (this->_rdma_queue_pairs[i] == nullptr) {
      return -1;
    }

    if (this->_remote_ip == "" || this->_remote_port == 0) {
      continue;
    }

    int sock = this->connectSocket();
    if (sock < 0) {
      return -1;
    }

    QueuePairMeta meta;
    QueuePairMeta remote_meta;
    int           compute_id = MY_COMPUTE_ID;
    int           remote_compute_id;     
    this->_rdma_queue_pairs[i]->GetLocalQPMetaData(meta);
    rc = this->dataSyncWithSocket(sock, compute_id, meta, remote_compute_id, 
            remote_meta);
    (void) close(sock);
    sock = 0;
    if (rc != 0) {
      LOG_DEBUG("RdmaClient failed to dataSyncWith RdmaServer");
      return -1;
    }
    this->_rdma_queue_pairs[i]->SetRemoteQPMetaData(remote_meta);
    
    // 准备进行Send/Receive操作
    rc = this->_rdma_queue_pairs[i]->ReadyToUseQP();
    if (rc != 0) {
      LOG_DEBUG("RdmaClient failed to make %d qp ready to send", i);
      return -1;
    }
  }

  return 0;
}

int RdmaClient::connectSocket() {
  int sock;
  struct sockaddr_in remote_address;
  struct timeval timeout = {3, 0};

  memset(&remote_address, 0, sizeof(remote_address));
  remote_address.sin_family = AF_INET;
  inet_aton(this->_remote_ip.c_str(), (struct in_addr*)&remote_address.sin_addr);
  remote_address.sin_port = htons(_remote_port);

  if ((sock = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
    return -1;
  }

  while (connect(sock, (struct sockaddr*)&remote_address, sizeof(struct sockaddr)) != 0) {
    usleep(1000);
  }

  return sock;
}

int RdmaClient::dataSyncWithSocket(int sock, int compute_id, const QueuePairMeta& meta,
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

  LOG_DEBUG("RdmaClient, compute id of %u, Start to dataSyncWithSocket, remote socket is %d, "
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

  LOG_DEBUG("RdmaClient, compute id of %u, received sync data, remote_compute_id=%u, "
          "remote_registered_memory=%lu, remote_registered_key=%u, remote_qp_num=%u, "
          "remote_qp_psn=%u, remote_lid=%hu", compute_id, remote_compute_id, remote_meta.registered_memory,
          remote_meta.registered_key, remote_meta.qp_num, remote_meta.qp_psn, remote_meta.lid);
  return 0;

}

/** 
 * -------------------------------------------------------------------------------------
 * SharedRdmaClient
 * --------------------------------------------------------------------------------------
 */

void SharedRdmaClient::sendThreadFun(int node_idx) {
  WaitSet *waitset = nullptr;
  int      rc      = 0;
  ZSend   *send    = &(_sends[node_idx].zsend);
  ZAwake  *awake   = &_awakes[node_idx];
  char     tmp_buf[1024];
  uint64_t send_cnt = 0;  // 成功发送消息的个数

  SCOPEEXIT([&]() {
    _stop = true;
    if (waitset != nullptr) {
      delete waitset;
      waitset = nullptr;
    }
    LOG_DEBUG("SharedRdmaClient sendThreadFun, send thread of %u, will retire", node_idx);
  });

  try {
    waitset = new WaitSet();
  } catch (...) {
    return;
  }
  
  RdmaQueuePair *qp = _rdma_queue_pairs[node_idx];
  rc = waitset->addFd(qp->GetChannel()->fd);
  if (rc != 0) {
    return;
  }
  rc = waitset->addFd(_listen_fd[node_idx * 2 + 1]);
  if (rc != 0) {
    return;
  }

  while (!_stop) {
    epoll_event event;
    if (!USE_BUSY_POLLING) {
      rc = waitset->waitSetWait(&event);
      if (rc < 0 && errno != EINTR) {
        LOG_DEBUG("SharedRdmaClient sendThreadFun, send thread of %u, failed to waitsetwait", node_idx);
        return;
      }
      if (rc <= 0) {
        continue;
      }
    }
    
    // 返回false表示需要退出整个循环
    auto process_wc = [&]() -> bool {
      std::vector<struct ibv_wc> wcs;
      rc = qp->PollCompletionsFromCQ(wcs);
      if (rc < 0) {
        LOG_DEBUG("SharedRdmaClient sendThreadFun, send thread of %u, failed to poll wcs", node_idx);
        return false;
      }

      for (int i = 0; i < rc; ++i) {
        struct ibv_wc &wc = wcs[i];
        if (wc.status != IBV_WC_SUCCESS) {
          LOG_DEBUG("SharedRdmaClient sendThreadFun, send thread of %u, get a wrong wc "
                  ", wc.status is %d", node_idx, wc.status);
          return false;
        }
        if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
          // 接收到回复
          int slot_idx = wc.imm_data;
          if (qp->PostReceive() != 0) {
            LOG_DEBUG("SharedRdmaClient sendThreadFun, send thread of %d, failed to post receive "
                    "after receiving a recv wc", node_idx);
            return false;
          }
          
          bool nowait;
          nowait = send->nowait[slot_idx];
          // slot_idx号的slot可以被标记为空闲了，除非slot_idx号的slot对应的nowait == false
          if (nowait == true) {
            for (int k = 0; k < send->segment_nums[slot_idx]; ++k) {
              int p = (slot_idx + k) % (_slot_num + 1);
              send->states[p] = SlotState::SLOT_IDLE;
            }
          }

          if (nowait == false) {
            if (!USE_BACKEND_BUSY_POLLING) {
              (void) sem_post(&(awake->sems[slot_idx]));
            } else {
              awake->done[slot_idx] = true;
            }
          }

        } 
      }

      /** 推进zsend->front到notsent_front，并更新free_slot_num */
      int add_free_num = 0;
      while (send->states[send->front] == SLOT_IDLE) {
        send->front = (send->front + 1) % (_slot_num + 1);
        add_free_num++;
      }
      int old_free_slot_num = send->free_slot_num.load();
      int new_free_slot_num;
      do {
        new_free_slot_num = old_free_slot_num + add_free_num;
      } while (!send->free_slot_num.compare_exchange_weak(old_free_slot_num, new_free_slot_num, std::memory_order_seq_cst));

      return true;
    };

    auto process_send_request = [&]() -> bool {
      // 需要发送slot中的数据
      {
        // 先清空pipe中的数据
        while (true) {
          // 不要忘了先把this->listend_fd设置为非阻塞
          int r = recv(_listen_fd[node_idx * 2 + 1], tmp_buf, 1024, 0);
          if (r > 0) {
            continue;
          } else if (r == 0 || (errno != EWOULDBLOCK && errno != EAGAIN)) {
            LOG_DEBUG("SharedRdmaClient sendThreadFun, send thread of %u, failed to recv from listen fd", 
                    node_idx);
            return false;
          } else {
            break;
          }
        }
      }
      send->has_notification.store(0);
      
      /** 
       * 等待rear到notwrite_rear之间的所有数据都填充好
       */
      int old_notwrite_rear = send->notwrite_rear.load();
      while (send->rear != old_notwrite_rear) {
        while (send->states[send->rear] != SLOT_INPROGRESS);
        send->rear = (send->rear + 1) % (_slot_num + 1);
      }

      if (!USE_GROUP_POST_SEND) {
        int slot_idx = send->notsent_front;
        while (slot_idx != send->rear) {
          char *buf = (char *)_rdma_queue_pairs[node_idx]->GetLocalMemory() 
                + slot_idx * _slot_size;
          int size = MessageUtil::parsePacketLength(buf);
          rc = _rdma_queue_pairs[node_idx]->PostSend(slot_idx, slot_idx, size);
          if (rc != 0) {
            LOG_DEBUG("SharedRdmaClient sendThreadFun, send thread of %u, failed to Post send, ret is %d, errno is %d", node_idx, rc, errno);
            return false;
          }
          slot_idx = (slot_idx + 1) % (_slot_num + 1);
        }
        send->notsent_front = send->rear;
      } else {
        /* 实现组发送机制 */
        int slot_idx = send->notsent_front;
        while (slot_idx != send->rear) {
          uint32_t msg_num = 1;
          // 组合发送的消息不能超过GROUP_POST_SEND_MAX_MSG_NUM
          if (send->rear > slot_idx) {
            msg_num = std::min((int)GROUP_POST_SEND_MAX_MSG_NUM, (int)(send->rear - slot_idx));
          } else {
            msg_num = std::min((int)GROUP_POST_SEND_MAX_MSG_NUM, (int)(_slot_num + 1 - slot_idx));
          }

          char *buf = (char *)_rdma_queue_pairs[node_idx]->GetLocalMemory() 
                  + slot_idx * _slot_size;
          // size是所有消息个数乘以slot_size的结果。
          int size = _slot_size * msg_num;
          if (msg_num == 1) {
            size = MessageUtil::parsePacketLength(buf);
          }
          uint32_t imm_data = 0;
          SET_SLOT_IDX_TO_IMM_DATA(imm_data, (uint32_t)slot_idx);
          SET_MSG_NUM_TO_IMM_DATA(imm_data, msg_num);
          rc = _rdma_queue_pairs[node_idx]->PostSend(imm_data, slot_idx, size);

          if (rc != 0) {
            LOG_DEBUG("SharedRdmaClient sendThreadFun, send thread of %u, failed to Post send, ret is %d, errno is %d", node_idx, rc, errno);
            return false;
          }
          slot_idx = (slot_idx + msg_num) % (_slot_num + 1);
        }
        send->notsent_front = send->rear;
      }

      return true;
    };
    
    if (!USE_BUSY_POLLING) {
      if (event.data.fd == qp->GetChannel()->fd) {
        if (!process_wc()) {
          return;
        }
      } else if (event.data.fd == _listen_fd[node_idx * 2 + 1]) {
        if (!process_send_request()) {
          return;
        }
      } else {
        /* can not reach here */
        return;
      }
    } else {
      if (!process_wc()) {
        return;
      }
      if (!process_send_request()) {
        return;
      }
    }
  }
}

void* SharedRdmaClient::sendThreadFunEntry(void *arg) {
  Args *args = (Args *)arg;
  args->client->sendThreadFun(args->node_idx);
  return args;
}

SharedRdmaClient::SharedRdmaClient(int slot_size, int slot_num, std::string remote_ip, uint32_t remote_port, 
          int node_num, void* shared_memory, int *listend_fd)
            : RdmaClient()
{
  LOG_DEBUG("SharedRdmaClient Start to construct SharedRdmaClient\n");
  size_t size = 0;

  MemoryAllocator *allocaotor = new MemoryAllocator();
  allocaotor->init((char *)shared_memory, GetSharedObjSize(slot_size, slot_num, node_num));
  RdmaClient(remote_ip, remote_port, allocaotor, node_num, slot_size, slot_num);

  this->_listen_fd = _listen_fd;
  _start_idx.store(0);
  
  size = sizeof(ZSendPad) * node_num;
  _sends = (ZSendPad *)(_allocator->alloc(size, CACHE_LINE_SIZE));
  for (int i = 0; i < _node_num; ++i) {
    try {
      new (&(_sends[i].zsend))ZSend(_slot_num, _allocator);
    } catch (...) {
      throw std::bad_exception();
    }
  }

  size = sizeof(ZAwake) * node_num;
  _awakes = (ZAwake *)(_allocator->alloc(size, CACHE_LINE_SIZE));
  for (int i = 0; i < _node_num; ++i) {
    try {
      new (&_awakes[i])ZAwake(_slot_num, _allocator);
    } catch (...) {
      throw std::bad_exception();
    }
  }

  LOG_DEBUG("SharedRdmaClient End to construct the SharedRdmaClient\n");
}

SharedRdmaClient::~SharedRdmaClient() {
  this->Destroy();
}

int SharedRdmaClient::Run() {
  _send_threads = new pthread_t[_node_num];
  int i = 0;
  SCOPEEXIT([&]() {
    if (i < _node_num) {
      _stop = true;
      for (int j = 0; j < i; ++j) {
        (void) pthread_join(_send_threads[j], nullptr);
      }
      delete[] _send_threads;
      _send_threads = nullptr;
    }
  });

  for (; i < _node_num; ++i) {
    Args *args = new Args();
    args->client = this;
    args->node_idx = i;
    if (pthread_create(_send_threads + i, nullptr, this->sendThreadFunEntry, args) != 0) {
      return -1;
    }
  }
  return 0;
}

void SharedRdmaClient::Stop() {
  _stop = true;
  if (_send_threads == nullptr) {
    return;
  }
  for (int i = 0; i < _node_num; ++i) {
    (void) pthread_join(_send_threads[i], NULL);
  }
  delete[] _send_threads;
  _send_threads = nullptr;
}

void SharedRdmaClient::Destroy() {  
  LOG_DEBUG("Start to destroy SharedRdmaClient\n");

  this->Stop();

  RdmaClient::Destroy();

  _sends = nullptr;
  if (_awakes != nullptr) {
    for (int i = 0; i < _node_num; ++i) {
      if (_awakes[i].sems != nullptr) {
        for (int j = 0; j < _slot_num + 1; ++j)
          (void) sem_destroy(&(_awakes[i].sems[j]));
        _awakes[i].sems = nullptr;
      }
    }
    _awakes = nullptr;
  }

  LOG_DEBUG("End to destroy SharedRdmaClient");
}

int SharedRdmaClient::rrLoadBalanceStrategy(void *send_content, int size, bool nowait, 
            int *out_node_idx, int *out_rear)
{
  char c;
  int  rc = 0;

  int start = 0;
  /** 采用round robin法来实现负载均衡 */
  int old_start_idx = _start_idx.load();
  int new_start_idx;
  do {
    new_start_idx = (old_start_idx + 1) % _node_num;
  } while (!_start_idx.compare_exchange_weak(old_start_idx, new_start_idx, std::memory_order_seq_cst));
  start = old_start_idx;

  while (true) {
    for (int j = 0; j < _node_num; ++j) {
      int i = (start + j) % _node_num;  // 考虑i号node是否可以用于发送
      ZSend  *zsend = &(_sends[i].zsend);
      ZAwake *zawake = &_awakes[i];
      
      int start_rear;
      int rear2;
      if (this->checkNodeCanSend(i, send_content, size, &start_rear, &rear2) == false) {
        continue;
      }

      zsend->segment_nums[start_rear] = (rear2 >= start_rear ? rear2 - start_rear : (_slot_num + 1 - start_rear + rear2));
      for (int k = start_rear; k != rear2; k = (k + 1) % (_slot_num + 1)) {
        zsend->nowait[k] = nowait;
      }
      for (int k = start_rear; k != rear2; k = (k + 1) % (_slot_num + 1)) {
        zsend->states[k].store(SLOT_INPROGRESS);
      }
      
      std::atomic_thread_fence(std::memory_order_seq_cst);
      zsend->has_notification.store(1);
      if (!USE_BUSY_POLLING) {
        rc = send(_listen_fd[i * 2], &c, 1, 0); 
        if (rc <= 0) {
          return -1;
        }
      }

      if (out_node_idx) {
        *out_node_idx = i;
      }
      if (out_rear) {
        *out_rear = start_rear;
      }
      return 0;
    }
  }
}

int SharedRdmaClient::getNeededSegmentNum(int size) {
  int num = 0;
  int left_size = size;
  do {
    num++;
    if (left_size + sizeof(SlotMeta) <= _slot_size) {
      left_size = 0;
      break;
    } else {
      left_size -= (_slot_size - sizeof(SlotMeta));
    }
  } while (true);
  return num;
}

bool SharedRdmaClient::checkNodeCanSend(int node_idx, void *send_content, int size, 
            int *start_rear, int *rear2) {
  ZSend  *zsend = &(_sends[node_idx].zsend);
  ZAwake *zawake = &_awakes[node_idx];
  int segment_num = this->getNeededSegmentNum(size);
  int free_seg = 0;
  int ret = -1;
  
  /* 看可用空间是否足够，只有足够了，才能够去填充数据到slot中 */
  int old_free_slot_num = zsend->free_slot_num.load();
  while (old_free_slot_num < segment_num) {
    int new_free_slot_num = old_free_slot_num - segment_num;
    if (zsend->free_slot_num.compare_exchange_weak(old_free_slot_num, new_free_slot_num, std::memory_order_seq_cst)) {
      ret = 0;
      break;
    }
  }
  if (ret != 0) {
    return false;
  }

  // std::atomic_thread_fence(std::memory_order_seq_cst);

  /**
   * 更新zsend->notwrite_rear
   * 这个时候已经有n个进程更新free_slot_num成功了，也就是能够去填充数据到slot中，
   * 这n个进程去竞争修改notwrite_rear，也就是决定了它们谁先发送，谁后发送的问题，
   * 不过最终一定会修改成功。
   */
  int old_notwrite_rear = zsend->notwrite_rear.load();
  int new_notwrite_rear;
  do {
    new_notwrite_rear = (old_notwrite_rear + segment_num) % (_slot_num + 1);
  } while (!zsend->notwrite_rear.compare_exchange_weak(old_notwrite_rear, new_notwrite_rear, std::memory_order_seq_cst));

  if (rear2) {
    *rear2 = new_notwrite_rear;
  }

  /** 将send_content中的数据填充到slot中 */
  int start_slot_idx = old_notwrite_rear;
  if (start_rear) {
    *start_rear = start_slot_idx;
  }
  int left_size = size;
  bool first = true;
  char *content = (char *)send_content;
  while (left_size > 0) {
    char *buf = (char *)_rdma_queue_pairs[node_idx]->GetLocalMemory() 
            + start_slot_idx * _slot_size;
    SlotMeta *meta = (SlotMeta *)buf;
    buf += sizeof(SlotMeta);
    if (left_size + sizeof(SlotMeta) <= _slot_size) {
      memcpy(buf, content, left_size);
      content += left_size;
      meta->size = left_size + sizeof(SlotMeta);
      left_size = 0;
    } else {
      memcpy(buf, content, _slot_size - sizeof(SlotMeta));
      left_size -= (_slot_size - sizeof(SlotMeta));
      content += (_slot_size - sizeof(SlotMeta));
      meta->size = _slot_size;
    }
    if (first) {
      if (left_size == 0) {
        meta->slot_segment_type = SlotSegmentType::SLOT_SEGMENT_TYPE_NORMAL;
      } else {
        meta->slot_segment_type = SlotSegmentType::SLOT_SEGMENT_TYPE_FIRST;
      }
      first = false;
    } else {
      if (left_size == 0) {
        meta->slot_segment_type = SlotSegmentType::SLOT_SEGMENT_TYPE_LAST;
      } else {
        meta->slot_segment_type = SlotSegmentType::SLOT_SEGMENT_TYPE_MORE;
      }
    }
    // zsend->states[start_slot_idx].store(SLOT_INPROGRESS);
    start_slot_idx = (start_slot_idx + 1) % (_slot_num + 1);
  }

  return true;
}

void SharedRdmaClient::WaitForResponse(ZSend *zsend, ZAwake *zawake, char *buf, int rear, void **response) {
  this->waitForResponse(zsend, zawake, buf, rear, response, nullptr);
}

int SharedRdmaClient::PostRequest(void *send_content, uint64_t size, void **response) {
  return this->postRequest(send_content, size, response, nullptr, false);
}

auto SharedRdmaClient::AsyncPostRequest(void *send_content, uint64_t size, int* ret) 
    -> decltype(std::bind(&SharedRdmaClient::WaitForResponse, (SharedRdmaClient *)(nullptr), 
    (ZSend *)(nullptr), (ZAwake *)(nullptr), (char *)(nullptr), (uint64_t)(0), std::placeholders::_1))
{
#define ZeroRetValue \
    std::bind(&SharedRdmaClient::WaitForResponse, (SharedRdmaClient *)(nullptr), \
        (ZSend *)(nullptr), (ZAwake *)(nullptr), (char *)(nullptr), (uint64_t)(0), std::placeholders::_1)
  
  uint64_t node_idx;
  uint64_t rear;
  *ret = this->rrLoadBalanceStrategy(send_content, size, false, &node_idx, &rear);
  if (*ret != 0) {
    return ZeroRetValue;
  }
  ZSend  *zsend = &this->sends[node_idx];
  ZAwake *zawake = &this->awakes[node_idx];
  char *buf = (char *)this->rdma_queue_pairs[node_idx]->GetLocalMemory() 
               + rear * this->slot_size;
  return std::bind(&SharedRdmaClient::WaitForResponse, this, zsend, zawake, buf, rear,
           std::placeholders::_1);
}

/* @todo:  AsyncPostRequestNowait()和AsyncPostRequest()有很多相似的代码 */
void SharedRdmaClient::AsyncPostRequestNowait(void *send_content, uint64_t size, int *ret) {
  *ret = this->rrLoadBalanceStrategy(send_content, size, true, nullptr, nullptr);
  return;
}

size_t SharedRdmaClient::GetSharedObjSize(int slot_size, int slot_num, int node_num) 
{
  size_t size = 0;
  size += MemoryAllocator::add_size(size, sizeof(RdmaQueuePair *) * node_num, CACHE_LINE_SIZE);
  for (int i = 0; i < node_num; ++i) {
    size += MemoryAllocator::add_size(size, sizeof(RdmaQueuePair), 8);
    size += MemoryAllocator::add_size(size, (size_t)slot_size * (slot_num + 1), CACHE_LINE_SIZE);
  }
  size += MemoryAllocator::add_size(size, sizeof(ZSendPad) * node_num, CACHE_LINE_SIZE);
  for (int i = 0; i < node_num; ++i) {
    size += MemoryAllocator::add_size(size, ZSend::GetSharedObjSize(slot_num), CACHE_LINE_SIZE);
  }
  size += MemoryAllocator::add_size(size, sizeof(ZAwake) * node_num, CACHE_LINE_SIZE);
  for (int i = 0; i < node_num; ++i) {
    size += MemoryAllocator::add_size(size, ZAwake::GetSharedObjSize(slot_num), CACHE_LINE_SIZE);
  }

  // 多申请一些
  return size * 2;
}




// 1. void*和uintptr_t之间的转换
// 2. 批量发送slot中的数据与批量处理slot中的数据
// 3. 不要用C语言的%d, %u之类的，很容易出bug
// 4. 赋值构造函数