#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <netdb.h>
#include <pwd.h>
#include <pthread.h>



/* This is sucky because I don't know the cause of it, but include has to be done
in order -

	#include <sys/types.h>
	#include <sys/socket.h>
	#include <linux/if.h>
	#include <linux/if_tun.h>

otherwise, I get error while compilation which is-

	/usr/include/linux/if.h:184:19: error: field ‘ifru_addr’ has incomplete type
	/usr/include/linux/if.h:185:19: error: field ‘ifru_dstaddr’ has incomplete type
	/usr/include/linux/if.h:186:19: error: field ‘ifru_broadaddr’ has incomplete type
	/usr/include/linux/if.h:187:19: error: field ‘ifru_netmask’ has incomplete type
	/usr/include/linux/if.h:188:20: error: field ‘ifru_hwaddr’ has incomplete type

Probably some conflicts need to be avoided which is done by including sys/sockets.h first
*/


#define BUFF_SIZE 3000



char prog_name[20];



struct user {
	char uname[BUFF_SIZE];
	struct passwd uinfo;
};


struct tun_dev {
	char device[BUFF_SIZE];
	int pers;
	char ip_addr[60];
};

struct input {
	int port;								/* Port number to be used for connection */
	int over;								/* Underlying type of connection SOCK_DGRAM/SOCK_STREAM */
	int verbose;							/* verbose tunneling flag */
	struct tun_dev dev;
	char serv[BUFF_SIZE];					/* Server name or ip address (for client) */
	char mode;								/* Mode of operation */
	char port_str[10];						/* Port number as a string */
	struct addrinfo server, * serv_ptr;		/* Address info of server (for client) */
	struct user usr;
} in;


struct node {
	struct node * next;
	char packet[BUFF_SIZE];
	int packet_len;
};


struct queue {
	struct node * head;
	struct node * tail;
	int is_empty;
	int num_ele;
	pthread_mutex_t mutex;
};


struct thread_args {
	int fd;
	struct queue * n2t_queue, * t2n_queue;
};






int mktun (char * , int , struct ifreq * );
int settun (int , struct ifreq * , int );
void read_bytes_tun (int , struct ifreq * );
void tunnel (int, int);
void check_usage (int, char * []);
void print_usage ();
void raise_error (const char *);
int net_connect();
int server_connect ();
int client_connect ();
int push (struct queue * , char * , int );
struct node * pop (struct queue * );
struct queue * new_queue ();
void * tun_io (void * );
void * sock_io (void * );







void main (int argc, char * argv[]) {

	int nfd, tfd;
	struct ifreq ifr;		/* structure having parameters for ioctl() call */

	strcpy(prog_name, argv[0]);
	in.verbose = 0;
	check_usage(argc,argv);
	//strcpy(in.dev, "tun2");


	tfd = mktun(in.dev.device, IFF_TUN | IFF_NO_PI, &ifr);

	if (tfd < 0) {
		printf("Error creating tun device\n");
		exit(1);
	} else {
		printf("Created tun device(%d): %s\n", tfd, ifr.ifr_name);
	}

	
	if (in.mode == 'm') {
		settun(tfd, &ifr, in.dev.pers);
		exit(0);
		}

	nfd = net_connect();

	tunnel(nfd, tfd);
	//read_bytes_tun(tfd, &ifr);

}







/* 
	mktun: This function takes a tun device name, flags and structure for ioctl()
		   and creates a tun device. This function is almost the same as given on
		   kernel.org on page https://www.kernel.org/doc/Documentation/networking/tuntap.txt
	
	input: char * <string containing the tun device name. Can be null>
		   int <flags for ioctl() call>
		   struct ifreq * <structure for ioctl() parameters>
	returns: int <file descriptor of the tun device> (is -ve if fails)
*/

