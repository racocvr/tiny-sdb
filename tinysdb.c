/*
	tinysdb - MrB 2020
	
	A command line tool to execute a .tpk on a remote Samsung smart tv.	
	
*/

#include <stdio.h> 
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/socket.h> 
#include <arpa/inet.h> 
#include <unistd.h> 
#include <string.h> 
#include <libgen.h>
#include <sys/stat.h>

#define try( fun, errmsg ) if( (fun) < 0 ) { printf(errmsg); printf("errno: %d\n", errno); return -1; }

#define MAX_PAYLOAD 4096

#define A_SYNC 0x434e5953
#define A_CNXN 0x4e584e43
#define A_OPEN 0x4e45504f
#define A_OKAY 0x59414b4f
#define A_CLSE 0x45534c43
#define A_WRTE 0x45545257
#define A_STAT 0x54415453

#define A_VERSION 0x02000000        // SDB protocol version

typedef struct amessage amessage;
typedef struct apacket apacket;

#ifdef HAVE_BIG_ENDIAN
static inline unsigned __swap_uint32(unsigned x) 
{
    return (((x) & 0xFF000000) >> 24)
        | (((x) & 0x00FF0000) >> 8)
        | (((x) & 0x0000FF00) << 8)
        | (((x) & 0x000000FF) << 24);
}
#define htoll(x) __swap_uint32(x)
#define ltohl(x) __swap_uint32(x)
#define MKID(a,b,c,d) ((d) | ((c) << 8) | ((b) << 16) | ((a) << 24))
#else
#define htoll(x) (x)
#define ltohl(x) (x)
#define MKID(a,b,c,d) ((a) | ((b) << 8) | ((c) << 16) | ((d) << 24))
#endif

#define ID_STAT MKID('S','T','A','T')
#define ID_LIST MKID('L','I','S','T')
#define ID_ULNK MKID('U','L','N','K')
#define ID_SEND MKID('S','E','N','D')
#define ID_RECV MKID('R','E','C','V')
#define ID_DENT MKID('D','E','N','T')
#define ID_DONE MKID('D','O','N','E')
#define ID_DATA MKID('D','A','T','A')
#define ID_OKAY MKID('O','K','A','Y')
#define ID_FAIL MKID('F','A','I','L')
#define ID_QUIT MKID('Q','U','I','T')

typedef union {
    unsigned id;
    struct {
        unsigned id;
        unsigned namelen;
    } req;
    struct {
        unsigned id;
        unsigned mode;
        unsigned size;
        unsigned time;
    } stat;
    struct {
        unsigned id;
        unsigned mode;
        unsigned size;
        unsigned time;
        unsigned namelen;
    } dent;
    struct {
        unsigned id;
        unsigned size;
    } data;
    struct {
        unsigned id;
        unsigned msglen;
    } status;    
} syncmsg;

struct amessage {
    unsigned command;       /* command identifier constant      */
    unsigned arg0;          /* first argument                   */
    unsigned arg1;          /* second argument                  */
    unsigned data_length;   /* length of payload (0 is allowed) */
    unsigned data_check;    /* checksum of data payload         */
    unsigned magic;         /* command ^ 0xffffffff             */
};

struct apacket
{    
    amessage msg;
    unsigned char *data;
};

////////////////////////////////////////////////////////////////////

static void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(-1);
}

static char* cmd2str(unsigned int cmd)
{
	switch(cmd)
	{
		case A_CNXN: return "CNXN";
		case A_OKAY: return "OKAY";
		case A_CLSE: return "CLSE";
		case A_WRTE: return "WRTE";	 
		case A_OPEN: return "OPEN";	 
	}
	return "UNKN";
}

static apacket *get_apacket(void)
{
    apacket *p = malloc(sizeof(apacket));
    if(p == 0) fatal("failed to allocate an apacket");
    memset(p, 0, sizeof(apacket));
	p->data = malloc(MAX_PAYLOAD);
	if(p->data == 0) fatal("failed to allocate an apacket");
    memset(p->data, 0, MAX_PAYLOAD);
	
    return p;
}

static void put_apacket(apacket *p)
{
    if (p != NULL) {
		if( p->data != NULL ) free(p->data);
        free(p);		
        p = NULL;
    }
}

static void recv_packet(int socket, apacket* p)
{
	// read the header
	if( recv(socket, &p->msg, sizeof(p->msg), MSG_WAITALL) != sizeof(p->msg) || p->msg.data_length > MAX_PAYLOAD )
		fatal("%s", __func__);
	
	// read the payload
	int bytes_read = 0, bytes_remaining, current_read;
	while( bytes_read < p->msg.data_length )
	{
		bytes_remaining = p->msg.data_length - bytes_read;
		
		current_read = recv(socket, &p->data[bytes_read], bytes_remaining, 0);
		bytes_read += current_read;
		sleep(0);   // Yield processor
	}
/*	
	printf("received %s(%d, %d) len: %d\n", cmd2str(p->msg.command), p->msg.arg0, p->msg.arg1, p->msg.data_length);
	if( p->msg.command == A_WRTE )
		printf("A_WRTE data: %.*s\n", p->msg.data_length, p->data);
*/	
}

