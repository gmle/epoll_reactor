/*
 *epoll基于非阻塞I/O事件驱动
 * 反应堆模型 
 */
#include <stdio.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define MAX_EVENTS  1024                                    //监听上限数
#define BUFLEN 4096                                          //从管道中读取的最大字节数
#define SERV_PORT   8080                                    //默认端口

//读管道
void recvdata(int fd, int events, void *arg);
//写管道
void senddata(int fd, int events, void *arg);

/* 描述就绪文件描述符相关信息 */
struct myevent_s {
    int fd;                                                 //要监听的文件描述符
    int events;                                             //对应的监听事件
    void *arg;                                              //泛型参数
    void (*call_back)(int fd, int events, void *arg);       //回调函数
    int status;                                             //是否在监听:1->在红黑树上(监听), 0->不在(不监听)
    char buf[BUFLEN];										//存放读到的内容
    int len;												//读到的长度
    long last_active;                                       //记录每次加入红黑树 g_efd 的时间值
};

int g_efd;                                                  //全局变量, 保存epoll_create返回的文件描述符
struct myevent_s g_events[MAX_EVENTS+1];                    //自定义结构体类型数组. +1-->listen fd(最后一个元素存放监听套接字),方便管理套接字




/* -------------------------------------------*/
/**
* @brief  eventset 
* @function 数组元素的初始化
*
* @ev  数组元素地址
* @fd   要监听的文件描述符
* @call_back 回调函数
* @arg 参数
*
/* -------------------------------------------*/

void eventset(struct myevent_s *ev, int fd, void (*call_back)(int, int, void *), void *arg)
{
    ev->fd = fd; 
    ev->call_back = call_back;
    ev->events = 0;//对应的监听事件
    ev->arg = arg; //自己指向自己
    ev->status = 0;//是否在监听:1->在红黑树上(监听), 0->不在(不监听)
    //memset(ev->buf, 0, sizeof(ev->buf));
    //ev->len = 0;
    ev->last_active = time(NULL);                       //调用eventset函数的时间

    return;
}

/* -------------------------------------------*/
/**
* @brief  eventadd 
* @function 事件的添加或修改
*
* @efd  epoll句柄
* @events   要监听或修改的事件
* @ev 数组元素地址
*
/* -------------------------------------------*/
void eventadd(int efd, int events, struct myevent_s *ev)
{
    struct epoll_event epv = {0, {0}};//需要监听事件的结构体
    int op; //EPOLL_CTL_MOD（修改树中节点属性），EPOLL_CTL_ADD（向树中插入节点）
    epv.data.ptr = ev;     //指向一个 myevent_s结构体变量
    epv.events = ev->events = events;  //监听事件赋值 ，EPOLLIN 或 EPOLLOUT

    if (ev->status == 1) {           //已经在红黑树 g_efd 里
        op = EPOLL_CTL_MOD;            //修改其属性
    } else {                            //不在红黑树里
        op = EPOLL_CTL_ADD;              //将其加入红黑树 g_efd, 并将status置1
        ev->status = 1;
    }

    if (epoll_ctl(efd, op, ev->fd, &epv) < 0)                      
        printf("event add failed [fd=%d], events[%d]\n", ev->fd, events);
    else
        printf("event add OK [fd=%d], op=%d, events[%0X]\n", ev->fd, op, events);

    return ;
}


/* -------------------------------------------*/
/**
* @brief  eventdel 
* @function 将事件冲epoll中摘除
*
* @efd  epoll句柄
* @ev   数组元素指针
*
/* -------------------------------------------*/
void eventdel(int efd, struct myevent_s *ev)
{
    struct epoll_event epv = {0, {0}};

    if (ev->status != 1)                                        //不在红黑树上
        return ;

    epv.data.ptr = ev;
    ev->status = 0;                                             //修改状态
    epoll_ctl(efd, EPOLL_CTL_DEL, ev->fd, &epv);                //从红黑树 efd 上将 ev->fd 摘除

    return ;
}

