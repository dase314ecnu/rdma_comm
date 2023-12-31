
# 编译
## 一个server一个client
修改CMakeLists.txt，将下述四行代码中的三行注释掉，未注释的那行代码将会产生对应的可执行文件。
```
add_executable(test1 ./src/rdma_communication.cpp ./src/waitset.cpp ./test/myipc.cpp ./test/test_shared_memory.cpp)
add_executable(test2 ./test/test_simple_server.cpp ./src/rdma_communication.cpp ./src/waitset.cpp  ./test/test_worker_threadpool.cpp ./test/myipc.cpp) 
add_executable(test3 ./test/test_simple_server2.cpp ./src/rdma_communication.cpp ./src/waitset.cpp  ./test/test_worker_threadpool.cpp ./test/myipc.cpp) 
add_executable(test4 ./test/test_shared_client.cpp ./test/test_simple_server2.cpp ./src/rdma_communication.cpp ./src/waitset.cpp  ./test/test_worker_threadpool.cpp ./test/myipc.cpp)
```
例如test3测试多线程访问RDMA，test4测试多进程访问RDMA。

对于上述test1到test4，分别需要在include/configuration.h文件中，加入下面四行代码中的一个：
```cpp
#define TEST_SHARED_MEMORY
#define TEST_SIMPLE_SERVER
#define TEST_SIMPLE_SERVER2
#define TEST_SHARED_CLIENT
```
加入其中一个后，不能加入剩余其他的。

对于server端和client端还需要分别修改include/configuration.h文件。对于server端，需要修改
```cpp
#define MY_COMPUTE_ID (0)
// 总的计算节点个数，每个计算节点的编号是0, 1 ... TOTAL_COMPUTE_NUM-1
#define TOTAL_COMPUTE_NUM (2)

// for test
#define IS_SERVER (1)   // 是否是RdmaServer
```
对于client端，需要修改
```cpp
#define MY_COMPUTE_ID (1)
// 总的计算节点个数，每个计算节点的编号是0, 1 ... TOTAL_COMPUTE_NUM-1
#define TOTAL_COMPUTE_NUM (2)

// for test
#define IS_SERVER (0)   // 是否是RdmaServer
#define SERVER_IP ("49.52.27.135")  //RdmaServer的地址
```

至此，源文件已经配置完成。

在代码根目录下新建build文件夹，然后在build文件夹下执行：
```bash
cmake .. -DDEBUG=true
```
在build/下会产生一个用于测试的可执行文件。根据情况，会产生test1到test4这四种测试用的可执行文件。假如产生的是test4，则这样启动程序。
在server端执行：
```bash
./build/test4
```
在client端执行前，先删除共享内存段：
```bash
# 查找自己用户名的共享内存段，第一个字段是shmem id
ipcs -m
# 删除自己用户名的所有的shmem id
ipcs -m [your shmem id]
```
然后执行:
```bash
./build/test4
```

## 是否启用busy polling模式
在pgrac_configuration.h中有一个宏叫做USE_BUSY_POLLING。如果为false，则使用忙等来等待事件，如果是true，则使用IO多路复用机制来等待事件。默认为false。

## 是否启用组发送（batch）机制
在pgrac_configuration.h中有一个宏叫做USE_GROUP_POST_SEND，以及对应的参数GROUP_POST_SEND_MAX_MSG_NUM。组发送机制可以让多个连续的消息通过一个post send发送出去，而不是一个一个发送出去，这样有助于提高吞吐率。GROUP_POST_SEND_MAX_MSG_NUM表示最大允许同时发送的消息个数，这个参数可以设置在1 -- 255之间。

# 原理
![image](assets/rdma1.png)
![image](assets/rdma2.png)
## 请求响应模式
请求响应模式指的是请求者如何获得响应。主要有两种方式，第一种是请求者向服务端发送请求，服务端通过另一条链路（QP）向请求者发送响应数据，然后再通过原链路向请求者响应空数据或简单的控制数据，表明任务完成；第二种是请求者向服务端发送请求，服务端直接通过原链路向请求者响应响应数据。当不确定响应数据的长度是多少时，建议使用第一种请求响应模式，比如csnlog长度可能是几个页的长度，也可能只有几个事务的长度；当确定响应数据的长度是多少时，建议使用第二种请求响应模式，比如snapshot长度是固定的，页转移的大小也是固定的。很显然，第二种方式的延迟会更低，大部分情况下，使用第二种方式。

