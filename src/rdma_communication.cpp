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
#include <sys/socket.h>
#include <stdio.h>

#include "rdma_communication.h"
#include "inner_scope.h"

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
  if (ibv_req_notify_cq(this->cq, 0) != 0) {
    return -1;
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
  mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE 
             | IBV_ACCESS_REMOTE_ATOMIC;
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
  attr.cap.max_recv_wr = this->local_slot_num;
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
  attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
  flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
  rc = ibv_modify_qp(qp, &attr, flags);

  if (rc != 0) {
    return -1;
  }
  return 1;
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

  attr.max_dest_rd_atomic = 16;
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
  attr.sq_psn = this->remote_qp_psn;
  flags = IBV_QP_STATE | IBV_QP_SQ_PSN;

  attr.timeout = 14;
  attr.retry_cnt = 7;
  attr.rnr_retry = 7;
  attr.max_rd_atomic = 16;
  attr.max_dest_rd_atomic = 16;
  flags |= IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC;

  rc = ibv_modify_qp(qp, &attr, flags);
  if (rc != 0) {
    return -1;
  }
  return 0;
}

RdmaQueuePair::RdmaQueuePair(uint64_t _local_slot_num, uint64_t _local_slot_size, 
            const char *_device_name, uint32_t _rdma_port)
            : local_slot_num(_local_slot_num), local_slot_size(_local_slot_size), 
              device_name(_device_name), rdma_port(_rdma_port)
{
  // Initialize local memory for the QP to register in MR
  this->local_memory_size = this->local_slot_size * this->local_slot_num;
  this->local_memory = malloc(this->local_memory_size);
  if (this->local_memory == nullptr) {
    throw std::bad_exception();
  }

  int rc = this->initializeLocalRdmaResource();
  if (rc != 0) {
    this->Destroy();
    throw std::bad_exception();
  }
}

RdmaQueuePair::RdmaQueuePair(uint64_t _local_slot_num, uint64_t _local_slot_size, 
            void *_shared_memory, const char *_device_name, uint32_t _rdma_port)
            : local_slot_num(_local_slot_num), local_slot_size(_local_slot_size), 
              device_name(_device_name), rdma_port(_rdma_port)
{
  // Initialize local memory for the QP to register in MR
  // it is allocated in shared memory
  this->local_memory_size = this->local_slot_size * this->local_slot_num;
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
  }
  if (this->local_mr != nullptr) {
    ibv_dereg_mr(this->local_mr);
  }
  if (this->pd != nullptr) {
    ibv_dealloc_pd(this->pd);
  }
  if (this->cq != nullptr) {
    ibv_destroy_cq(this->cq);
  }
  if (this->channel != nullptr) {
    ibv_destroy_comp_channel(this->channel);
  }
  if (this->ctx != nullptr) {
    ibv_close_device(this->ctx);
  }
  if (this->use_shared_memory == false) {
    free(this->local_memory);
  }
}

void* RdmaQueuePair::GetLocalMemory() {
  return this->local_memory;
}

uint64_t RdmaQueuePair::GetLocalMemorySize() {
  return this->local_memory_size;
}

void RdmaQueuePair::SetSendContent(void *send_content, uint64_t size, uint64_t slot_idx) {
  void *buf = this->local_memory + slot_idx * this->local_slot_size;
  memcpy(buf, send_content, size);
}

int RdmaQueuePair::PostSend(uint32_t imm_data, uint64_t slot_idx) {
  uintptr_t send_addr = (uintptr_t)(this->local_memory + slot_idx * this->local_slot_size);
  uintptr_t recv_addr = this->remote_memory + slot_idx * this->local_slot_size;

  struct ibv_sge sg;
  struct ibv_send_wr wr;
  struct ibv_send_wr *bad_wr;

  memset(&sg, 0, sizeof(sg));
  sg.addr = send_addr;
  sg.length = this->local_slot_size;
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

  if (ibv_post_send(this->qp, &wr, &bad_wr) != 0) {
    return -1;
  } else {
    return 0;
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
    return -1;
  } else {
    return 0;
  }
}

int RdmaQueuePair::PollCompletionFromCQ(struct ibv_wc *wc) {
  int ret = 0;
  ret = ibv_poll_cq(this->cq, 1, wc);
  if (ret >= 0) {
    return ret;
  } else {
    return -1;
  }
}

