#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <errno.h>

#define SERVER_PORT  12345

#define TRUE             1
#define FALSE            0


#define SOCK_MESSAGE(fmt,...)       printf(" %s:%d " fmt,__func__,__LINE__,__VA_ARGS__)
#define SOCK_DEBUG(fmt,...)         printf(" %s:%d " fmt,__func__,__LINE__,__VA_ARGS__)
#define WRAP_SYSAPI(ret,func)           do{ \
    ret = func;                             \
}while( ret == -1 && ( errno == EINTR))

#define POLLIN_FLAG                (POLLIN|POLLHUP|POLLERR)
#define SET_POLLIN_FLAG(pollfds,fileid)    { (pollfds).fd	= fileid; (pollfds).events = POLLIN_FLAG; }
#define IS_POLLIN_OK(revent)  (((revent) & POLLIN) && (!((revent) & (POLLERR|POLLHUP|POLLNVAL))) )
#define TCP_DEFAULTBACK_LOG    5

#define MAX_POLLFD_NUM      32

struct PollCtl;
struct PollEv;
/*
 *  callback return 0: ok, other: error
 */
typedef int (*POLLEV_CALLBACK)(struct PollCtl *pCtl,struct PollEv *ev);

struct PollEv{
    int                 fd;
    
    POLLEV_CALLBACK     onread;
    POLLEV_CALLBACK     onclose;
    
    void*               priv;
};

struct PollCtl{
    int                 nfds;
    struct pollfd       fds[MAX_POLLFD_NUM];
    struct PollEv       evs[MAX_POLLFD_NUM];
};

struct PollCtl  sPollCtl;

static void PollEvSet(struct pollfd *pfd,int fd)
{
    memset(pfd,0,sizeof(*pfd));
    pfd->fd     = fd;
    pfd->events = POLLIN_FLAG;
}

static void PollEvReset(struct pollfd *pfd,struct PollEv* pev)
{
    memset(pfd,0,sizeof(*pfd));
    memset(pev,0,sizeof(*pev));
    pfd->fd  = -1;
    pev->fd  = -1;
}

static void PollEvClose(struct PollCtl *pCtl,int index,int doclose)
{
    if( index < 0 || index >= pCtl->nfds || index >= MAX_POLLFD_NUM)
        return ;

    struct pollfd*  pfd = pCtl->fds + index;
    struct PollEv*  pev = pCtl->evs + index;

    if( doclose ){
        if( pev->onclose )
            pev->onclose(pCtl,pev);
        close(pev->fd);
    }

    PollEvReset(pfd,pev);
    if( pCtl->nfds > 1 && index != pCtl->nfds -1 ){
        int i = pCtl->nfds -1;
        memcpy(pfd,&pCtl->fds[i],sizeof(*pfd));
        memcpy(pev,&pCtl->evs[i],sizeof(*pev));
        PollEvReset(&pCtl->fds[i],&pCtl->evs[i]);
    }

    pCtl->nfds  --;
}



int PollCtl_Init(struct PollCtl *pCtl)
{
    int i = 0;
    memset(pCtl,0,sizeof(*pCtl));
    for( ; i < MAX_POLLFD_NUM ; i ++ )  
        PollEvReset(&pCtl->fds[i],&pCtl->evs[i]);

    return 0;
}

int PollCtl_Add(struct PollCtl *pCtl,int sd,POLLEV_CALLBACK onread,POLLEV_CALLBACK onclose,void *priv)
{
    if( sd < 0 || !onread || pCtl->nfds >= MAX_POLLFD_NUM )
        return -1;

    pCtl->evs[pCtl->nfds].fd        = sd;
    pCtl->evs[pCtl->nfds].onread    = onread;
    pCtl->evs[pCtl->nfds].onclose   = onclose;
    pCtl->evs[pCtl->nfds].priv      = priv;
    PollEvSet(&pCtl->fds[pCtl->nfds],sd);
    SOCK_DEBUG("add %d = index %d ++\n",sd,pCtl->nfds);
    pCtl->nfds ++;
    
    return 0;
}

int PollCtl_DelBySd(struct PollCtl *pCtl,int sd,int doclose)
{
    int i = 0; 
    
    for( ; i < pCtl->nfds && pCtl->fds[i].fd != sd && pCtl->fds[i].fd >= 0; i++ ) ;
    if( i >= pCtl->nfds || pCtl->fds[i].fd < 0 )
        return -1;

    PollEvClose(pCtl,i,doclose);
    return 0;
}