int mktun (char * dev, int flags, struct ifreq * ifr) {

	int fd, stat;
	char * clonedev = "/dev/net/tun";


	/* Get the file descriptor from the tun clone to use as input to ioctl() */
	if ( (fd = open(clonedev, O_RDWR) ) < 0 )
		return fd;
	
	/* Now prepare the structure ifreq for ioctl() */
	memset(ifr, 0, sizeof(*ifr));					/* reset memory to 0 */
	ifr->ifr_flags = flags;				/* set the flags IFF_TUN or IFF_TAP and IFF_NO_PI */

	if (*dev)
		strcpy(ifr->ifr_name, dev);
	
	/* Now we try and create a device */
	if ( (stat = ioctl(fd, TUNSETIFF, (void *) ifr) ) < 0 ) {
		perror("ioctl()");
		close(fd);
		return stat;
	}

	/* Now write back the name of the interface to dev just to be sure */
	strcpy(dev, ifr->ifr_name);

	/* Now return the file descriptor that can be used to talk to the tun interface */
	return fd;
}







int settun (int tun_fd, struct ifreq * ifr, int pers) {

	int owner = -1, group = -1;
	char * buff;
	int buffsize;
	struct passwd * s;

	buffsize = sysconf(_SC_GETPW_R_SIZE_MAX);
	buff = (char *) malloc (buffsize);
	if (buff == NULL)
		raise_error("error allocating memory for passwd buffer");


	if (! pers) {
		/* We want to make this fd non persistent. */
		if (ioctl(tun_fd, TUNSETPERSIST, 0) < 0) 
			raise_error("ioctl() - unset persistence");
	}
	else {
		
		if (in.usr.uname != NULL) {
			/* If user was given, get his uid and gid */
			getpwnam_r(in.usr.uname, &in.usr.uinfo, buff, buffsize, &s);

			if (s == NULL)
				raise_error("getpwnam_r()");

			owner = in.usr.uinfo.pw_uid;
			group = in.usr.uinfo.pw_gid;

		} else {
			/* Get effective uid and gid */
			owner = geteuid();
			group = getegid();
		}

		/* Set owner using ioctl() */
		if (owner != -1)
			if ( ioctl(tun_fd, TUNSETOWNER, owner) < 0) 
				raise_error("ioctl() - set owner");

		/* Set group using ioctl() */
		if (group != -1)
			if ( ioctl(tun_fd, TUNSETGROUP, group) < 0) 
				raise_error("ioctl() - set group");

		/* Now set persistence */
		if ( ioctl(tun_fd, TUNSETPERSIST, 1) < 0) 
			raise_error("ioctl() - set persistence");

	}

/*	if (in.dev.ip_addr[0] != 0)
		set_ip(tun_fd, ifr);*/
	
	printf("tun device: %s,",ifr->ifr_name);
	if (owner != -1)
		printf(" uid: %d,", owner);
	if (group != -1)
		printf(" gid: %d,", group);
	printf(" persistence: %d\n", pers);

	free(buff);
	return 0;
}





/* 
	read_bytes_tun: This function reads data from tun device and displays how many
					bytes it read on stdout. This is used for testing the read from
					tun device.
	
	input:  int <tun device descriptor>
			struct ifreq * <pointer to structure ifreq which was populated during mktun().
	returns: void
*/


void read_bytes_tun (int tun_fd, struct ifreq * ifr) {

	int stat;
	char buff[3000];
	int buff_size = 3000;

	while (1) {
		stat = read(tun_fd, buff, buff_size);
		if (stat < 0) 
		raise_error("read() failed");

		printf("Read %d bytes from tun device %s from: \n",stat, ifr->ifr_name);
	} 

}





/*
	tunnel: This function is where tunnelling actually happens. We use select() call
			to juggle between tun and network devices. We read the data from one
			device and write on other. This way tun data is sent over network and
			data received over network is written to tun.
	
	input:	int <network socket descriptor>,
			int <tun device descriptor>
	returns: void

*/




