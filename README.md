
# 问题
## fork()引起的rdma资源出错
    最近4天我一直在调试一个非常奇怪的错误，他的表现大部分是这样的：SharedRdmaClient在初始化的时候会发出n个post recv，然后在整个运行过程中，它就只能收到n个消息，即使我在合适的时机继续post recv了。数据显示，SharedRdmaClient会发送n+m个消息（m是工作进程数），然后RdmaaServer也会收到n+m个消息进行处理，同时去发出n+m个post send，但是它只有n个post send会成功，SharedRdmaClient也只能收到n个回复，因此m个工作进程会因为得不到回复而一直阻塞等待。
    
    问题出在fork()上。我测试的是多进程下并发访问rdma资源的场景，因此有多个rdma发送线程和多个工作进程。但是我在fork多个进程之前初始化了rdma资源，这就导致了在fork是rdma资源也在多个进程之间复制了，这是没有必要甚至错误的，因为工作进程不需要直接访问rdma资源，而是通过共享内存来告诉rdma发送线程来发送数据。当我调整顺序后就解决了这个bug，也就是，在fork多个进程之后，仅仅rdma发送线程才会初始化rdma资源。

## 其他问题
1. =和==符号弄错
2. 参数名字写反