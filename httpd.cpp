#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

#define BUFFER_SIZE 4096

/*
 * 主状态机的两种可能状态：当前正在分析请求行、当前正在分析头部字段
 */
enum CHECK_STATE {CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER};

/*
 * 从状态机的三种可能状态，即行的读取状态：读取到一个完整的行、行出错、行数据尚且不完整
 */
enum LINE_STATUS {LINE_OK = 0, LINE_BAD, LINE_OPEN};

/*
 * 服务器处理HTTP请求的结果
 * NO_REQUEST：请求不完整，需要继续读取客户数据
 * GET_REQUEST：获得一个完整的客户请求
 * BAD_REQUEST：客户请求有语法错误
 * FORBIDDEN_REQUEST：客户对资源没有足够的访问权限
 * INTERNAL_ERROR：服务器内部错误
 * CLOSED_CONNECTION：客户端已经关闭连接了
 */
enum HTTP_CODE {NO_REQUEST, GET_REQUEST, BAD_REQUEST, FORBIDDEN_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION};

/*
 * 为了简化问题，我们没有给客户端发送一个完整的HTTP应答报文
 * 而只是根据服务器的处理结果发送如下成功或失败信息
 */
static const char* szret[] = {
       "HTTP/1.1 200 OK\r\nContent-Type: text/html;\r\nContent-Length: 22\r\n\r\nI get a correct result",
       "HTTP/1.1 200 OK\r\nContent-Type: text/html;\r\nContent-Length: 15\r\n\r\nSomething wrong"
    };

/*
 * 从状态机，用于解析出一行内容
 */