void tunnel (int sockfd, int tunfd) {


	pthread_t t2n, n2t;
	int ret1, ret2;
	struct queue * n2tq, * t2nq;
	struct thread_args tun_to_net, net_to_tun;
	

	n2tq = new_queue();
	t2nq = new_queue();

	if (n2tq == NULL || t2nq == NULL)
		raise_error("new_queue() failed");

	net_to_tun.fd = sockfd;
	net_to_tun.n2t_queue = n2tq;
	net_to_tun.t2n_queue = t2nq;

	tun_to_net.fd = tunfd;
	tun_to_net.n2t_queue = n2tq;
	tun_to_net.t2n_queue = t2nq;

	printf("Starting the tunnelling threads\n");
	ret1 = pthread_create( &t2n, NULL, tun_io, (void *) &tun_to_net);
	ret2 = pthread_create( &n2t, NULL, sock_io, (void *) &net_to_tun);

	pthread_join(t2n, NULL);
	printf("Thread tun-to-network returned\n");
	pthread_join(n2t, NULL);
	printf("Thread network-to-tun returned\n");

}







void * sock_io (void * ptr) {


	struct thread_args * args;
	args = (struct thread_args *) ptr;

	int fd;
	struct queue * n2t, * t2n;
	char buff[BUFF_SIZE];
	int stat = 0;
	int len;

	fd = args->fd;
	n2t = args->n2t_queue;
	t2n = args->t2n_queue;

	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 10000;

	fd_set r_set;
	struct sockaddr_storage peer_addr;
	socklen_t peer_addr_len = sizeof(peer_addr);


	printf("Thread %ld: Starting operations on socket\n",pthread_self());
	while (1) {
		
		/* Do pending operations from socket device to incoming queue */
		FD_ZERO(&r_set);
		FD_SET(fd, &r_set);
		stat = select(fd+1, &r_set, NULL, NULL, &timeout);
		if (stat < 0 && errno == EINTR);

		if (stat < 0)
			raise_error("select() failed");


		if (FD_ISSET(fd, &r_set)) {

			stat = 0;
			len = 0;
			bzero(buff, BUFF_SIZE);

			if (in.over == SOCK_DGRAM) {

				if (in.mode == 'c')
					stat = recvfrom(fd, buff, BUFF_SIZE, 0, NULL, NULL);
				else if (in.mode == 's')
					stat = recvfrom(fd, buff, BUFF_SIZE, 0, (struct sockaddr *) &peer_addr, &peer_addr_len);
				else
					raise_error("invalid mode");

				if (stat < 0)
					raise_error("recvfrom() failed");


			} else {

				stat = read(fd, (char *) &len, sizeof(len));
				if (stat < 0)
					raise_error("read() failed on socket");

				stat = read(fd, buff, ntohs(len));
				if (stat < 0)
					raise_error("read() failed on socket");
			}

			if (in.verbose == 1)
				printf("Read %d bytes on socket\n",stat);

			pthread_mutex_lock(&n2t->mutex);
			
			if (! push(n2t, buff, stat))
				raise_error("push() failed in tun to network queue");

			if (in.verbose == 1)
				printf("n2t has %d packets\n",n2t->num_ele);

			pthread_mutex_unlock(&n2t->mutex);


		}



		/* Write all the data on socket that needs to be written */
		while (t2n->is_empty != 1) {

			pthread_mutex_lock(&t2n->mutex);

			struct node * n;
			n = pop(t2n);
			if (in.verbose == 1)
				printf("t2n has %d more packets\n",t2n->num_ele);

			pthread_mutex_unlock(&t2n->mutex);

			if (in.over == SOCK_DGRAM) {

				if (in.mode == 'c')
					stat = sendto(fd, n->packet, n->packet_len, 0, in.serv_ptr->ai_addr, in.serv_ptr->ai_addrlen);
				else if (in.mode == 's')
					stat = sendto(fd, n->packet, n->packet_len, 0, (struct sockaddr *) &peer_addr, peer_addr_len);
				else
					raise_error("invalid mode");

				if (stat < n->packet_len)
					raise_error("sendto() - incomplete send");
			} else {

				len = htons(n->packet_len);
				stat = write(fd, &len, sizeof(len));
				if (stat < 0)
					raise_error("write() failed on socket");

				stat = write(fd, n->packet, n->packet_len);
				if (stat < 0)
					raise_error("write() failed on socket");
			}


			if (in.verbose == 1) 
				printf("wrote %d bytes on socket\n",stat);
			free(n);
		}
	}
}