目前CommonRdmaClient，也就是用于多线程环境下的rdma客户端接口，未完全开发完请求响应模式；SharedRdmaClient支持第一种和第二种请求响应模式，但是需要在响应的相关状态位中指明，当前的响应数据仅仅是控制数据，还是真正的响应数据。

## 组发送机制
```cpp
/** 
 * 是否实现rdma框架层的组发送机制，也就是将一个qp中连续的多个消息一次性发送出去。
 * 如果使用组发送机制，则需要imm data做一些修改：imm data的前24个字节表示slot_idx，后8个字节
 * 表示从slot_idx开始的多少个slot都是已经到来的消息。
 */
#define USE_GROUP_POST_SEND (true)
#define IMM_DATA_SLOT_IDX_MASK (0xFFFFFF00)
#define IMM_DATA_MSG_NUM_MASK (0x000000FF)
#define IMM_DATA_SHIFT (8)
#define GROUP_POST_SEND_MAX_MSG_NUM (200)
#define GET_SLOT_IDX_FROM_IMM_DATA(imm_data) \
    ((imm_data & IMM_DATA_SLOT_IDX_MASK) >> IMM_DATA_SHIFT)
#define SET_SLOT_IDX_TO_IMM_DATA(imm_data, slot_idx) \
    (imm_data = ((slot_idx << IMM_DATA_SHIFT) | imm_data))
#define GET_MSG_NUM_FROM_IMM_DATA(imm_data) \
    (imm_data & IMM_DATA_MSG_NUM_MASK)
#define SET_MSG_NUM_TO_IMM_DATA(imm_data, msg_num) \
    (imm_data = (msg_num | imm_data))

```
组发送机制目前只支持SharedRdmaClient，也就是说多个进程的消息可以一次性发送出去。当SharedRdmaClient的发送线程收到发送事件通知后，即可检查相关共享变量来确定有多少连续的消息要发送，imm data中用于存储组发送的参数，例如slot_idx是第一个要发送的消息的slot号，后面有连续msg_num个消息未发送，则可以在imm data中的前24bit指定slot_idx，后8个字节指定msg_num，然后这个post send的内容就是这个msg_num个slot对应的内存区域。

经测试，组发送可以提高吞吐量。

## 异步发送机制
SharedRdmaClient提供了AsyncPostRequest()接口实现异步发送消息，也即post send后直接返回，不等待结果，但是AsyncPostRequest()会返回一个可调用的binder对象，以供上层在合适的时机等待参数返回。异步发送的例子是：
```cpp
void *response = nullptr;
int rc = 0;
auto wait = rdma_client->AsyncPostRequest((void *)send_buf, length, &rc); 
wait(&response); // 等待响应到来，response用于存储响应数据
```

## 向RDMA框架注册回调函数
由于RDMA框架默认是将注册内存中的消息拷贝到上层，让上层去处理。但是当某些消息非常频繁，且字节数很大的时候，这种拷贝会对性能产生影响，因此可以选择将对消息的处理逻辑传递到RDMA框架，让底层自动对响应执行处理逻辑，而不是把响应拷贝到上层。
可以这样使用回调函数：
```cpp
auto callback = [&](void *response) = {
    // your code
};
// 或者
auto callback = std::bind(yourfunc, your args);
rdma_client->PostRequest((void *)send_buf, length, callback); 
``` 

## nowait机制
这个机制是基于异步发送机制开发的，如果一个异步发送操作不需要使用响应结果，它就无需等待。因此它只需要发送一个异步请求，然后直接不用再管这个请求的结果。这个feature适用于pg的异步提交机制，以提高性能。
```cpp
int ret;
rdma_client->AsyncPostRequestNowait((void *)content, *length, &ret);
```

