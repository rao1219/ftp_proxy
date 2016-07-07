#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <unistd.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define MAXSIZE 2048                        //buffer每回发的最大长度
#define FTP_PORT 21                         //服务器连接端口
#define BACKLOG 10                          //侦听的最大连接数
#define FTP_PASV_CODE 227                   //被动模式代码
#define FTP_ADDR "192.168.32.1"             //服务器&客户端IP
#define PROXYIP "192.168.223.252"           //代理IP
#define CMDPORT 21                          //代理端口
#define CMDSTREAM 1                         //转发控制流的标示
#define DATASTREAM 2                        //转发数据流的标示
#define max(X,Y) ((X) > (Y) ? (X) : (Y))

int static FILE_EXIT_IN_CACHE;              //文件存在缓存的标示
int static IS_DOWNLOADING;                  //正在下载的标示
int static CACHE_OK;                        //缓存完成的标示
int static PASSIVE_MODE;                    //被动模式的标示
int static ACTIVE_MODE;                     //主动模式的标示
int static DO_CACHE;                        //进行缓存的标示

int proxy_IP[4];
char filename[100],destFile[111],cacheBuffer[9999],content[333];
/*
    分别是提取出的文件名，缓存文件地址，用于写入缓存的buffer，用于写入标示文件flags的buffer
*/
int connect_FTP(int ser_port);
/*
    连接FTP（服务器或客户端）的某一端口
*/
int proxy_func(int ser_port, int clifd,int option);
/*
    核心转发函数，将cmd或data从一端转发到另一端。option表示控制流类型(cmd或data)
    因为发现控制流和数据流只有转发端口不同，其转发的逻辑是一样的，所以在原框架基础上可将两者合并，提高代码复用效率
*/
int bind_listen(int port);
/*
    proxy侦听某一端口
*/

void reset_flags(){
    FILE_EXIT_IN_CACHE = 0;
    IS_DOWNLOADING = 0;
    CACHE_OK = 0;
    ACTIVE_MODE = 0;
    PASSIVE_MODE = 0;
    DO_CACHE = 0;
    memset(filename,0,sizeof(filename));
}

void save_flags(){
    int fd = open("./cache/flags",O_RDWR | O_CREAT);
    memset(content,0,sizeof(content));
    sprintf(content,"%d %d %d %d %d %d\n%s\n",FILE_EXIT_IN_CACHE,IS_DOWNLOADING,CACHE_OK,\
                                                ACTIVE_MODE,PASSIVE_MODE,DO_CACHE,filename);
    int num = strlen(content);
    content[num]='\0';
    write(fd,content,num);
    close(fd);
}

void load_flags(){
    int fd = open("./cache/flags",O_RDWR | O_CREAT);
    read(fd,content,333);
    sscanf(content,"%d %d %d %d %d %d\n%[^\n]",&FILE_EXIT_IN_CACHE,&IS_DOWNLOADING,&CACHE_OK,\
                                             &ACTIVE_MODE, &PASSIVE_MODE, &DO_CACHE,filename);
    printf("Loading filename:%s\n",filename);
    close(fd);
}
/*
    以上三个函数为对标示变量的重置，读写操作
    因为cmd和data传输不在一个线程，将这些表示状态的标示符通过文件进行交流
*/

int main (int argc, char **argv) {
    int ctrlfd, connfd, port;
    pid_t childpid;
    socklen_t clilen;
    struct sockaddr_in cliaddr;

    sscanf(PROXYIP, "%d.%d.%d.%d", &proxy_IP[0], &proxy_IP[1], &proxy_IP[2], &proxy_IP[3]);
    port = CMDPORT;

    printf("CMDPORT:%d\n\n",CMDPORT);
    ctrlfd = bind_listen(port);
    clilen = sizeof(struct sockaddr_in);
    while (1) {
        connfd = accept(ctrlfd, (struct sockaddr *)&cliaddr, &clilen);
        reset_flags();
        save_flags();
        if (connfd < 0) {
            printf("[x] Accept failed\n");
            return 0;
        }

        printf("[v] Client: %s:%d connect!\n", inet_ntoa(cliaddr.sin_addr), htons(cliaddr.sin_port));
        if ((childpid = fork()) == 0) {
            close(ctrlfd);
            proxy_func(FTP_PORT, connfd, CMDSTREAM);
            printf("[v] Client: %s:%d terminated!\n", inet_ntoa(cliaddr.sin_addr), htons(cliaddr.sin_port));
            exit(0);
        }

        close(connfd);
    }
    return 0;
}