void * tun_io (void * ptr) {

	struct thread_args * args;
	args = (struct thread_args *) ptr;

	int fd;
	struct queue * n2t, * t2n;
	char buff[BUFF_SIZE];
	int stat = 0;

	fd = args->fd;
	n2t = args->n2t_queue;
	t2n = args->t2n_queue;

	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 10000;

	fd_set r_set;

	printf("Thread %ld: Starting operations on tun\n",pthread_self());
	while (1) {
		
		/* First write all the data on tun that needs to be written */

		while (n2t->is_empty != 1) {

			pthread_mutex_lock(&n2t->mutex);

			struct node * n;
			n = pop(n2t);
			if (in.verbose == 1)
				printf("n2t has %d more packets\n",n2t->num_ele);

			pthread_mutex_unlock(&n2t->mutex);

			stat = write(fd, n->packet, n->packet_len);
			if (stat < 0)
				raise_error("tun_io - write() failed");

			if (in.verbose == 1)
				printf("Wrote %d bytes on tun device\n",stat);

			free(n);
		}



		/* Do pending operations from tun device to outgoing queue */
		FD_ZERO(&r_set);
		FD_SET(fd, &r_set);
		stat = select(fd+1, &r_set, NULL, NULL, &timeout);
		if (stat < 0 && errno == EINTR);

		if (stat < 0)
			raise_error("select() failed");


		if (FD_ISSET(fd, &r_set)) {


			stat = 0;
			bzero(buff, BUFF_SIZE);
			stat = read(fd, buff, BUFF_SIZE);
			if (stat < 0)
				raise_error("tun_io - read() failed");

			if (in.verbose == 1)
				printf("Read %d bytes on tun device\n",stat);

			pthread_mutex_lock(&t2n->mutex);
			
			if (! push(t2n, buff, stat))
				raise_error("push() failed in tun to network queue");

			if (in.verbose == 1)
				printf("t2n has %d more packets\n",t2n->num_ele);

			pthread_mutex_unlock(&t2n->mutex);

		}
	}
}

	




/* 
	raise_error: Print error message and exit with non zero status 
	
	input: const char * <string having message to be displayed>
	returns: void
*/

void raise_error (const char * msg) {

	perror(msg);
	exit(1);
}






/* 
	check_usage: Check the command line arguments passed and output prompting messages
				 if necessary. We parse and store the commandline arguments in the 
				 global structure
	
	input: int <number of cmd arguments received>, char *[] array of strings having the
			received arguments
	returns: void (exits with 0 status if usage had to be printed for user)
*/