## 消息分片机制
由于这个框架设计之初是假设每个消息都有固定的长度，但是有些场景下，消息长度并不是固定的。因此就实现了消息分片机制，将一个长消息分成多个slot来发送，接收端负责识别出消息分片，并在inplace地将消息分片组合成一个完整的消息，最后将指针返回给上层。因此，上层可以传递任意长度的消息到底层框架，也可以收到任意长度的消息，并不用关心底层的消息分片机制。

# commit 25c740dacbc6d4605324075f65f57484ab8d2d2f重大更新
## 整体架构图
![image](assets/rdma3.png)
![image](assets/rdma4.png)

## 大锁优化
整个通信框架的运行是基于状态机转换的，任何发送操作，接收操作都会推进状态机，为了保证状态机转换的原子性，一般使用一个mutex锁来进行维护。不过这样做效率不高，原因是状态机的更改通常较为轻量，而且只涉及到几个变量的更改，即使存在内存拷贝操作（如拷贝要发送的数据到注册内存中）也可以使用“并行写”技术来进行优化。最后底层框架实现了完全无锁的状态机转换，只使用原子变量和CAS操作。

# 问题
## fork()引起的rdma资源出错
最近4天我一直在调试一个非常奇怪的错误，他的表现大部分是这样的：SharedRdmaClient在初始化的时候会发出n个post recv，然后在整个运行过程中，它就只能收到n个消息，即使我在合适的时机继续post recv了。数据显示，SharedRdmaClient会发送n+m个消息（m是工作进程数），然后RdmaaServer也会收到n+m个消息进行处理，同时去发出n+m个post send，但是它只有n个post send会成功，SharedRdmaClient也只能收到n个回复，因此m个工作进程会因为得不到回复而一直阻塞等待。
    
问题出在fork()上。我测试的是多进程下并发访问rdma资源的场景，因此有多个rdma发送线程和多个工作进程。但是我在fork多个进程之前初始化了rdma资源，这就导致了在fork是rdma资源也在多个进程之间复制了，这是没有必要甚至错误的，因为工作进程不需要直接访问rdma资源，而是通过共享内存来告诉rdma发送线程来发送数据。当我调整顺序后就解决了这个bug，也就是，在fork多个进程之后，仅仅rdma发送线程才会初始化rdma资源。

## ibv_post_send()返回ENOMEM错误码
由于一个bug，客户端在死循环内发送了大量的组提交消息，服务端于是不断处理大量的消息，不断发送大量的响应，导致消息个数远远超过qp的容量，导致了客户端的ibv_post_send()返回了ENOMEM错误码。

## 共享内存被错误覆盖的问题
见这个commit: https://gitee.com/zhouhuahui/pgrac_shared-cache-service/commit/818a30f1e9b4beead7cabd65cab24e4ed96c50a7。send thread和backend这两个并发的线程都有可能更新状态位，如果send thread在收到响应之后先唤醒backend线程，那么就有可能发生这样的情况：
1. backend进程被唤醒
2. 对zsend->spinlock加锁，更新zsend中的front状态位，states状态位等，解锁
3. backend2发现zsend->front更新，于是在这个发送器上填充数据，并更新zsend的nowait, states状态位
4. send thread发现了不一致的nowait状态，导致它执行了不该执行的代码：再次更新zsend的states状态位。states状态被污染
5. send thread再次循环时，由于zsend的states状态错误，导致某些slot的数据（属于backend2的）应该发送却没有发送
6. 服务器接收到了slot head和slot tail，但是由于中间的某些slot并没有发送过来，因此服务器读到的数据是未赋值的数据，用这个错误数据进行后续操作导致了错误（比如，访问到了错误的length字段，进而访问了错误的内存）

## 传输协议问题
本框架目前没有定义传输协议，而是又上层用户自己定义传输协议，这是功能上的缺陷。

## 字节对齐问题
共享内存中的数据没有做到字节对齐，比较影响性能

## 未实现完整的控制反转
需要上层用户自己调用PostResponse()方法，这是不应该的，因为所有请求都需要返回响应，所以这个PostResponse()方法应该由框架调用，而不是用户，用户只需要设置返回数据。

## 其他问题
1. =和==符号弄错
2. 参数名字写反