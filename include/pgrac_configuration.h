#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#define DEVICE_NAME ("mlx5_0")
#define RDMA_PORT   (1)

#define MY_COMPUTE_ID (0)
// 总的计算节点个数，每个计算节点的编号是0, 1 ... TOTAL_COMPUTE_NUM-1
#define TOTAL_COMPUTE_NUM (2)

// rdma客户端，服务器是否使用忙等的方式来得知发送消息的到来，以及响应的到来
#define USE_BUSY_POLLING (true)

/** 
 * 是否实现rdma框架层的组发送机制，也就是将一个qp中连续的多个消息一次性发送出去。
 * 如果使用组发送机制，则需要imm data做一些修改：imm data的前24个字节表示slot_idx，后8个字节
 * 表示从slot_idx开始的多少个slot都是已经到来的消息。
 */
#define USE_GROUP_POST_SEND (true)
#define IMM_DATA_SLOT_IDX_MASK (0xFFFFFF00)
#define IMM_DATA_MSG_NUM_MASK (0x000000FF)
#define IMM_DATA_SHIFT (8)
#define GROUP_POST_SEND_MAX_MSG_NUM (15)
#define GET_SLOT_IDX_FROM_IMM_DATA(imm_data) \
    ((imm_data & IMM_DATA_SLOT_IDX_MASK) >> IMM_DATA_SHIFT)
#define SET_SLOT_IDX_TO_IMM_DATA(imm_data, slot_idx) \
    (imm_data = ((slot_idx << IMM_DATA_SHIFT) | imm_data))
#define GET_MSG_NUM_FROM_IMM_DATA(imm_data) \
    (imm_data & IMM_DATA_MSG_NUM_MASK)
#define SET_MSG_NUM_TO_IMM_DATA(imm_data, msg_num) \
    (imm_data = (msg_num | imm_data))

// backend是否使用忙等的方式来得知响应是否到来
#define USE_BACKEND_BUSY_POLLING (false)

#define CACHE_LINE_SIZE (128)

// for test
#define IS_SERVER (1)   // 是否是RdmaServer
#define SERVER_IP ("49.52.27.135")  //RdmaServer的地址
// #define TEST_SHARED_MEMORY
// #define TEST_SIMPLE_SERVER
// #define TEST_SIMPLE_SERVER2
// #define TEST_SHARED_CLIENT
#define TEST_SHARED_CLIENT_METRICS


/** 
 * @todo
 */
// node_num, slot_num, slot_size值
// 每个工作线程的接收队列长度

#endif