void check_usage (int argc, char *argv[] ) {

	int arg;

	bzero(in.serv, BUFF_SIZE);
	bzero(in.dev.device, BUFF_SIZE);
	bzero(in.dev.ip_addr, 60);
	bzero(in.port_str, 10);
	in.dev.pers = 0;
	

	while ((arg = getopt(argc, argv, "evhm:s:d:p:o:u:")) != -1) {

		switch (arg) {
			case 'h':	print_usage();
						break;

			case 'e':	in.dev.pers = 1;
						break;

			case 'u':	strcpy(in.usr.uname, optarg);
						break;	

			case 'v':	in.verbose = 1;
						break;

			case 'm':	if (strcmp(optarg,"s") == 0)
							in.mode = 's';
						else if (strcmp(optarg,"c") == 0)
							in.mode = 'c';
						else if (strcmp(optarg,"m") == 0)
							in.mode = 'm';
						else {
							fprintf(stderr,"Invalid mode of operation %s. Valid modes are 'c' and 's'\n",optarg);
							exit(1);
						}
						break;

			case 's':	strcpy(in.serv,optarg);
						break;

			case 'd':	strcpy(in.dev.device,optarg);
						break;

			case 'p':	strcpy(in.port_str,optarg);
						in.port = atoi(in.port_str);
						break;

			case 'o':	if (strcmp(optarg,"tcp") == 0)
							in.over = SOCK_STREAM;
						else if (strcmp(optarg,"udp") == 0)
							in.over = SOCK_DGRAM;
						else {
							fprintf(stderr,"Invalid underlying protocol %s. Valid args are 'tcp' or 'udp'\n",optarg);
							exit(1);
						}
						break;

			case '?':	fprintf(stderr,"Invalid arguments\n");
						print_usage();
						break;

			default:	
						print_usage();

		}

	}


	if (in.mode != 'c' && in.mode != 's' && in.mode != 'm') {
		fprintf(stderr,"Operation mode is mandetory argument. use -m to specify the mode\n");
		exit(1);
	}

	if (in.dev.device[0] == 0)
		raise_error("Device name is mandetory argument. use -d to specify. Use -h for help");

	/* Usage check for tunneling operations */
	if (in.mode != 'm') {
		if (in.port_str[0] == 0)
		raise_error("Port number is mandetory argument in while tunnelling. use -p to specify. Use -h for help");
		
		if (in.mode == 'c' && in.serv[0] == 0) 
			raise_error("Client mode needs option -s with server name/ip as an argument");
		
	} else if (in.mode == 'm') {

	/* Usage check for creation of tun device */
		if (in.dev.pers != 0 && in.dev.pers != 1)
			raise_error("Invalid persistence value given");

	}

}







/* 	
	print_usage: print the usage and exit

	input: void
	returns: exit with 0 status
*/




void print_usage () {
	
	printf("Usage: %s -m [mode] -d [device name] -p [port] -o [underlying prot] -s [server] -v \n\
	            -m [mode] -d [device name] -e -u [user]\n\
	where,\n\
		-m: mode        : either of 's' or 'c' signifying whether to act as client or server while tunnelling,\n\
		                   OR\n\
		                  can be 'm' which tells the program that it is supposed to create new tun device.\n\
		-d: device name : tun device name\n\
		-p: port        : port number used by client and server for tunnelling (for listening in case of server).\n\
		                  only significant in case mode isn't 'm'\n\
		-o: protocol    : name of the underlying protocol over which tunneling happens. can be 'tcp' or 'udp'\n\
		                  only significant in case mode isn't 'm'\n\
		-s: server name : name or ip address of the server. (Only considered in case of client)\n\
		                  only significant in case mode isn't 'm'\n\
		-e: persistence : Whether to set device persistent or not\n\
		                  only significant in case mode is 'm'\n\
		-u: user        : User to set as owner of the device\n\
		                  only significant in case mode is 'm'\n\
		-v: verbose     : print the info messages which may slow down the performance\n\
		-h: help        : print this usage\n", prog_name);
	
	exit(0);
}






/*
	net_connect: Get the network connection with remote machine depending on whether we are
				 in client mode or a server mode.
	
	input: void
	returns: int (socket file descriptor)
*/



int net_connect() {

	/* We have to get a net connection. For client, it means try to connect to the server. 
	For server, it means, listen to a port and accept connection coming in there */

	int sock;

	if (in.mode == 'c')
		sock = client_connect();
	else if (in.mode == 's')
		sock = server_connect();
	else {
		fprintf(stderr,"invalid mode of operation %c\n", in.mode);
		exit(1);
	}
	

	return sock;
}





/*
	client_connect: Get the network connection considering that we are client. use
					getaddrinfo() for resolving the server and then create a UDP or
					TCP socket. Connect to server on same socket number if we are
					using TCP. Return the descriptor.
	
	input: void
	returns: int (socket file descriptor)
*/