int connect_FTP(int ser_port) {
    int sockfd;
    char addr[] = FTP_ADDR;
    int byte_num;
    char buffer[MAXSIZE];
    struct sockaddr_in servaddr;

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("[x] Create socket error");
        return -1;
    }

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(ser_port);

    if (inet_pton(AF_INET, addr, &servaddr.sin_addr) <= 0) {
        printf("[v] Inet_pton error for %s", addr);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        printf("[x] Connect error");
        return -1;
    }

    printf("[v] Connect to FTP server\n");
    return sockfd;
}

int proxy_func(int ser_port, int clifd,int option) {
    if(option == DATASTREAM)
        printf("\n-----------------Data stream---------------\n");
    else
        printf("\n-----------------Cmd stream---------------\ n");

    char buffer[MAXSIZE];
    int serfd = -1, datafd = -1, connfd;
    int data_port;
    int byte_num;
    int status, params[7];
    int childpid;
    socklen_t clilen;
    struct sockaddr_in cliaddr;

    int select_sd;
    int i, selectResult = 0;
    fd_set working_set, master_set;

    if ((serfd = connect_FTP(ser_port)) < 0) {
        printf("[x] Connect to FTP server failed.\n");
        return -1;
    }

    datafd = serfd;

    /*初始化socket集 */
    FD_ZERO(&master_set);
    FD_SET(clifd, &master_set);
    FD_SET(serfd, &master_set);

    while (1) {
        // 重置工作集，用于下一轮的选择
        working_set = master_set;
        select_sd = max(clifd, serfd) + 1;

        // 选择并过滤工作集中有读操作的socket
        selectResult = select(select_sd + 1, &working_set, NULL, NULL, NULL);
        if (selectResult > 0) {
            if (FD_ISSET(clifd, &working_set)) {
                memset(buffer, 0, MAXSIZE);

                printf("\nFile_exit_in_cache:%d\nIS_DOWNLOADING:%d\nCACHE_OK:%d\nACTIVE_MODE:%d\nPASSIVEMODE:%d\nDOCACHE:%d\n\n",\
                FILE_EXIT_IN_CACHE,\
                IS_DOWNLOADING,\
                CACHE_OK,\
                ACTIVE_MODE,\
                PASSIVE_MODE,\
                DO_CACHE);

                if ((byte_num = read(clifd, buffer, MAXSIZE)) <= 0) {
                    printf("[!] Client terminated the connection.\n");
                    break;
                }
                load_flags(); //后续操作前需先读取上一次和缓存相关的状态标示
                printf("****************\noption:%d\nis downloading?%d\nfilename?%s***********************\n",\
                                                                            option,IS_DOWNLOADING,filename);

                printf("CLI-BUFFER --%s\n",buffer);
                if(option==DATASTREAM)
                    printf("DATA STREAM,Buffer:%s\n",buffer);
                if(ACTIVE_MODE&&DO_CACHE&&option==DATASTREAM){
                    sprintf(destFile,"./cache/%s",filename);
                    int fd =open(destFile,O_RDWR|O_CREAT|O_APPEND,S_IRUSR|S_IWUSR);
                    write(fd,buffer,byte_num);
                    close(fd);
                }

                if(strncmp(buffer,"RETR",4)==0){
                     /*
                        处理下载请求，拿到待下载的文件名，先在缓存清单里查找，如果找到了把文件存在缓存的标识打开
                    */
                    char name[111];
                    strncpy(filename,buffer+5,strlen(buffer)-5);
                    filename[strlen(buffer)-5] = '\0';
                    printf("\nFilename:%s\n",filename);
                    FILE *fn = fopen("./cache/cache","r");
                    while(fgets(name,MAXSIZE,fn)!=NULL){
                        if(strcmp(name,filename)==0){
                            FILE_EXIT_IN_CACHE = 1;
                            DO_CACHE=0;
                            close(fn);
                            break;
                        }
                    }
                    close(fn);
                    if(FILE_EXIT_IN_CACHE){
                        printf("FILE EXIT IN CACHE.%s\n",filename);
                    }
                    else{
                        int fd = open("./cache/cache",O_RDWR | O_APPEND);
                        int num = strlen(filename);
                        filename[num]='\0';
                        write(fd,filename,num);
                        close(fd);
                        DO_CACHE = 1;
                    }

                    IS_DOWNLOADING = 1;
                    save_flags();
                }
                if(strncmp(buffer,"STOR",4)==0){
                    reset_flags();
                    save_flags();
                }

                if(ACTIVE_MODE&&FILE_EXIT_IN_CACHE&&option==DATASTREAM&&(!CACHE_OK)){
                    /*
                        主动模式缓存的发送
                    */
                    sprintf(destFile,"./cache/%s",filename);
                    if(childpid = fork() ==0){
                        int fd =open(destFile,O_RDWR,S_IRUSR|S_IWUSR);
                        int num;
                        while((num = read(fd,cacheBuffer,8888))!=0){
                            //printf("cacheBuffer:\n%s",cacheBuffer);
                            //if(send(clifd,cacheBuffer,num,0)<0){
                            if (write(serfd, cacheBuffer, num) < 0) {
                                printf("[x]239 Write to client failed.\n");
                                break;
                            }
                            memset(cacheBuffer,0,sizeof(cacheBuffer));
                            if(num<8888)
                                break;
                        }
                        close(fd);
                        exit(0);
                    }
                    CACHE_OK = 1;
                    DO_CACHE=0;
                    save_flags();

                    close(clifd);
                    close(serfd);
                    FD_CLR(clifd,&master_set);
                    continue;
                }


                if(strncmp(buffer,"PORT",4)==0){
                    /*
                        主动模式，处理PORT命令：把PORT前四位IP换成Proxy的IP，并根据后两位计算出端口号
                    */
                    reset_flags();
                    sscanf(buffer, "PORT %d,%d,%d,%d,%d,%d",&params[0],&params[1],&params[2],&params[3],&params[4],&params[5]);
                    printf("\nLine 157 Buffer:%s\n",buffer);
                    memset(buffer, 0, MAXSIZE);
                    sprintf(buffer, "PORT %d,%d,%d,%d,%d,%d\r\n", proxy_IP[0], proxy_IP[1], proxy_IP[2], proxy_IP[3], params[4], params[5]);
                    printf("\nLine 162 Buffer:%s\n",buffer);

                    int port_num = params[4] * 256 + params[5];
                    byte_num = strlen(buffer);

                    ACTIVE_MODE = 1,PASSIVE_MODE = 0;
                    save_flags();

                    printf("BIND LISTEN DATA SOCKET\n");
                    if ((childpid = fork()) == 0) {
                         /*
                            拿到端口号后，开一个线程传数据
                        */
                        reset_flags();
                        datafd = bind_listen(port_num);
                        printf("[-] Waiting for data connection!\n");
                        clilen = sizeof(struct sockaddr_in);

                        connfd = accept(datafd, (struct sockaddr *)&cliaddr, &clilen);

                        if (connfd < 0) {
                            printf("[x] Accept failed\n");
                            return 0;
                        }

                        printf("[v] Data connection from: %s:%d connect.\n", inet_ntoa(cliaddr.sin_addr), htons(cliaddr.sin_port));
                        proxy_func(port_num, connfd,DATASTREAM);
                         /*
                            发现data的转发和cmd的逻辑相同，直接将data端口作为参数传入，并标示为数据流
                        */
                        printf("[!] End of data connection!\n");
                        exit(0);
                    }
                }

                save_flags();
                if (write(serfd, buffer, byte_num) < 0) {
                    printf("[x] Write to server failed.\n");
                    break;
                }
            }

            if (FD_ISSET(serfd, &working_set)) {
                int PASS_DO_CACHE = 0;
                load_flags();
                if(CACHE_OK){
                    //write(clifd,"226 Successfully transferred\r\n",30);
                    CACHE_OK = 0;
                    save_flags();
                }
                printf("****************\noption:%d\nis downloading?%d\nfile exit in cache?%d\n***********************\n",option,IS_DOWNLOADING,FILE_EXIT_IN_CACHE);
                load_flags();
                if(IS_DOWNLOADING&&option==DATASTREAM){
                    if(FILE_EXIT_IN_CACHE){
                         /*
                            被动模式缓存的发送
                            如果文件在缓存里，直接从缓存文件里读，跳过后面写操作
                            如果文件不在缓存里，把文件写入缓存
                        */

                        printf("Exits!!!!filename :%s-------------\n",filename);
                        sprintf(destFile,"./cache/%s",filename);
                        int fd =open(destFile,O_RDWR,S_IRUSR|S_IWUSR);
                        int num;
                        while((num = read(fd,cacheBuffer,8888))!=0){
                            //printf("cacheBuffer:\n%s",cacheBuffer);
                            if (write(clifd, cacheBuffer, num) < 0) {
                                printf("[x]307 Write to client failed.\n");
                                break;
                            }
                            memset(cacheBuffer,0,sizeof(cacheBuffer));
                            if(num<8888)
                                break;
                        }
                        CACHE_OK = 0;
                        FILE_EXIT_IN_CACHE=0;
                        IS_DOWNLOADING=0;
                        save_flags();
                        close(fd);
                        close(clifd);
                        FD_CLR(serfd,&master_set);
                        exit(0);
                    }
                    else{
                        printf("First time to download %s\n",filename);
                        PASS_DO_CACHE=1;
                    }
                }
                memset(buffer, 0, MAXSIZE);
                if ((byte_num = read(serfd, buffer, MAXSIZE)) <= 0) {
                    printf("[!] Server terminated the connection.\n");
                    break;
                }
                if(option==DATASTREAM){
                    if((IS_DOWNLOADING&&PASS_DO_CACHE)||(ACTIVE_MODE&&DO_CACHE)){
                        /*
                            写入缓存
                        */
                        sprintf(destFile,"./cache/%s",filename);
                        int fd =open(destFile,O_RDWR|O_CREAT|O_APPEND,S_IRUSR|S_IWUSR);
                        write(fd,buffer,byte_num);
                        close(fd);
                    }
                }
                printf("****************\noption:%d\nis downloading?%d\n***********************\n",option,IS_DOWNLOADING);
                printf("SER-BUFFER --%s\n",buffer);

                if(ser_port == FTP_PORT)
                    buffer[byte_num] = '\0';

                status = atoi(buffer);

                if (status == FTP_PASV_CODE && ser_port == FTP_PORT) {
                    /*
                        被动模式处理下IP，替换成Proxy的IP
                    */
                    sscanf(buffer, "%d Entering Passive Mode (%d,%d,%d,%d,%d,%d)",&params[0],&params[1],&params[2],&params[3],&params[4],&params[5],&params[6]);
                    printf("Buffer line 174: %s",buffer);
                    memset(buffer, 0, MAXSIZE);
                    sprintf(buffer, "%d Entering Passive Mode (%d,%d,%d,%d,%d,%d)\r\n", status, proxy_IP[0], proxy_IP[1], proxy_IP[2], proxy_IP[3], params[5], params[6]);
                    printf("Buffer line 177: %s",buffer);

                    byte_num = strlen(buffer);
                    PASSIVE_MODE = 1,ACTIVE_MODE = 0;
                    save_flags();

                    if ((childpid = fork()) == 0) {
                        reset_flags();
                        data_port = params[5] * 256 + params[6];
                        datafd = bind_listen(data_port);
                        printf("[-] Waiting for data connection!\n");
                        clilen = sizeof(struct sockaddr_in);
                        connfd = accept(datafd, (struct sockaddr *)&cliaddr, &clilen);
                        if (connfd < 0) {
                            printf("[x] Accept failed\n");
                            return 0;
                        }

                        printf("[v] Data connection from: %s:%d connect.\n", inet_ntoa(cliaddr.sin_addr), htons(cliaddr.sin_port));
                        /*
                            用新拿到的端口号做数据转发
                        */
                        proxy_func(data_port, connfd, DATASTREAM);
                        printf("[!] End of data connection!\n");
                        exit(0);
                    }
                }
                save_flags();
                if (write(clifd, buffer, byte_num) < 0) {
                    printf("[x]373 Write to client failed.\n");
                    break;
                }
            }
        } else {
            printf("[x] Select() returns -1. ERROR!\n");
            return -1;
        }
    }
    return 0;
}

int bind_listen(int port) {
    int listenfd;
    struct sockaddr_in servaddr;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);
    if (bind(listenfd, (struct sockaddr *)&servaddr , sizeof(servaddr)) < 0) {
        perror("bind failed. Error");
        return -1;
    }

    listen(listenfd, BACKLOG);
    return listenfd;
}