/* -------------------------------------------*/
/**
* @brief  acceptconn 
* @function 获取客户端套接字,加入eopll树和全局数组中
*
* @lfd  套接字
* @events   当前响应的事件类型
* @arg 指向数组元素的指针
*
/* -------------------------------------------*/
void acceptconn(int lfd, int events, void *arg)
{
    struct sockaddr_in cin;
    socklen_t len = sizeof(cin);
    int cfd, i;
    //获取客户端套接字
    if ((cfd = accept(lfd, (struct sockaddr *)&cin, &len)) == -1) {
        if (errno != EAGAIN && errno != EINTR) {
            /* 暂时不做出错处理 */
        }
        printf("%s: accept, %s\n", __func__, strerror(errno));
        return ;
    }

    do {
    	//查找自定义数组中的空闲位置
        for (i = 0; i < MAX_EVENTS; i++)                                //从全局数组g_events中找一个空闲元素
            if (g_events[i].status == 0)                                //类似于select中找值为-1的元素
                break;                                                  //跳出 for
        //超出数组的最大限制
        if (i == MAX_EVENTS) {
            printf("%s: max connect limit[%d]\n", __func__, MAX_EVENTS);
            break;                                                      //跳出do while(0) 不执行后续代码
        }
       
        int flag = 0;
        if ((flag = fcntl(cfd, F_SETFL, O_NONBLOCK)) < 0) {             //将cfd也设置为非阻塞
            printf("%s: fcntl nonblocking failed, %s\n", __func__, strerror(errno));
            break;
        }

        //将客户端套接字放入全局数组
        eventset(&g_events[i], cfd, recvdata, &g_events[i]);   
         //将cfd添加到红黑树g_efd中,监听读事件
        eventadd(g_efd, EPOLLIN, &g_events[i]);                        

    } while(0);

    printf("new connect [%s:%d][time:%ld], pos[%d]\n", 
            inet_ntoa(cin.sin_addr), ntohs(cin.sin_port), g_events[i].last_active, i);
    return ;
}

/* -------------------------------------------*/
/**
* @brief  recvdata 
* @function 响应读事件的回调函数,将客户端描述符的监听事件改变
*
* @fd  客户端套接字
* @events   当前响应的事件类型
* @arg 指向数组元素的指针
*
/* -------------------------------------------*/
void recvdata(int fd, int events, void *arg)
{
    struct myevent_s *ev = (struct myevent_s *)arg;
    int len;

    //读文件描述符, 数据存入myevent_s成员buf中
    len = recv(fd, ev->buf, sizeof(ev->buf), 0);            

    eventdel(g_efd, ev);        //将该节点从红黑树上摘除

    if (len > 0) {

        ev->len = len;
        ev->buf[len] = '\0';                                //手动添加字符串结束标记
        printf("C[%d]:%s\n", fd, ev->buf);

        //设置该 fd 对应的回调函数为 senddata
        eventset(ev, fd, senddata, ev);                    

        //将fd加入红黑树g_efd中,监听其写事件,当套接字可以写的时候,触发epoll_wait返回
        eventadd(g_efd, EPOLLOUT, ev);                      

    } else if (len == 0) {
        close(ev->fd);
        /* ev-g_events 地址相减得到偏移元素位置 */
        printf("[fd=%d] pos[%ld], closed\n", fd, ev-g_events);
    } else {
        close(ev->fd);
        printf("recv[fd=%d] error[%d]:%s\n", fd, errno, strerror(errno));
    }

    return;
}


/* -------------------------------------------*/
/**
* @brief  senddata 
* @function 向客户端发送信息
*
* @fd  客户端套接字
* @events   当前响应的事件类型
* @arg 指向数组元素的指针
*
/* -------------------------------------------*/
void senddata(int fd, int events, void *arg)
{
    struct myevent_s *ev = (struct myevent_s *)arg;
    int len;

    len = send(fd, ev->buf, ev->len, 0);                    //直接将数据 回写给客户端。未作处理
  
    if (len > 0) {

        printf("send[fd=%d], [%d]%s\n", fd, len, ev->buf);
        eventdel(g_efd, ev);                                //从红黑树g_efd中移除
        eventset(ev, fd, recvdata, ev);                     //将该fd的 回调函数改为 recvdata
        eventadd(g_efd, EPOLLIN, ev);                       //从新添加到红黑树上， 设为监听读事件

    } else {
        close(ev->fd);                                      //关闭链接
        eventdel(g_efd, ev);                                //从红黑树g_efd中移除
        printf("send[fd=%d] error %s\n", fd, strerror(errno));
    }

    return ;
}