static void send_packet(int socket, apacket* p)
{
    unsigned char *x;
    unsigned sum;
    unsigned count;

    p->msg.magic = p->msg.command ^ 0xffffffff;

    count = p->msg.data_length;
    x = (unsigned char *) p->data;
    sum = 0;
    while(count-- > 0){
        sum += *x++;
    }
    p->msg.data_check = sum;
	
//	printf("sending %s(%d, %d) len: %d\n", cmd2str(p->msg.command), p->msg.arg0, p->msg.arg1, p->msg.data_length);
	
	if( send(socket, &p->msg, sizeof(p->msg), 0) != sizeof(p->msg) || 
				send(socket, p->data, p->msg.data_length, 0) != p->msg.data_length )
		fatal("%s", __func__);
}

static int send_connect( int sock, const char* device_name )
{
	apacket *cp = get_apacket();
    cp->msg.command = A_CNXN;
    cp->msg.arg0 = A_VERSION;
    cp->msg.arg1 = MAX_PAYLOAD;    
	strcpy(cp->data, device_name);
    cp->msg.data_length = strlen((char*) cp->data) + 1;

    send_packet(sock, cp);	
}

static int send_open( int sock, const char *destination, int local_id )
{    
    apacket *p = get_apacket();
    int len = strlen(destination) + 1;

    if(len > (MAX_PAYLOAD-1))
        fatal("destination oversized");
	    
    p->msg.command = A_OPEN;
    p->msg.arg0 = local_id;
    p->msg.data_length = len;
    strncpy((char*) p->data, destination, len);
    
	send_packet(sock, p);
}

static void send_cmd( int sock, unsigned int cmd, int local_id, int remote_id )
{    
    apacket *p = get_apacket();
    p->msg.command = cmd;
    p->msg.arg0 = local_id;
    p->msg.arg1 = remote_id;
    send_packet(sock, p);
}

// send ID_DATA + size + data bytes + ID_DONE + 0 truncated in MAX_PAYLOAD packets
static void send_file( int sock, apacket *p, const char* lpath )
{	
	FILE *fp;	
	if( (fp = fopen(lpath, "r")) == NULL )
		fatal("unable to open %s", lpath);
		
	syncmsg msg;
	apacket *rp = get_apacket();
	unsigned char* buf = malloc(MAX_PAYLOAD);
	int rb;	
	
	while( (rb = fread(buf, 1, MAX_PAYLOAD - p->msg.data_length - sizeof(msg.data), fp)) > 0 )
	{
		msg.data.id = ID_DATA;
		msg.data.size = htoll(rb);	
		memcpy(p->data + p->msg.data_length, &msg.data, sizeof(msg.data));
		p->msg.data_length += sizeof(msg.data);
		
		memcpy(p->data + p->msg.data_length, buf, rb);
		p->msg.data_length += rb;
		
		send_packet( sock, p );
		recv_packet( sock, rp );	

		if( rp->msg.command != A_OKAY )
			fatal("%d: %s", __LINE__, __func__);
		
		p->msg.data_length = 0;
	}
	
	fclose(fp);
	
	msg.data.id = ID_DONE;
	msg.data.size = 0;	// timestamp
	memcpy(p->data, &msg.data, sizeof(msg.data));
	p->msg.data_length = sizeof(msg.data);
	
	send_packet( sock, p );	
	recv_packet( sock, rp );
	
	if( rp->msg.command != A_OKAY )
			fatal("%d: %s", __LINE__, __func__);
		
	put_apacket(rp);
}

static void sync_send( int sock, const char* lpath, const char* rpath, int local_id, int remote_id )
{			
	apacket *p = get_apacket();
    p->msg.command = A_WRTE;
    p->msg.arg0 = local_id;
    p->msg.arg1 = remote_id;
	
	int len = strlen(rpath);
	syncmsg msg;
	
	msg.req.id = ID_SEND;	
    msg.req.namelen = htoll(len);
	memcpy(p->data + p->msg.data_length, &msg.req, sizeof(msg.req));
	p->msg.data_length += sizeof(msg.req);
	
	memcpy(p->data + p->msg.data_length, rpath, len);
	p->msg.data_length += len;
	
	send_file( sock, p, lpath );
	recv_packet( sock, p );		
		
	if( p->msg.command == A_WRTE )
	{
		printf("%.*s\n", p->msg.data_length, p->data);
		send_cmd( sock, A_OKAY, local_id, remote_id );		
	}
	
	put_apacket(p);	
}