int RdmaQueuePair::ReadyToUseQP() {
  if (this->modifyQPtoInit() != 0) {
    return -1;
  }
  for (int i = 0; i < this->local_slot_num; ++i) {
    if (this->PostReceive() != 0) {
      return -1;
    }
  }
  if (this->modifyQPtoRTR() != 0) {
    return -1;
  }
  if (this->modifyQPtoRTS() != 0) {
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

int RdmaClient::createRdmaQueuePairs(void *shared_memory, void **end_memory) {
  int rc = 0;
  char *scratch = (char *)shared_memory;

  SCOPEEXIT([&]() {
    if (end_memory != nullptr) {
      *end_memory = (void *)scratch;
    }
  });
  
  if (shared_memory != nullptr) {
    this->rdma_queue_pairs = (RdmaQueuePair **)scratch;
    scratch += sizeof(RdmaQueuePair *) * this->node_num;
  } else {
    this->rdma_queue_pairs = new RdmaQueuePair*[this->node_num];
  }
  if (this->rdma_queue_pairs == nullptr) {
    return -1;
  }
  for (int i = 0; i < this->node_num; ++i) {
    this->rdma_queue_pairs[i] = nullptr;
  }

  for (int i = 0; i < this->node_num; ++i) {
    if (shared_memory == nullptr) {
      this->rdma_queue_pairs[i] = new RdmaQueuePair(this->slot_num, this->slot_size);
      if (this->rdma_queue_pairs[i] == nullptr) {
        return -1;
      }
    } else {
      this->rdma_queue_pairs[i] = (RdmaQueuePair *)scratch;
      *(this->rdma_queue_pairs[i]) = RdmaQueuePair(this->slot_num, 
              this->slot_size, (void *)(scratch + sizeof(RdmaQueuePair)), 
              DEVICE_NAME, RDMA_PORT);
      scratch += (sizeof(RdmaQueuePair) + 
              this->rdma_queue_pairs[i]->GetLocalMemorySize());
    }
    if (this->rdma_queue_pairs[i] == nullptr) {
      return -1;
    }

    int sock = this->connectSocket();
    if (sock < 0) {
      return -1;
    }

    QueuePairMeta meta;
    QueuePairMeta remote_meta;
    uint32_t      compute_id = MY_COMPUTE_ID;
    uint32_t      remote_compute_id;     
    this->rdma_queue_pairs[i]->GetLocalQPMetaData(meta);
    rc = this->dataSyncWithSocket(sock, compute_id, meta, remote_compute_id, 
            remote_meta);
    if (rc != 0) {
      return -1;
    }
    this->rdma_queue_pairs[i]->SetRemoteQPMetaData(remote_meta);
    
    // 准备进行Send/Receive操作
    rc = this->rdma_queue_pairs[i]->ReadyToUseQP();
    if (rc != 0) {
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
  inet_aton(this->remote_ip.c_str(), (struct in_addr*)&remote_address.sin_addr);
  remote_address.sin_port = htons(remote_port);

  if ((sock = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
    return -1;
  }

  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
  while (connect(sock, (struct sockaddr*)&remote_address, sizeof(struct sockaddr)) != 0) {
    usleep(1000);
  }

  return sock;
}

int RdmaClient::dataSyncWithSocket(int sock, uint32_t compute_id, const QueuePairMeta& meta,
            uint32_t &remote_compute_id, QueuePairMeta &remote_meta)
{
  size_t length = sizeof(uint32_t) + sizeof(QueuePairMeta);
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
      delete send_buf;
    }
    if (recv_buf != nullptr) {
      delete recv_buf;
    }
  });

  sprintf(pointer, "%08x", htobe32(compute_id));
  pointer += sizeof(uint32_t);
  sprintf(pointer, "%016lx", htobe64(meta.registered_memory));
  pointer += sizeof(uintptr_t);
  sprintf(pointer, "%08x", htobe32(meta.registered_key));
  pointer += sizeof(uint32_t);
  sprintf(pointer, "%08x", htobe32(meta.qp_num));
  pointer += sizeof(uint32_t);
  sprintf(pointer, "%08x", htobe32(meta.qp_psn));
  pointer += sizeof(uint32_t);
  sprintf(pointer, "%04x", htobe16(meta.lid));
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

  sscanf(recv_buf, "%08x%016lx%08x%08x%08x%04x", &compute_id, 
          &remote_meta.registered_memory, &remote_meta.registered_key,
          &remote_meta.qp_num, &remote_meta.qp_psn, &remote_meta.lid);

  return 0;
}


RdmaClient::RdmaClient(uint64_t _slot_size, uint64_t _slot_num, std::string _remote_ip, 
            uint32_t _remote_port, uint32_t _node_num) 
            : remote_ip(_remote_ip), remote_port(_remote_port), node_num(_node_num), 
              slot_size(_slot_size), slot_num(_slot_num)
{
  int rc = 0;

  rc = this->createRdmaQueuePairs(nullptr);
  if (rc != 0) {
    throw std::bad_exception();
  }

  this->sends = new ZSend[this->node_num];
  if (this->sends = nullptr) {
    throw std::bad_exception();
  }
  for (int i = 0; i < this->node_num; ++i) {
    try {
      this->sends[i] = ZSend(nullptr, this->slot_num);
    } catch (...) {
      throw std::bad_exception();
    }
  }

  this->awakes = new ZAwake[this->node_num];
  if (this->awakes == nullptr) {
    throw std::bad_exception();
  }
  for (int i = 0; i < this->node_num; ++i) {
    try {
      this->awakes[i] = ZAwake(nullptr, this->slot_num);
    } catch (...) {
      throw std::bad_exception();
    }
  }
}

RdmaClient::RdmaClient(uint64_t _slot_size, uint64_t _slot_num, std::string _remote_ip, uint32_t _remote_port, 
            uint32_t _node_num, void *_shared_memory)
            : remote_ip(_remote_ip), remote_port(_remote_port), node_num(_node_num)
{
  int rc = 0;
  char *scratch = (char *)_shared_memory;
  void *end_memory;

  rc = this->createRdmaQueuePairs(scratch, &end_memory);
  if (rc != 0) {
    throw std::bad_exception();
  }
  scratch = (char *)end_memory;

  this->sends = (ZSend *)scratch;
  scratch += sizeof(ZSend) * this->node_num;
  for (int i = 0; i < this->node_num; ++i) {
    try {
      this->sends[i] = ZSend(scratch, this->slot_num);
      scratch += sizeof(SlotState) * (this->slot_num + 1);
    } catch (...) {
      throw std::bad_exception();
    }
  }

  this->awakes = (ZAwake *)scratch;
  scratch += sizeof(ZAwake) * this->node_num;
  for (int i = 0; i < this->node_num; ++i) {
    try {
      this->awakes[i] = ZAwake(scratch, this->slot_num);
      scratch += sizeof(sem_t) * (this->slot_num + 1);
    } catch (...) {
      throw std::bad_exception();
    }
  }
}

RdmaClient::~RdmaClient() {
  if (this->rdma_queue_pairs != nullptr) {
    for (int i = 0; i < this->node_num; ++i) {
      if (this->rdma_queue_pairs[i] != nullptr) {
        if (this->use_shared_memory == false) {
          delete this->rdma_queue_pairs[i];
        } else {
          this->rdma_queue_pairs[i]->Destroy();
        }
      }
    }
    if (this->use_shared_memory == false) {
      delete[] this->rdma_queue_pairs;
    }
  }
  if (this->sends != nullptr && this->use_shared_memory == false) {
    for (int i = 0; i < this->node_num; ++i) {
      if (this->sends[i].states != nullptr) {
        delete[] this->sends[i].states;
      }
    }
  }
  if (this->awakes != nullptr && this->use_shared_memory == false) {
    for (int i = 0; i < this->node_num; ++i) {
      if (this->awakes[i].sems != nullptr) {
        for (int j = 0; j < this->slot_num + 1; ++i) {
          sem_destroy(&(this->awakes[i].sems[j]));
        }
        delete[] this->awakes[i].sems;
      }
    }
  }
}

SharedRdmaClient::SharedRdmaClient(uint64_t _slot_size, uint64_t _slot_num, 
            std::string _remote_ip, uint32_t _remote_port, 
            uint32_t _node_num, void* _shared_memory)
            : RdmaClient(_slot_size, _slot_num, _remote_ip, _remote_port, _node_num, 
                        (char*)_shared_memory + sizeof(SharedRdmaClient))
{
  this->listen_fd = new int*[_node_num];
  for (int i = 0; i < _node_num; ++i) {
    this->listen_fd[i] = new int[2];
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, this->listen_fd[i]);
    assert(ret != -1);
  }

  /** @todo */
}


// 1. void*和uintptr_t之间的转换
