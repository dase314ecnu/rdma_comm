#include <sys/socket.h>
#include <assert.h>
#include <semaphore.h>

#include "rdma_communication.h"

RdmaQueuePair::RdmaQueuePair(uint64_t _local_slot_num, uint64_t _local_slot_size, 
            const char *_device_name, uint32_t _rdma_port)
            : local_slot_num(_local_slot_num), local_slot_size(_local_slot_size), 
              device_name(_device_name), rdma_port(_rdma_port)
{
  this->local_memory_size = this->local_slot_size * this->local_slot_num;
  this->local_memory = malloc(this->local_memory_size);
  /** @todo */
}

RdmaQueuePair::RdmaQueuePair(uint64_t _local_slot_num, uint64_t _local_slot_size, 
            void *_shared_memory, const char *_device_name, uint32_t _rdma_port)
            : local_slot_num(_local_slot_num), local_slot_size(_local_slot_size), 
              device_name(_device_name), rdma_port(_rdma_port)
{
  this->local_memory_size = this->local_slot_size * this->local_slot_num;
  this->local_memory = _shared_memory;
  /** @todo */
}

RdmaQueuePair::~RdmaQueuePair() {
  if (this->local_memory != 0) {
    free(this->local_memory);
  }
}

void* RdmaQueuePair::GetLocalMemory() {
  return this->local_memory;
}

uint64_t RdmaQueuePair::GetLocalMemorySize() {
  return this->local_memory_size;
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