void PollCtl_Close(struct PollCtl *pCtl)
{
    while( pCtl->nfds > 0 )
        PollEvClose(pCtl,pCtl->nfds-1,1);
}

int PollCtl_RunOnce(struct PollCtl *pCtl,int timeout_ms)
{
    int rc = poll(pCtl->fds, pCtl->nfds, timeout_ms);        
    if (rc < 0){
        SOCK_MESSAGE("  poll(%d) failed %d:%s\n",pCtl->nfds,errno,strerror(errno));
        return -1;
    }

    if (rc == 0){
        SOCK_MESSAGE("  poll(%d) timed out.  End program.\n",pCtl->nfds);
        return 0;
    }

    for (int i = 0; i < pCtl->nfds && rc > 0 ; ){
        /*********************************************************/
        /* Loop through to find the descriptors that returned    */
        /* POLLIN and determine whether it's the listening       */
        /* or the active connection.                             */
        /*********************************************************/
        if(pCtl->fds[i].revents == 0){
            i ++;
            continue;
        }

        rc      --;

        int fd  = pCtl->fds[i].fd;
        int err = -1;
        if( IS_POLLIN_OK(pCtl->fds[i].revents) ){
            if( pCtl->evs[i].onread )
                err = pCtl->evs[i].onread(pCtl,&pCtl->evs[i]);
        }

        if( err != 0 ){
            PollEvClose(pCtl,i,1);
            SOCK_DEBUG("error close %d, after close nfds:%d\n",i,pCtl->nfds);
        }
        else{
            i ++;
        }
    }

    return 1;
}



static int CreateTCP(struct addrinfo *pinfo,
        const struct sockaddr *addr,socklen_t addrlen,
        int isbind,int connecttimeout_ms)
{
    int sockfd = -1;
    if( pinfo ){
        sockfd = socket(pinfo->ai_family, pinfo->ai_socktype,
                pinfo->ai_protocol);
        addr    = pinfo->ai_addr;
        addrlen = pinfo->ai_addrlen;
    }
    else{
        sockfd = socket(AF_INET, SOCK_STREAM,0);
    }

    if(sockfd < 0) {
        return -1;
    }

    int on	= 1;
    if( isbind ){
        if( setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0 ){
            SOCK_MESSAGE( "tcp reuse port %d fail\n",htons(((struct sockaddr_in *)addr)->sin_port));
        }
        if(bind(sockfd, addr, addrlen) == 0
                && listen(sockfd, TCP_DEFAULTBACK_LOG) == 0 ) {
            return sockfd;
        }

        close(sockfd);
        return -1;        
    }

    int sock_opt    = 1;
    if( connecttimeout_ms != -1){
        ioctl(sockfd, FIONBIO, &sock_opt);
    }

    int connret = connect(sockfd, addr, addrlen);

    if( connret == -1 && connecttimeout_ms != -1
            && (errno == EWOULDBLOCK || errno == EINPROGRESS) ){
        struct pollfd pfd;
        pfd.fd      = sockfd;
        pfd.events  = POLLOUT;
        int error   = 0;
        socklen_t errorlen = sizeof(error);

        int ret = -1;
        WRAP_SYSAPI(ret , poll( &pfd, 1, connecttimeout_ms ));
        if(  ret == 1
                && getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (char *)&error, &errorlen) == 0 
                && error == 0 ){
            connret = 0;
        }
    }

    if( connret == 0) {
        if( connecttimeout_ms != -1){
            sock_opt    = 0;
            ioctl(sockfd, FIONBIO, &sock_opt);
        }

        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (void*)&on, sizeof(on)) ;
        return sockfd;
    }

    close(sockfd);
    return -1;
}

