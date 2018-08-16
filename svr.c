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
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/un.h>
#include <ctype.h>
#include <fcntl.h>           /* Definition of AT_* constants */
#include <unistd.h>

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

    SOCK_DEBUG("close %d\n",pev->fd);

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

static int SockAPI_UnixCreate(const char* fname,int issvr,int type, mode_t perm , uid_t uid, gid_t gid )
{
    int ret = 0;
    int sock;
    struct sockaddr_un addr;

    //  TODO: add selinux support
    //  setsockcreatecon
    //      socket
    //          selabel_lookup ...

    /* Create socket from which to read. */
    sock = socket(AF_UNIX, type|SOCK_CLOEXEC, 0);
    if (sock < 0) {
        return -1;
    }


    /* Create addr. */
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, fname);

    if( issvr ){
        if ((unlink(addr.sun_path) != 0) && (errno != ENOENT)) {
            SOCK_MESSAGE("unlink %s fail %d/%s\n",addr.sun_path,errno,strerror(errno));
            close(sock);
            return -1;
        }

        /* Bind the UNIX domain address to the created socket */
        ret = bind(sock, (struct sockaddr *) &addr, sizeof(struct sockaddr_un));

        if( ret == 0 ){
            if (lchown(addr.sun_path, uid, gid)) {
                SOCK_MESSAGE("chown %s to %d,%d fail %d/%s\n",addr.sun_path,uid, gid,errno,strerror(errno));
                ret = -1;
            }          
        }
        if( ret == 0 ){
            if (fchmodat(AT_FDCWD, addr.sun_path, perm, AT_SYMLINK_NOFOLLOW)) {
                SOCK_MESSAGE("chmod %s to %x fail %d/%s\n",addr.sun_path,perm,errno,strerror(errno));
                ret = -1;
            }
        }
        if( ret == 0 && type == SOCK_STREAM )
            ret = listen(sock,TCP_DEFAULTBACK_LOG);
    }
    else{
        ret = connect(sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_un));
    }

    if( ret != 0 ) {
        printf("bind to %s fail %d/%s\n",addr.sun_path,errno,strerror(errno));
        close(sock);
        sock    = -1;
    }

    return sock;
}




static int tcp_onrecv(struct PollCtl *pCtl,struct PollEv *ev)
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

static int tcp_onaccept(struct PollCtl *pCtl,struct PollEv *ev)
{
    int sd = SockAPI_TCPAccept(ev->fd,NULL);
    if( sd >= 0 ){
        int ret = PollCtl_Add(pCtl,sd,tcp_onrecv,NULL,NULL);
        if( ret < 0 ){
            SOCK_MESSAGE("add %d to poll fail\n",sd);
            close(sd);
        }
        return 0;
    }

    return -1;
}

