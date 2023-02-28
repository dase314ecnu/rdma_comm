
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


# 问题
## fork()引起的rdma资源出错
最近4天我一直在调试一个非常奇怪的错误，他的表现大部分是这样的：SharedRdmaClient在初始化的时候会发出n个post recv，然后在整个运行过程中，它就只能收到n个消息，即使我在合适的时机继续post recv了。数据显示，SharedRdmaClient会发送n+m个消息（m是工作进程数），然后RdmaaServer也会收到n+m个消息进行处理，同时去发出n+m个post send，但是它只有n个post send会成功，SharedRdmaClient也只能收到n个回复，因此m个工作进程会因为得不到回复而一直阻塞等待。
    
问题出在fork()上。我测试的是多进程下并发访问rdma资源的场景，因此有多个rdma发送线程和多个工作进程。但是我在fork多个进程之前初始化了rdma资源，这就导致了在fork是rdma资源也在多个进程之间复制了，这是没有必要甚至错误的，因为工作进程不需要直接访问rdma资源，而是通过共享内存来告诉rdma发送线程来发送数据。当我调整顺序后就解决了这个bug，也就是，在fork多个进程之后，仅仅rdma发送线程才会初始化rdma资源。

## 其他问题
1. =和==符号弄错
2. 参数名字写反