int client_connect () {

	struct addrinfo * servptr, *s;
	int ret, sockfd;
	memset(&in.server, 0, sizeof(in.server));

	/* Fill in the server 'hint' information for getaddrinfo() */
	in.server.ai_family = AF_INET;
	in.server.ai_socktype = in.over;
	in.server.ai_protocol = 0;
	in.server.ai_flags = AI_CANONNAME|AI_ADDRCONFIG;


	ret = getaddrinfo(in.serv, in.port_str, &in.server, &in.serv_ptr);
	if (ret != 0)
		raise_error("getaddrinfo()");
	
	for (s = in.serv_ptr; s != NULL; s = s->ai_next) {
		sockfd = socket(s->ai_family, s->ai_socktype, s->ai_protocol);

		if (sockfd == -1)
			continue;

		if (in.over == SOCK_DGRAM) {
			/* in case of UDP, if we are here, means we have got the socket.
			so break out of the loop now */
			break;
			in.serv_ptr = s;		/* Store the server address info for UDP sendto() call later */
		}
		else
			/* For TCP, we need to do a connect() call with server over 
			the socket we have got */
			if (connect(sockfd, s->ai_addr, s->ai_addrlen) == 0)
				break;

		close(sockfd);
	}

	/* We are here means we either broke out of the loop in which case S != NULL
	or loop ran out hinting we couldnt get the connection => S == NULL */
	if (s == NULL)
		raise_error("connect()");
	
	return sockfd;
}





/*
	server_connect: Get the network connection considering that we are server. That 
					means in case of TCP, we have to do listen() and accept() calls
					from client. In case of UDP, we are simply ready after we bind.
	
	input: void
	returns: int (socket file descriptor)
*/



int server_connect () {

	struct addrinfo me, * myptr, * m;
	int ret, servsock, sock;

	memset(&me, 0, sizeof(me));
	me.ai_family = AF_INET;
	me.ai_socktype = in.over;
	me.ai_protocol = 0;
	me.ai_flags = AI_PASSIVE;
	me.ai_canonname = NULL;
	me.ai_addr = NULL;
	me.ai_next = NULL;



	in.serv_ptr = &in.server;


	ret = getaddrinfo(NULL, in.port_str, &me, &myptr);
	if (ret != 0)
		raise_error("getaddrinfo()");
	
	for (m = myptr; m != NULL; m = m->ai_next) {
		servsock = socket(m->ai_family, m->ai_socktype, m->ai_protocol);

		if (servsock == -1)
			continue;

		if (bind(servsock, m->ai_addr, m->ai_addrlen) == 0)
			break;

		close(servsock);
	}

	if (m == NULL)
		raise_error("bind()");

	if (in.over != SOCK_DGRAM) {
		listen(servsock, 5);
		sock = accept(servsock, NULL, NULL);
		if (sock < 0)
			raise_error("accept()");
	
		return sock;

	} else {
		return servsock;
	}
}








/* ---------------------------------------------------------------------
		QUEUE FUNCTIONS
	probably should separate these in different file
--------------------------------------------------------------------- */


struct queue * new_queue () {

	struct queue * q;
	q = (struct queue *) malloc (sizeof(struct queue));

	if (q == NULL)
		return NULL;
	

	q->head = NULL;
	q->tail = NULL;
	q->is_empty = 1;
	pthread_mutex_init(&q->mutex, NULL);
	q->num_ele = 0;

	return q;
}







int push (struct queue * q, char * buff, int len) {

	struct node * n;
	n = (struct node *) malloc (sizeof(struct node));

	if (n == NULL)
		return 0;
	
	bzero(n->packet, BUFF_SIZE);
	bcopy(buff, n->packet, BUFF_SIZE);
	n->packet_len = len;
	n->next = NULL;

	if (q->is_empty) {
		
		q->head = n;
		q->tail = n;
		q->is_empty = 0;

	} else {
	
		q->tail->next = n;
		q->tail = n;
	}

	q->num_ele++;

	return 1;
}



struct node * pop (struct queue * q) {

	if (q->is_empty)
		return NULL;
	
	struct node * n;
	n = q->head;

	q->head = q->head->next;

	if (q->head == NULL) {
		q->tail = NULL;
		q->is_empty = 1;
	}
	q->num_ele--;

	return n;
}