static void tcpserver(int port)
{
    struct PollCtl CtlInfo;
    int listensd    = SockAPI_TCPCreate(NULL,port,1,-1);
    if( listensd < 0 ){
        SOCK_MESSAGE("listen port %d fail\n",port);
        return ;
    }

    PollCtl_Init(&CtlInfo);
    int ret = PollCtl_Add(&CtlInfo,listensd,tcp_onaccept,NULL,NULL);
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


static int unix_onread(struct PollCtl *pCtl,struct PollEv *ev)
{
    int ret = -1;
    char    buf[1024];
    WRAP_SYSAPI( ret , recv(ev->fd,buf ,sizeof(buf) - 1,0) );
    if( ret <= 0 )
        return -1;

    buf[ret]   = 0;
    SOCK_DEBUG("%d recv:%s\n",ev->fd,buf);
    return 0;
}



static int unix_onaccept(struct PollCtl *pCtl,struct PollEv *ev)
{
    int sd = accept(ev->fd,NULL,NULL);
    if( sd >= 0 ){
        int ret = PollCtl_Add(pCtl,sd,unix_onread,NULL,NULL);
        if( ret < 0 ){
            SOCK_MESSAGE("add %d to poll fail\n",sd);
            close(sd);
        }
        else{
        SOCK_DEBUG("accept %d succ\n",ev->fd);
        }
        return 0;
    }
    else{
        SOCK_MESSAGE("accept %d fail\n",ev->fd);
    }

    return -1;
}


static void unixserver(const char* path)
{
    struct PollCtl CtlInfo;
    int listensd    = SockAPI_UnixCreate(path,1,SOCK_STREAM,0777,0,0);
    if( listensd < 0 ){
        SOCK_MESSAGE("listen %s fail\n",path);
        return ;
    }

    PollCtl_Init(&CtlInfo);
    int ret = PollCtl_Add(&CtlInfo,listensd,unix_onaccept,NULL,NULL);
    if( ret < 0 ){
        SOCK_MESSAGE("push %s to pollqueue fail\n",path);
        close(listensd);
        return ;
    }
    
    SOCK_MESSAGE("begin run service on %s\n",path);
    while( (ret = PollCtl_RunOnce(&CtlInfo,-1) ) > 0 ) ;

    SOCK_MESSAGE("stop run poll,ret:%d\n",ret);
    PollCtl_Close(&CtlInfo);
}


static int unixclient_onreadstdin(struct PollCtl *pCtl,struct PollEv *ev)
{
    int ret = -1;
    char    buf[1024];
    int sock = (intptr_t)ev->priv;
    WRAP_SYSAPI( ret , read(ev->fd,buf ,sizeof(buf) - 1) );
    if( ret <= 0 )
        return -1;

    buf[ret++]   = '\n';
    buf[ret]   = 0;
    WRAP_SYSAPI( ret , write(sock,buf ,ret) );
    printf("%d recv:%s write ret:%d\n",ev->fd,buf,ret);
    return 0;
}

static int unixclient_onread(struct PollCtl *pCtl,struct PollEv *ev)
{
    int ret = -1;
    char    buf[1024];
    WRAP_SYSAPI( ret , recv(ev->fd,buf ,sizeof(buf) - 1,0) );
    if( ret <= 0 )
        return -1;

    buf[ret]   = 0;
    printf("%d recv:%s\n",ev->fd,buf);
    return 0;
}

static void unixcli(const char* path)
{
    struct PollCtl CtlInfo;
    int sock    = SockAPI_UnixCreate(path,0,SOCK_STREAM,0,0,0);
    if( sock < 0 ){
        SOCK_MESSAGE("connect %s fail\n",path);
        return ;
    }
    int ret = PollCtl_Add(&CtlInfo,sock,unixclient_onread,NULL,NULL);
    if( ret < 0 ){
        SOCK_MESSAGE("push %s to pollqueue fail\n",path);
        close(sock);
        return ;
    }

    ret = PollCtl_Add(&CtlInfo,0,unixclient_onreadstdin,NULL,(void *)(intptr_t )sock);
    if( ret < 0 ){
        SOCK_MESSAGE("push %s to pollqueue fail\n",path);
        close(sock);
        return ;
    }
 
    SOCK_MESSAGE("begin run service on %s\n",path);
    while( (ret = PollCtl_RunOnce(&CtlInfo,-1) ) > 0 ) ;

    SOCK_MESSAGE("stop run poll,ret:%d\n",ret);
    PollCtl_Close(&CtlInfo);
}

int main (int argc, char *argv[])
{
    if( argc > 2 ){
        if( !strcmp("-l",argv[1]) ){
            const char *p = argv[2];
            while( isdigit(*p) ) p++;
            if( *p == 0 )
                tcpserver(atoi(argv[2]));
            else
                unixserver(argv[2]);
            return 0;
        }
        else if( !strcmp("-c",argv[1]) ){
            if( argc == 3 ){
                unixcli(argv[2]);
            }
            return 0;
        }
    }
    printf("run as server: %s -l unixsockname|tcport\n"
           "run as client: %s -c unixsockname\n",argv[0],argv[0]);
    return 0;
}