int SockAPI_TCPCreate(const char *serv,uint16_t port,int isbind,int connecttimeout_ms)
{
    int 	sockfd	= -1;
    char	portstr[32];
    struct addrinfo hints, *res, *ressave;

    if( serv && *serv && inet_addr(serv) == inet_addr("127.0.0.1")){
        struct sockaddr_in addr;
        memset(&addr,0,sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = inet_addr(serv);

        return CreateTCP(NULL,(const struct sockaddr *)&addr,sizeof(addr),isbind,connecttimeout_ms);
    }

    bzero(&hints, sizeof(struct addrinfo));
    hints.ai_family 		= PF_UNSPEC;
    hints.ai_socktype		= SOCK_STREAM;
    if( isbind ){
        hints.ai_flags		= AI_PASSIVE;
    }
    else{
        if( !serv || !serv[0] ){
            SOCK_MESSAGE( "SockAPI_Create	param error for %s:%d bind:%d\n",serv ? serv: "NULL",port,isbind);
            return -1;
        }
    }

    sprintf(portstr,"%d",port);

    int ret =  getaddrinfo(serv, portstr, &hints, &res);
    if( ret != 0) {
        SOCK_MESSAGE( "SockAPI_Create getaddr fail for %s:%d bind:%d\n",serv ? serv: "NULL",port,isbind);
        return -1;
    }

    ressave = res;

    do {
        struct sockaddr_in* paddr = (struct sockaddr_in*)res->ai_addr;
        if( isbind ){
            if( paddr->sin_addr.s_addr == htonl(0x7f000001))
                continue;
        }
        else{
            if( paddr->sin_addr.s_addr == 0)
                continue;
        }

        sockfd = CreateTCP(res,NULL,0,isbind,connecttimeout_ms);
        if(sockfd >= 0) {
            break;
        }
    }while((res = res->ai_next) != NULL);

    freeaddrinfo(ressave);
    if( sockfd >= 0 )
        return sockfd;

    SOCK_MESSAGE( "SockAPI_Create fatal create fail for %s:%d bind:%d\n",serv ? serv: "NULL",port,isbind);
    return -1;
}

int SockAPI_TCPAccept(int ld,struct sockaddr_storage* dst)
{
    if( ld == -1 )
        return -1;

    struct sockaddr_storage tmp;
    if( !dst ) dst = &tmp;

    int sd  = -1;
    while(1){
        socklen_t len = sizeof(tmp);
        sd = accept(ld,(struct sockaddr *)dst,&len);
        if( sd >= 0 )
            break;

        if( sd == -1 ){
            if( errno == EINTR )
                continue;
            else{
                SOCK_MESSAGE("accept fail %d:%s\n",errno,strerror(errno));
                return -1;
            }
        }
    }

    int flag = 1;
    int ret = setsockopt( sd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag) );
    if (ret == -1) {
        SOCK_MESSAGE("Couldn't setsockopt(TCP_NODELAY) to %d\n",sd);
    }
    return sd;
}


static int onrecv(struct PollCtl *pCtl,struct PollEv *ev)
{
    char    buf[512];
    int     ret;
    WRAP_SYSAPI( ret , recv(ev->fd,buf,sizeof(buf),0) );
    if( ret > 0 ){
        buf[ret]    = 0;
        ret = send(ev->fd,buf,ret,0);
        SOCK_MESSAGE("recv %s echo ret:%d\n",buf,ret);
    }
    return ret > 0 ? 0 : -1;
}

static int onaccept(struct PollCtl *pCtl,struct PollEv *ev)
{
    int sd = SockAPI_TCPAccept(ev->fd,NULL);
    if( sd >= 0 ){
        int ret = PollCtl_Add(pCtl,sd,onrecv,NULL,NULL);
        if( ret < 0 ){
            SOCK_MESSAGE("add %d to poll fail\n",sd);
            close(sd);
        }
        return 0;
    }

    return -1;
}

static void runasserver(int port)
{
    struct PollCtl CtlInfo;
    int listensd    = SockAPI_TCPCreate(NULL,port,1,-1);
    if( listensd < 0 ){
        SOCK_MESSAGE("listen port %d fail\n",port);
        return ;
    }

    PollCtl_Init(&CtlInfo);
    int ret = PollCtl_Add(&CtlInfo,listensd,onaccept,NULL,NULL);
    if( ret < 0 ){
        SOCK_MESSAGE("push port %d to pollqueue fail\n",port);
        close(listensd);
        return ;
    }
    
    SOCK_MESSAGE("begin run service on port %d\n",port);
    while( (ret = PollCtl_RunOnce(&CtlInfo,-1) ) > 0 ) ;

    SOCK_MESSAGE("stop run poll,ret:%d\n",ret);
    PollCtl_Close(&CtlInfo);
}

static void runasclient(const char* dst, int port)
{
}

int main (int argc, char *argv[])
{
    runasserver(330);
    return 0;
}

