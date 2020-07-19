# mkdyd-architecture
本项目是一个支持linux的轻量级通讯服务器。其基于C++11，采用多进程（1个master进程+1个worker进程）+多线程（Thread Pool）的架构方式，epoll(LT)的单reactor多线程模型的多路复用技术，实现了数据包的收发功能。核心技术如下：

```
在worker进程中创建线程池进行业务逻辑的处理。

·采用epoll多路复用技术监听事件提高并发能力。

·约定包头+包体的数据包格式解决粘包问题。
```

### 系统主要架构 
[!avatar](/img/mkdyd.png)