LINE_STATUS parse_line(char *buffer, int &checked_index, int &read_index)
{
    char temp;
    //checked_index指向buffer(应用程序的读缓冲区)中当前正在分析的字节
    //read_index指向buffer中客户数据的尾部的下一字节
    //buffer中第0~checked_index字节都已分析完毕
    //第checked_index~(read_index-1)字节由下面的循环挨个分析
    for(; checked_index < read_index; ++checked_index){
        //获取当前要分析的字节
        temp = buffer[checked_index];
        //如果当前字节是'\r'，即回车符，说明可能读到一个完整行
        if('\r' == temp){
            //如果'\r'字符碰巧是目前buffer中的最后一个已经被读入的客户数据
            //那么这次分析没有读取到一个完整的行，返回LINE_OPEN以表示还需继续读客户数据才能进一步分析
            if(read_index == (checked_index + 1)){
                return LINE_OPEN;
            }
            //如果下一个字符是'\n'则说明成功读到一个完整行
            else if('\n' == buffer[checked_index + 1]){
                buffer[checked_index++] = '\0';
                buffer[checked_index++] = '\0';
                return LINE_OK;
            }
            //佛足额的话，说明客户发送的HTTP请求存在语法问题
            return LINE_BAD;
        }
        //当前的字节是'\n'，即换行符，则说明可能读到一个完整的行
        else if('\n' == temp){
            if((checked_index > 1) && ('\r' == buffer[checked_index - 1])){
                buffer[checked_index - 1] = '\0';
                buffer[checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //如果所有内容都分析完毕还没有遇到'\r'，则返回LINE_OPEN，表示还需继续读取客户数据才能进一步分析
    return LINE_OPEN;
}


/*
 * 分析请求行
 */
HTTP_CODE parse_requestline(char *temp, CHECK_STATE &checkstate)
{
    char *url = strpbrk(temp, " \t");
    //如果请求行中没有空白字符或'\t'字符，则HTTP请求必有问题
    if(!url){
        return BAD_REQUEST;
    }
    *url++ = '\0';

    char *method = temp;
    //仅支持GET方法
    if(0 == strcasecmp(method, "GET")){
        printf("The request method is GET\n");
    }
    else{
        return BAD_REQUEST;
    }

    url += strspn(url, " \t");
    char *version = strpbrk(url, " \t");
    if(!version){
        return BAD_REQUEST;
    }
    *version++ = '\0';
    version += strspn(version, " \t");

    //仅支持HTTP/1.1
    if(0 != strcasecmp(version, "HTTP/1.1")){
        return BAD_REQUEST;
    }

    //检查URL是否合法
    if(strncasecmp(url, "http://", 7) == 0){
        url += 7;
        url = strchr(url, '/');
    }
    if((!url) || (url[0] != '/')){
        return BAD_REQUEST;
    }
    printf("The request url is: %s\n", url);

    //HTTP请求行处理完毕，状态转移到头部字段的分析
    checkstate = CHECK_STATE_HEADER;
    return NO_REQUEST;
}


/*
 * 分析头部字段
 */
HTTP_CODE parse_headers(char *temp)
{
    //遇到一个空行，说明我们得到了一个正确的HTTP请求
    if('\0' == temp[0]){
        return GET_REQUEST;
    }
    //处理“HOST头部字段”
    else if(0 == strncasecmp(temp, "Host:", 5)){
        temp += 5;
        temp += strspn(temp, " \t");
        printf("the request host is: %s\n", temp);
    }
    //其他头部字段都不处理
    else{
        printf("I can not handle this header:(%s)\n", temp);
    }
    return NO_REQUEST;
}


/*
 * 分析HTTP请求的入口函数
 */
HTTP_CODE parse_content(char *buffer, int &checked_index, CHECK_STATE &checkstate, int &read_index, int &start_line)
{
    //记录当前行的读取状态
    LINE_STATUS linestatus = LINE_OK;
    //记录HTTP请求的处理结果
    HTTP_CODE retcode = NO_REQUEST;
    /*
     * 主状态机，用于从buffer中取出所有完整的行
     */
    while(LINE_OK == (linestatus = parse_line(buffer, checked_index, read_index))){
        //start_line是行在buffer中的起始位置
        char *temp = buffer + start_line;
        //记录下一行的起始位置
        start_line = checked_index;
        //checkstate记录主状态机当前的状态
        switch(checkstate){
            //第一个状态：分析请求行
            case CHECK_STATE_REQUESTLINE:{
                retcode = parse_requestline(temp, checkstate);
                if(BAD_REQUEST == retcode){
                    return BAD_REQUEST;
                }
                break;
            }
            // 第二个状态：分析头部字段
            case CHECK_STATE_HEADER:{
                retcode = parse_headers(temp);
                if(BAD_REQUEST == retcode){
                    return BAD_REQUEST;
                }
                else if(GET_REQUEST == retcode){
                    return GET_REQUEST; 
                }
                break;
            }
            default:{
                return INTERNAL_ERROR;
            }
        }
    }
    //若没有读取到一个完整的行，则表示还需要继续读取客户数据才能进行下一步分析
    if(LINE_OPEN == linestatus){
        return NO_REQUEST;
    }
    else{
        return BAD_REQUEST;
    }
}



/*
 * 主函数
 */
int main(int argc, char * argv[])
{
    const char *ip = "0.0.0.0";
    int port = 10000;
    if(argc > 2){
        ip = argv[1];
        port = atoi(argv[2]);
    }
    printf("%s %s %d\n", basename(argv[0]), ip, port);
    

    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);
    int ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret != -1);
    ret = listen(listenfd, 5);
    assert(ret != -1);
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    int fd = accept(listenfd,  (struct sockaddr *)&client_address, &client_addrlength);
    if(fd < 0){
        printf("errno is: %d\n", errno);
    }
    else{
    	//读缓冲区
        char buffer[BUFFER_SIZE];
        memset(buffer, '\0', BUFFER_SIZE);
        int data_read = 0;
        //当前已读取多少字节的客户数据
        int read_index = 0;
        //当前已经分析完了多少字节的客户数据
        int checked_index = 0;
        //行在buffer中的起始位置
        int start_line = 0;

        //设置主状态机的初始状态
        CHECK_STATE checkstate = CHECK_STATE_REQUESTLINE;
        //循环读取客户数据并分析之
        while(1){
            data_read = recv(fd, buffer + read_index, BUFFER_SIZE - read_index, 0);
            if(data_read == -1){
                printf("reading failed\n");
                break;
            }
            else if(data_read == 0){
                printf("remote client has closed the connection\n");
                break;
            }
            read_index += data_read;
            //分析目前已经获取的所有客户数据
            HTTP_CODE result = parse_content(buffer, checked_index, checkstate, read_index, start_line);
            //尚未得到一个完整的HTTP请求
            if(result == NO_REQUEST){
                continue;
            }
            //得到一个完整的、正确的HTTP请求
            else if(result == GET_REQUEST){
                send(fd, szret[0], strlen(szret[0]), 0);
                break;
            }
            //其他情况表示发送错误
            else{
                send(fd, szret[1], strlen(szret[1]), 0);
                break;
            }
        }
        close(fd);
    }
    close(listenfd);
    return 0;
}
