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





struct input {
	int port;								/* Port number to be used for connection */
	int over;								/* Underlying type of connection SOCK_DGRAM/SOCK_STREAM */
	int verbose;							/* verbose tunneling flag */
	char dev[BUFF_SIZE];					/* Tun device name */
	char serv[BUFF_SIZE];					/* Server name or ip address (for client) */
	char mode;								/* Mode of operation */
	char port_str[10];						/* Port number as a string */
	struct addrinfo server, * serv_ptr;		/* Address info of server (for client) */
} in;






void main (int argc, char * argv[]) {

	int nfd, tfd;
	struct ifreq ifr;		/* structure having parameters for ioctl() call */

	strcpy(prog_name, argv[0]);
	in.verbose = 0;
	check_usage(argc,argv);
	//strcpy(in.dev, "tun2");


	tfd = mktun(in.dev, IFF_TUN | IFF_NO_PI, &ifr);

	if (tfd < 0) {
		printf("Error creating tun device\n");
		exit(1);
	} else {
		printf("Created tun device(%d): %s\n", tfd, ifr.ifr_name);
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

	int owner = -1, group = 1;

	if (! pers) {
		/* We want to make this fd non persistent. */
		if (ioctl(tun_fd, TUNSETPERSIST, 0) < 0) 
			raise_error("ioctl() - unset persistence");
	}
	else {

		/* Get effective uid and gid */
		owner = geteuid();
		group = getegid();

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

	printf("tun device: %s,",ifr->ifr_name);
	if (owner != -1)
		printf(" uid: %d,", owner);
	if (group != -1)
		printf(" gid: %d,", group);
	printf(" persistence: %d\n", pers);

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

	int lrgfd;
	lrgfd = (sockfd > tunfd) ? sockfd : tunfd;		/* chose larger fd for select() */
	char buff[BUFF_SIZE];
	int read_bytes, wrote_bytes;
	int ret;			/* return value */
	fd_set r_set;		/* set of file descriptors */
	int len;

	/* Following two structures are needed for storing client's address in server's case
	NOTE that 'sockaddr_storage' is the only type that worked. sockaddr_in, sockaddr, etc
	did not work. */
	struct sockaddr_storage peer_addr;
	socklen_t peer_addr_len = sizeof(peer_addr);


	while (1) {

		FD_ZERO(&r_set);			/* clear the set */
		FD_SET(sockfd, &r_set);		/* Add the two descriptors to the set for read checking */
		FD_SET(tunfd, &r_set);

		/* Select will add the descriptor in the set if it is ready for a read. ISSET macro
		is to be used to check if the descriptor is in the set */
		ret = select(lrgfd+1, &r_set, NULL, NULL, NULL);

		/* if select() returned an error because it caught a signal, we can try 
		again */
		if (ret < 0 && errno == EINTR)
			continue;

		/* is select() fails because of invalid descriptor or alloc failure, exit() */
		if (ret < 0) 
			raise_error("select() failed");
		


		/* Case where tun device is ready to be read */

		if (FD_ISSET(tunfd, &r_set)) {
			
			/* Read the data from tun device and write it on the network */
			bzero(buff, BUFF_SIZE);
			read_bytes = read(tunfd, buff, BUFF_SIZE);
			if (read_bytes < 0)
				raise_error("read() on tun failed");

			if (in.over != SOCK_DGRAM) {
				/* TCP makes the data appear as if it is a stream. Hence first we write the length 
				of the data and then we write the packet itself. This helps at the receiver to 
				distinguish between packets. (otherwise it will appear as single data stream and 
				packet boundries can not be found). */
				len = htons(read_bytes);
				wrote_bytes = write(sockfd, &len, sizeof(len));
				if (wrote_bytes < 0)
					raise_error("write() on socket failed");
			

				wrote_bytes = write(sockfd, buff, read_bytes);
				if (wrote_bytes < 0)
					raise_error("write() on socket failed");

			} else {
				/* For UDP, we have to consider two cases */	
				if (in.mode == 'c')
					/* Client already has server's address, so no worries there. Just do a sendto() */
					wrote_bytes = sendto(sockfd, buff, read_bytes, 0, in.serv_ptr->ai_addr, in.serv_ptr->ai_addrlen);
				else if (in.mode == 's') 
					/* Server does not have client's address from the start, so we have to use address stored
					by recvfrom() call which must have happened some time before (server never initiates the 
					conversation, so it is fair assumption that recvfrom() has happened before) */
					wrote_bytes = sendto(sockfd, buff, read_bytes, 0, (struct sockaddr *) &peer_addr, peer_addr_len);
				else
					raise_error("invalid mode");



				if (wrote_bytes < read_bytes)
					raise_error("sendto() failed");
			}
			
			if (in.verbose == 1)
				printf("Read %d bytes from tun and wrote %d on socket\n", read_bytes, wrote_bytes);

		}



		/* Case where socket is ready to be read */

		if (FD_ISSET(sockfd, &r_set)) {

			/* Read the data from the network and write on tun device */
			bzero(buff, BUFF_SIZE);
			len = 0;

			/* First read the length of the packet */
			if (in.over != SOCK_DGRAM) {
				/* For TCP, life is simple.
				First few bytes will be the length of the packet. */
				read_bytes = read(sockfd, (char *) &len, sizeof(len));
				if (read_bytes < 0)
					raise_error("read() on socket failed");

				/* Now read the packet */
				read_bytes = read(sockfd, buff, ntohs(len));
				if (read_bytes < 0)
					raise_error("read() on socket failed");
			} else {
				/* UDP has to have different behaviour for client and server */
				if (in.mode == 'c')
					/* For the client, we can ignore who we got the data from */
					read_bytes = recvfrom(sockfd, buff, BUFF_SIZE, 0, NULL, NULL);
				else if (in.mode == 's') 
					/* Server has to store the address of the client to be used in a sendto() call
					later */
					read_bytes = recvfrom(sockfd, buff, BUFF_SIZE, 0, (struct sockaddr *) &peer_addr, &peer_addr_len);
				else
					raise_error("invalid mode");
	
				if (read_bytes < 0)
					raise_error("recvfrom() failed");
			}

			/* Write the packet on the tun device now */
			wrote_bytes = write(tunfd, buff, read_bytes);
			if (wrote_bytes < 0)
				raise_error("write() on tun failed");

			if (in.verbose == 1)
				printf("Read %d bytes from socket and wrote %d on tun\n", read_bytes, wrote_bytes);
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
	bzero(in.dev, BUFF_SIZE);
	bzero(in.port_str, 10);

	while ((arg = getopt(argc, argv, "vhm:s:d:p:o:")) != -1) {

		switch (arg) {
			case 'h':	print_usage();
						break;

			case 'v':	in.verbose = 1;
						break;

			case 'm':	if (strcmp(optarg,"s") == 0)
							in.mode = 's';
						else if (strcmp(optarg,"c") == 0)
							in.mode = 'c';
						else {
							fprintf(stderr,"Invalid mode of operation %s. Valid modes are 'c' and 's'\n",optarg);
							exit(1);
						}
						break;

			case 's':	strcpy(in.serv,optarg);
						break;

			case 'd':	strcpy(in.dev,optarg);
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

	if (in.mode != 'c' && in.mode != 's') {
		fprintf(stderr,"Operation mode is mandetory argument. use -m to specify the mode\n");
		exit(1);
	}

	if (in.dev[0] == 0 || in.port_str[0] == 0) {
		fprintf(stderr,"Port number and device name are mandetory arguments. use -p and -d to specify them\n");
		exit(1);
	}

	if (in.mode == 'c' && in.serv[0] == 0) {
		fprintf(stderr,"Client mode needs option -s with server name/ip as an argument\n");
		exit(1);
	}
}







/* 	
	print_usage: print the usage and exit

	input: void
	returns: exit with 0 status
*/




void print_usage () {
	
	printf("Usage: %s -m [mode] -d [device name] -p [port] -o [underlying prot] -s [server] -v\n\
	where,\n\
		-m:	mode		: either of 's' or 'c' signifying whether to act as client or server.\n\
		-d:	device name	: tun device name\n\
		-p:	port		: port number used by client and server (for listening in case of server)\n\
		-o:	protocol	: name of the underlying protocol over which tunneling happens. can be 'tcp' or 'udp'\n\
		-s:	server name	: name or ip address of the server. (Only considered in case of client)\n\
		-v: verbose		: print the info messages which may slow down the performance\n\
		-h:	help		: print this usage\n", prog_name);
	
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