/* -------------------------------------------*/
/**
* @brief  initlistensocket 
* @function 服务端套接字初始化,将套接字加入epoll树中
*
* @efd  epoll句柄
* @port 待绑定的端口号
*
/* -------------------------------------------*/
void initlistensocket(int efd, short port)
{
	 //创建监听套接字
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    //设置监听套接字属性为套接字属性为非阻塞
    fcntl(lfd, F_SETFL, O_NONBLOCK);                            

   	//结构体最后一个元素初始化,将listenfd信息存放
    eventset(&g_events[MAX_EVENTS], lfd, acceptconn, &g_events[MAX_EVENTS]);

   	//将listenfd加入epoll数中
    eventadd(efd, EPOLLIN, &g_events[MAX_EVENTS]); 

    struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));                                               //bzero(&sin, sizeof(sin))
	sin.sin_family = AF_INET; // ipv4 地址
	sin.sin_addr.s_addr = htonl(INADDR_ANY); //任意ip
	sin.sin_port = htons(port);
     //绑定ip和端口,将主动套接字变为被动连接套接字
	bind(lfd, (struct sockaddr *)&sin, sizeof(sin));
	//监听,最大等待队列20,创建2个队列,一个是已经完成3次握手的队列,一个是正在等待3次握手完成的队列
	listen(lfd, 20);

    return ;
}

int main(int argc, char *argv[])
{
	//使用用户指定端口.如未指定,用默认端口
    unsigned short port = SERV_PORT;
    if (argc == 2)
        port = atoi(argv[1]);                         
   
   
    //创建epoll句柄
    g_efd = epoll_create(MAX_EVENTS+1);                 
    if (g_efd <= 0)
        printf("create efd in %s err %s\n", __func__, strerror(errno));

    //服务端套接字初始化,将套接字加入epoll树中
    initlistensocket(g_efd, port);                      

    //创建已经满足就绪事件的文件描述符数组 
  	 struct epoll_event events[MAX_EVENTS+1];            
	printf("server running:port[%d]\n", port);

    int checkpos = 0, i;//一次循环检测100个。 使用checkpos控制检测对象
    while (1) {

        /* 超时验证，每次测试100个链接，不测试listenfd 当客户端60秒内没有和服务器通信，则关闭此客户端链接 */
        long now = time(NULL);                          //当前时间
        for (i = 0; i < 100; i++, checkpos++) {         //一次循环检测100个。 使用checkpos控制检测对象
            if (checkpos == MAX_EVENTS)//过滤listenfd
                checkpos = 0;
            if (g_events[checkpos].status != 1)         //不在红黑树 g_efd 上
                continue;
            long duration = now - g_events[checkpos].last_active;       //客户端不活跃的世间

            if (duration >= 60) {
                close(g_events[checkpos].fd);                           //关闭与该客户端链接
                printf("[fd=%d] timeout\n", g_events[checkpos].fd);
                //g_efd树根  ,结构体地址
                eventdel(g_efd, &g_events[checkpos]);                   //将该客户端 从红黑树 g_efd移除,并将自定义数组中的status置为0
            }
        }
        //关闭连接结束

  

        /* -------------------------------------------*/
		/**
		* @brief  epoll_wait 
		* @function 非阻塞等待1秒钟监控事件的响应,响应成功,将事件结构体放入events中
		* @return nfd 代表返回就绪的文件描述符
		*
		/* -------------------------------------------*/
        int nfd = epoll_wait(g_efd, events, MAX_EVENTS+1, 1000);
        if (nfd < 0) {
            printf("epoll_wait error, exit\n");
            break;
        }

        for (i = 0; i < nfd; i++) {
            /*使用自定义结构体myevent_s类型指针, 接收 联合体data的void *ptr成员*/
            struct myevent_s *ev = (struct myevent_s *)events[i].data.ptr;  
           
            if ((events[i].events & EPOLLIN) && (ev->events & EPOLLIN)) {           //读就绪事件
            	//调用回调函数
                ev->call_back(ev->fd, events[i].events, ev->arg);
            }
            if ((events[i].events & EPOLLOUT) && (ev->events & EPOLLOUT)) {         //写就绪事件
                ev->call_back(ev->fd, events[i].events, ev->arg);
            }
        }//end for

    }
    /* 退出前释放所有资源 */
    return 0;
}