#if 0
int main (int argc, char *argv[])
{
    int    len, rc, on = 1;
    int    listen_sd = -1, new_sd = -1;
    int    desc_ready, end_server = FALSE, compress_array = FALSE;
    int    close_conn;
    char   buffer[80];
    struct sockaddr_in6   addr;
    int    timeout;
    struct pollfd fds[200];
    int    nfds = 1, current_size = 0, i, j;

    /*************************************************************/
    /* Create an AF_INET6 stream socket to receive incoming      */
    /* connections on                                            */
    /*************************************************************/
    listen_sd = socket(AF_INET6, SOCK_STREAM, 0);
    if (listen_sd < 0)
    {
        perror("socket() failed");
        exit(-1);
    }

    /*************************************************************/
    /* Allow socket descriptor to be reuseable                   */
    /*************************************************************/
    rc = setsockopt(listen_sd, SOL_SOCKET,  SO_REUSEADDR,
            (char *)&on, sizeof(on));
    if (rc < 0)
    {
        perror("setsockopt() failed");
        close(listen_sd);
        exit(-1);
    }

    /*************************************************************/
    /* Set socket to be nonblocking. All of the sockets for      */
    /* the incoming connections will also be nonblocking since   */
    /* they will inherit that state from the listening socket.   */
    /*************************************************************/
    rc = ioctl(listen_sd, FIONBIO, (char *)&on);
    if (rc < 0)
    {
        perror("ioctl() failed");
        close(listen_sd);
        exit(-1);
    }

    /*************************************************************/
    /* Bind the socket                                           */
    /*************************************************************/
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family      = AF_INET6;
    memcpy(&addr.sin6_addr, &in6addr_any, sizeof(in6addr_any));
    addr.sin6_port        = htons(SERVER_PORT);
    rc = bind(listen_sd,
            (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0)
    {
        perror("bind() failed");
        close(listen_sd);
        exit(-1);
    }

    /*************************************************************/
    /* Set the listen back log                                   */
    /*************************************************************/
    rc = listen(listen_sd, 32);
    if (rc < 0)
    {
        perror("listen() failed");
        close(listen_sd);
        exit(-1);
    }

    /*************************************************************/
    /* Initialize the pollfd structure                           */
    /*************************************************************/
    memset(fds, 0 , sizeof(fds));

    /*************************************************************/
    /* Set up the initial listening socket                        */
    /*************************************************************/
    fds[0].fd = listen_sd;
    fds[0].events = POLLIN;
    /*************************************************************/
    /* Initialize the timeout to 3 minutes. If no                */
    /* activity after 3 minutes this program will end.           */
    /* timeout value is based on milliseconds.                   */
    /*************************************************************/
    timeout = (3 * 60 * 1000);

    /*************************************************************/
    /* Loop waiting for incoming connects or for incoming data   */
    /* on any of the connected sockets.                          */
    /*************************************************************/
    do
    {
        /***********************************************************/
        /* Call poll() and wait 3 minutes for it to complete.      */
        /***********************************************************/
        printf("Waiting on poll()...\n");
        rc = poll(fds, nfds, timeout);

        /***********************************************************/
        /* Check to see if the poll call failed.                   */
        /***********************************************************/
        if (rc < 0)
        {
            perror("  poll() failed");
            break;
        }

        /***********************************************************/
        /* Check to see if the 3 minute time out expired.          */
        /***********************************************************/
        if (rc == 0)
        {
            printf("  poll() timed out.  End program.\n");
            break;
        }


        /***********************************************************/
        /* One or more descriptors are readable.  Need to          */
        /* determine which ones they are.                          */
        /***********************************************************/
        current_size = nfds;
        for (i = 0; i < current_size; i++)
        {
            /*********************************************************/
            /* Loop through to find the descriptors that returned    */
            /* POLLIN and determine whether it's the listening       */
            /* or the active connection.                             */
            /*********************************************************/
            if(fds[i].revents == 0)
                continue;

            /*********************************************************/
            /* If revents is not POLLIN, it's an unexpected result,  */
            /* log and end the server.                               */
            /*********************************************************/
            if(fds[i].revents != POLLIN)
            {
                printf("  Error! revents = %d\n", fds[i].revents);
                end_server = TRUE;
                break;

            }
            if (fds[i].fd == listen_sd)
            {
                /*******************************************************/
                /* Listening descriptor is readable.                   */
                /*******************************************************/
                printf("  Listening socket is readable\n");

                /*******************************************************/
                /* Accept all incoming connections that are            */
                /* queued up on the listening socket before we         */
                /* loop back and call poll again.                      */
                /*******************************************************/
                do
                {
                    /*****************************************************/
                    /* Accept each incoming connection. If               */
                    /* accept fails with EWOULDBLOCK, then we            */
                    /* have accepted all of them. Any other              */
                    /* failure on accept will cause us to end the        */
                    /* server.                                           */
                    /*****************************************************/
                    new_sd = accept(listen_sd, NULL, NULL);
                    if (new_sd < 0)
                    {
                        if (errno != EWOULDBLOCK)
                        {
                            perror("  accept() failed");
                            end_server = TRUE;
                        }
                        break;
                    }

                    /*****************************************************/
                    /* Add the new incoming connection to the            */
                    /* pollfd structure                                  */
                    /*****************************************************/
                    printf("  New incoming connection - %d\n", new_sd);
                    fds[nfds].fd = new_sd;
                    fds[nfds].events = POLLIN;
                    nfds++;

                    /*****************************************************/
                    /* Loop back up and accept another incoming          */
                    /* connection                                        */
                    /*****************************************************/
                } while (new_sd != -1);
            }

            /*********************************************************/
            /* This is not the listening socket, therefore an        */
            /* existing connection must be readable                  */
            /*********************************************************/

            else
            {
                printf("  Descriptor %d is readable\n", fds[i].fd);
                close_conn = FALSE;
                /*******************************************************/
                /* Receive all incoming data on this socket            */
                /* before we loop back and call poll again.            */
                /*******************************************************/

                do
                {
                    /*****************************************************/
                    /* Receive data on this connection until the         */
                    /* recv fails with EWOULDBLOCK. If any other         */
                    /* failure occurs, we will close the                 */
                    /* connection.                                       */
                    /*****************************************************/
                    rc = recv(fds[i].fd, buffer, sizeof(buffer), 0);
                    if (rc < 0)
                    {
                        if (errno != EWOULDBLOCK)
                        {
                            perror("  recv() failed");
                            close_conn = TRUE;
                        }
                        break;
                    }

                    /*****************************************************/
                    /* Check to see if the connection has been           */
                    /* closed by the client                              */
                    /*****************************************************/
                    if (rc == 0)
                    {
                        printf("  Connection closed\n");
                        close_conn = TRUE;
                        break;
                    }

                    /*****************************************************/
                    /* Data was received                                 */
                    /*****************************************************/
                    len = rc;
                    printf("  %d bytes received\n", len);

                    /*****************************************************/
                    /* Echo the data back to the client                  */
                    /*****************************************************/
                    rc = send(fds[i].fd, buffer, len, 0);
                    if (rc < 0)
                    {
                        perror("  send() failed");
                        close_conn = TRUE;
                        break;
                    }

                } while(TRUE);

                /*******************************************************/
                /* If the close_conn flag was turned on, we need       */
                /* to clean up this active connection. This            */
                /* clean up process includes removing the              */
                /* descriptor.                                         */
                /*******************************************************/
                if (close_conn)
                {
                    close(fds[i].fd);
                    fds[i].fd = -1;
                    compress_array = TRUE;
                }


            }  /* End of existing connection is readable             */
        } /* End of loop through pollable descriptors              */

        /***********************************************************/
        /* If the compress_array flag was turned on, we need       */
        /* to squeeze together the array and decrement the number  */
        /* of file descriptors. We do not need to move back the    */
        /* events and revents fields because the events will always*/
        /* be POLLIN in this case, and revents is output.          */
        /***********************************************************/
        if (compress_array)
        {
            compress_array = FALSE;
            for (i = 0; i < nfds; i++)
            {
                if (fds[i].fd == -1)
                {
                    for(j = i; j < nfds; j++)
                    {
                        fds[j].fd = fds[j+1].fd;
                    }
                    i--;
                    nfds--;
                }
            }
        }

    } while (end_server == FALSE); /* End of serving running.    */

    /*************************************************************/
    /* Clean up all of the sockets that are open                 */
    /*************************************************************/
    for (i = 0; i < nfds; i++)
    {
        if(fds[i].fd >= 0)
            close(fds[i].fd);
    }
}
#endif
