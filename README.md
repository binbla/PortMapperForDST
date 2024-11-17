# 这是什么？

Fork From : [TinyPortMapper](https://github.com/wangyu-/tinyPortMapper)

简单来说就是一个端口映射工具，饥荒连接版到现在还不官方支持IPv6直连，但我们有一些特殊的方式去完成这件事情，相当于nat64啦。

关键词: 饥荒联机版、IPv6、直连

工作原理：服务器上面的饥荒联机版只能监听IPv4地址，也就是说，当服务器启动完成后，客户端只能通过IPv4的方式去连接服务器。但是，我们可以做一层小小的转发，我们把送到服务器IPv6地址上10999端口的流量送到IPv4的10999去，IPv6地址上10998端口的流量送到IPv4的10998去，不就可以了。当然，只有服务器做这一层转发是不行的，因为客户端的流量也只能往IPv4上发啊，所以客户端也需要做一层转发，将IPv4的流量送到IPv6上去。
![图片](https://github.com/user-attachments/assets/69f54a96-c28d-46ed-817c-24424d7411a5)

# 需要怎么做？

## 服务端

搭建好服务器，确认有IPv6公网，确认防火墙允许饥荒联机版默认端口10999和10998通行。

然后使用tinyportmapper去做一个端口映射，对于搭服务器的人来说，这点技术操作属于毛毛雨啦


## 客户端

但是，服务器自然有懂Linux的玩家去操心，但你要让**好不容易拉过来的朋友**通过一系列对他来说超级复杂的操作才能跟你联机一起玩。他可能就*下次一定*了。

所以，我在好哥哥wangyu-的开源代码上修改了一下下，有了这个小软件，你的好朋友只需要双击一下就完成了客户端的端口映射。

当然，默认域名是我自己的域名，你要是想修改默认域名自己下载我这里的源码，改main.cpp里面的默认值，然后使用命令`make mingw_cross`（Ubuntu下要安装mingw-w64）重新编译一下就行。

如果重新编译一下对你来说也有点困难，那么你可以让你的朋友再麻烦那么一丢丢，下载这个软件，在软件所在的目录按住shift+鼠标右键，选择“在此处打开PowerShell”或者"Open in Terminal"就能在当前文件夹打开终端，然后输入`./dstPortMapperClient.exe [domain]`，将domain换成你自己的域名就可以啦。

然后，你的好哥们就可以打开游戏，进入大厅按键盘上的`~`键，输入连接命令`c_connect("127.0.0.1")`回车就可以进房间咯。



# 东西在哪儿？

右边下载Releases：dstPortMapperClient.zip,解压就得到了exe文件。
或者点[下载连接](https://github.com/binbla/PortMapperForDST/releases/download/publish/tinymapper.zip)

# 计划

目前只做了傻瓜式客户端。服务端倒是也可以写，但是一个合格的腐竹并不需要这么照顾。

对于腐竹，我也可以分享下小玩意儿，这是很久之前写的,这个程序通过fork的方式去启动/usr/local/bin/tinymapper_amd64
```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#define MAX_IP_LEN 100
int main(int argv, char* args[]) {
  pid_t sub_process_id = 0;
  FILE* fp = NULL;
  char ip[MAX_IP_LEN] = {0};
  char *getIPcmd = "ip a | grep inet6 | grep global | grep -v fd00 | awk '{print $2}' | sed 's#/128##'";
  char remote_ip_port1[120] = {0},remote_ip_port2[120] = {0};
  char* exe_bin = "/usr/local/bin/tinymapper_amd64";
  
  fp = popen(getIPcmd,"r");
  if (fgets(ip, sizeof(ip), fp) == NULL) {
    printf("Wrong with command.\n");
  } else {
    ip[strcspn(ip, "\n")] = '\0';
    printf("获取到的IPv6地址为：%s\n", ip);
  }
  sprintf(remote_ip_port1,"[%s]:10999",ip);
  sprintf(remote_ip_port2,"[%s]:10998",ip);
  sub_process_id = fork();
  if (sub_process_id < 0) {
    // 错误
    printf("Fork Failed!\n");
  } else if (sub_process_id == 0) {
    // 父进程
    char * args[] = {exe_bin,"-l","127.0.0.1:10999","-r",remote_ip_port1,"-u", NULL};
    printf("父进程:cmd1\n");
    execv(exe_bin,args);
  } else {
    // 子进程
    sleep(1);
    char * args[] = {exe_bin,"-l","127.0.0.1:10998","-r",remote_ip_port2,"-u", NULL};
    printf("子进程:cmd2\n");
    execv(exe_bin,args);
  }
  return 0;
}
```
然后再写个systemd unit去启用就可以了
```
#/etc/systemd/system/dstnat64.service
[Unit]
Description=dst nat64
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
ExecStart=/home/bla/crontab/dstnat64

[Install]
WantedBy=default.target
```