static void sync_quit( int sock, const char* rpath, int local_id, int remote_id )
{			
	apacket *p = get_apacket();
    p->msg.command = A_WRTE;
    p->msg.arg0 = local_id;
    p->msg.arg1 = remote_id;
	
	int len = strlen(rpath);
	syncmsg msg;
	
	msg.req.id = ID_QUIT;	
    msg.req.namelen = htoll(len);
	memcpy(p->data + p->msg.data_length, &msg.req, sizeof(msg.req));
	p->msg.data_length += sizeof(msg.req);
	
	memcpy(p->data + p->msg.data_length, rpath, len);
	p->msg.data_length += len;
	
	send_packet( sock, p );
	recv_packet( sock, p );
		
	put_apacket(p);			
}

static void do_sync_push( int sock, const char* lpath, const char* rpath, int local_id )
{
	printf("push %s to %s\n", lpath, rpath);
	
	int remote_id;
	send_open( sock, "sync:", local_id );
	
	apacket *p = get_apacket();
	recv_packet( sock, p );
	
	if( p->msg.command != A_OKAY )
		fatal("open failed");
	
	remote_id = p->msg.arg0;
	put_apacket(p);	
	
	sync_send( sock, lpath, rpath, local_id, remote_id );
	sync_quit( sock, rpath, local_id, remote_id );	
	
	send_cmd( sock, A_CLSE, local_id, remote_id );		
	
	while(1)
	{
		recv_packet( sock, p );		
		
		if( p->msg.command == A_WRTE )
		{
			printf("%.*s\n", p->msg.data_length, p->data);
			send_cmd( sock, A_OKAY, local_id, remote_id );		
		}
		else if( p->msg.command == A_CLSE )
			break;
	}	
}

static void do_appcmd(int sock, const char* cmd, int local_id)
{
	printf("%s: cmd=%s\n", __func__, cmd);
	
	int remote_id;
	send_open( sock, cmd, local_id );
	
	apacket *p = get_apacket();
	recv_packet( sock, p );
	
	if( p->msg.command != A_OKAY )
		fatal("open failed");
	
	remote_id = p->msg.arg0;
	
	while( p->msg.command != A_CLSE )
	{
		recv_packet( sock, p );		
		
		if( p->msg.command == A_WRTE )
		{
			printf("%.*s\n", p->msg.data_length, p->data);
			send_cmd( sock, A_OKAY, local_id, remote_id );		
		}
	}	
	
	put_apacket(p);
}

static void do_connect(int sock, const char* device_name)
{
	apacket *cp = get_apacket();
    cp->msg.command = A_CNXN;
    cp->msg.arg0 = A_VERSION;
    cp->msg.arg1 = MAX_PAYLOAD;    
	strcpy(cp->data, device_name);
    cp->msg.data_length = strlen((char*) cp->data) + 1;

    send_packet(sock, cp);
	
	apacket *p = get_apacket();
	recv_packet(sock, p);
		
	printf("%s: ver: 0x%08X, %d, %s\n", cmd2str(p->msg.command), p->msg.arg0, p->msg.arg1, p->data);
		
	put_apacket(cp);
	put_apacket(p);	
}
   
int main(int argc, char const *argv[]) 
{ 
	if(argc < 3)
	{
		printf("TinySdb v0.1 - executes a .tpk on a remote device - MrB (c)2020\n");
		printf("usage: %s <device_ip> <local_tpk_file>\n", argv[0]);
		
		return 0;
	}
	
    int sock; 
    struct sockaddr_in serv_addr; 
	
	try( sock = socket(AF_INET, SOCK_STREAM, 0), "Socket creation error\n" ); 
           
    serv_addr.sin_family = AF_INET; 
    serv_addr.sin_port = htons(26101);
       
    // Convert IPv4 and IPv6 addresses from text to binary form 
	try( inet_pton(AF_INET, argv[1], &serv_addr.sin_addr), "\nInvalid address/Address not supported\n" ); 
	try( connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)), "Connection Failed\n" );
	
	do_connect(sock, "host::");
	
	char* filename = basename((char*)strdup(argv[2]));
	char* pkgname = strtok(strdup(filename), "-");
	char cmd[MAX_PAYLOAD];
	
	sprintf(cmd, "appcmd:killapp:%s:\0", pkgname);
	do_appcmd(sock, cmd, 1);
	
	sprintf(cmd, "/home/owner/share/tmp/sdk_tools/%s\0", filename);
	do_sync_push(sock, argv[2], cmd, 2);
	
	sprintf(cmd, "shell:0 appinstall tpk %s\0", filename);
	do_appcmd(sock, cmd, 3);
	
	sprintf(cmd, "shell:0 rmfile /home/owner/share/tmp/sdk_tools/%s\0", filename);
	do_appcmd(sock, cmd, 4);
	
	sprintf(cmd, "appcmd:runapp:%s:\0", pkgname);
	do_appcmd(sock, cmd, 5);
	
	close(sock);
	
    return 0; 
}

