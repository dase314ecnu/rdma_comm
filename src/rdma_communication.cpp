#include <sys/socket.h>
#include <assert.h>
#include <semaphore.h>
#include <infiniband/verbs.h>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

#include "rdma_communication.h"

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

void RdmaQueuePair::destroy() {
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

  int rc = this->initializeLocalRdmaResource();
  if (rc != 0) {
    this->destroy();
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
    this->destroy();
  }
}

RdmaQueuePair::~RdmaQueuePair() {
  if (this->local_memory != 0) {
    free(this->local_memory);
  }
}

RdmaQueuePair::~RdmaQueuePair() {
  this->destroy();
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



RdmaClient::RdmaClient(uint64_t _slot_size, uint64_t _slot_num, std::string _remote_ip, 
            uint32_t _remote_port, uint32_t _node_num) 
            : remote_ip(_remote_ip), remote_port(_remote_port), node_num(_node_num)
{
  this->rdma_queue_pairs = new RdmaQueuePair*[this->node_num];
  for (int i = 0; i < this->node_num; ++i) {
    this->rdma_queue_pairs[i] = new RdmaQueuePair(_slot_num, _slot_size);
  }
  /** @todo */
}

RdmaClient::RdmaClient(uint64_t _slot_size, uint64_t _slot_num, std::string _remote_ip, uint32_t _remote_port, 
            uint32_t _node_num, void *_shared_memory)
            : remote_ip(_remote_ip), remote_port(_remote_port), node_num(_node_num)
{
  /** 
   * _shared_memory的布局是：
   * SharedRdmaClient node_num_RdmaQueuePair_LocalMemory node_num_ZAwake N_sem_t
   */
  this->rdma_queue_pairs = new RdmaQueuePair*[this->node_num];
  char *scratch = (char *)_shared_memory;
  for (int i = 0; i < this->node_num; ++i) {
    this->rdma_queue_pairs[i] = new RdmaQueuePair(_slot_num, _slot_size, _shared_memory, "mlx5_0", 1);
    scratch += this->rdma_queue_pairs[i]->GetLocalMemorySize();
  }

  this->awakes = (ZAwake *)scratch;
  scratch += sizeof(ZAwake) * this->node_num;
  for (int i = 0; i < this->node_num; ++i) {
    this->awakes[i].sems = (sem_t *)scratch;
    scratch += sizeof(sem_t) * (_slot_num + 1);
    for (int j = 0; j < _slot_num + 1; ++j) {
      sem_init(&(this->awakes[i].sems[j]), 1, 0);
    }
  }
  /** @todo */
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
