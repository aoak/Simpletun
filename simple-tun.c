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
#include <arpa/inet.h>



/* I don't know the cause of it, but include has to be done in order -

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
This sucks, but we can live with it ;)
*/


#define BUFF_SIZE 3000



char prog_name[20];



/* Structure to store the user info. Needed when we want
to create a tun device and set its owner */
struct user {
	char uname[BUFF_SIZE];					/* User (owner) name. */
	struct passwd uinfo;					/* structure populated by /etc/passwd */
};

/* Structure for storing the tun device info */
struct tun_dev {
	char device[BUFF_SIZE];					/* Device name eg. tun2 */
	int pers;								/* persistence. can be 1 or 0 */
	char ip_addr[60];						/* ip address. For future use */
};

/* Global structure storing the commandline input */
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
	struct sockaddr_storage peer_addr;		/* structure to store address of the client */
	socklen_t peer_addr_len;				/* in case of UDP server */

} in;


/* Thread function can have only void* input. Hence arguments to the thread function
are kept in a structure and the pointer is passed */
struct thread_args {
	int tun_fd;								/* Device file descriptor */
	int sock_fd;
};






int mktun (char * , int , struct ifreq * );
void settun (int , struct ifreq * , int );
void read_bytes_tun (int , struct ifreq * );
void tunnel (int, int);
void check_usage (int, char * []);
void print_usage ();
void raise_error (const char *);
int net_connect();
int server_connect ();
int client_connect ();
void * tun_to_sock (void * );
void * sock_to_tun (void * );
void setip (int fd); 







void main (int argc, char * argv[]) {

	int nfd, tfd;
	struct ifreq ifr;		/* structure having parameters for ioctl() call */

	strcpy(prog_name, argv[0]);
	in.verbose = 0;
	check_usage(argc,argv);


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




/*
	settun: This function takes a tun device descriptor, structure ifreq and
			persistence flag and sets the persistence as well as owner of the
			tun device. This eliminates the need of a tool like openvpn to create
			a pesistent tun device with given user as owner
	
	input: int <tun device descriptor>
		   struct ifreq * <pointer to ifreq structure associated with the tun>
		   int <persistence flag>. (if 1, we need to set device to persistent)
	
	returns: void. (exits on failure)
*/




void settun (int tun_fd, struct ifreq * ifr, int pers) {

	int owner = -1, group = -1;
	char * buff;
	int buffsize;
	struct passwd * s;

	/* buffer for getpwnam_r() */
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
			/* Get effective uid and gid of current user instead*/
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

	setip(tun_fd);
	
	printf("tun device: %s,",ifr->ifr_name);
	if (owner != -1)
		printf(" uid: %d,", owner);
	if (group != -1)
		printf(" gid: %d,", group);
	printf(" persistence: %d\n", pers);

	free(buff);
}






void setip (int fd) {

	struct ifreq ifr;
	struct sockaddr_in addr;
	int stat, s;

	memset(&ifr, 0, sizeof(ifr));
	memset(&addr, 0, sizeof(addr));
	strncpy(ifr.ifr_name, in.dev.device, IFNAMSIZ);

	/* Need logic to set family dynamically taking commandline arg */
	addr.sin_family = AF_INET;

	/* we need a socket descriptor for ioctl(). Cant use tun descriptor */
	s = socket(addr.sin_family, SOCK_DGRAM, 0);

	/* Convert ip to network binary */
	stat = inet_pton(addr.sin_family, in.dev.ip_addr, &addr.sin_addr);
	if (stat == 0)
		raise_error("inet_pton() - invalid ip");
	if (stat == -1)
		raise_error("inet_pton() - invalid family");

	if (stat != 1)
		raise_error("inet_pton()");
	
	ifr.ifr_addr = *(struct sockaddr *) &addr;

	/* Set ip */
	if (ioctl(s, SIOCSIFADDR, (caddr_t) &ifr) == -1)
		raise_error("ioctl() - SIOCSIFADDR");

	/* Convert mask to net binary. Need logic here to adjust it dynamically
	too */
	stat = inet_pton(addr.sin_family, "255.255.0.0", &addr.sin_addr);
	if (stat == 0)
		raise_error("inet_pton() - invalid ip");
	if (stat == -1)
		raise_error("inet_pton() - invalid family");

	if (stat != 1)
		raise_error("inet_pton()");
	
	ifr.ifr_addr = *(struct sockaddr *) &addr;

	/* Set the mask */
	if (ioctl(s, SIOCSIFNETMASK, (caddr_t) &ifr) == -1)
		raise_error("ioctl() - SIOCSIFADDR");
	
	/* Get the current flags */
	if (ioctl(s, SIOCGIFFLAGS, &ifr) == -1)
		raise_error("ioctl() - SIOCGIFFLAGS");
	strncpy(ifr.ifr_name, in.dev.device, IFNAMSIZ);
	/* Or them with UP and RUNNING flags */
	ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);

	/* Set the flags */
	if (ioctl(s, SIOCSIFFLAGS, &ifr) == -1)
		raise_error("ioctl() - SIOCSIFFLAGS");

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
	tunnel: This function is where tunnelling actually happens. We spawn two
			threads, one reads from tun and writes on socket and other reads
			from socket and writes on tun. 
			Single threaded version uses select() to juggle between two descriptors. 
			This can cause delay in read() or write() because select can return any 
			descriptor from the set. Multi-threading will ensure that the device is
			read as soon as it is ready to be read and the data is immidiately written
			on the other device. (Well, the device will serialize the reads and writes
			but that is on lower level than the API)
	
	input:	int <network socket descriptor>,
			int <tun device descriptor>
	returns: void

*/




void tunnel (int sockfd, int tunfd) {


	/* We need two threads, one for transferring data from tun to socket
	and other from socket to tun */
	pthread_t t2n, n2t;
	int ret1, ret2;

	/* Thread function can only take void pointer as an argument, so we need
	a structure to hold all the arguments and then we will pass the pointer 
	to the structure as an argument */
	struct thread_args tun_to_net, net_to_tun;
	
	net_to_tun.tun_fd = tunfd;
	net_to_tun.sock_fd = sockfd;

	tun_to_net.tun_fd = tunfd;
	tun_to_net.sock_fd = sockfd;

	printf("Starting the tunnelling threads\n");
	/* spawn the two threads */
	ret1 = pthread_create( &t2n, NULL, tun_to_sock, (void *) &tun_to_net);
	ret2 = pthread_create( &n2t, NULL, sock_to_tun, (void *) &net_to_tun);

	/* Wait for threads to join. (This is actually useless in this implementation
	cause we are running infinite loops in the threads and not catching any signals
	to exit graacefully) */
	pthread_join(t2n, NULL);
	printf("Thread tun-to-network returned\n");
	pthread_join(n2t, NULL);
	printf("Thread network-to-tun returned\n");

}




/*
	sock_to_tun: This function is ran in a thread which checks if the socket is ready
				 to be read and if it is, reads the data from socket and writes it 
				 immidiately on the tun device
	
	input: void * <pointer to the thread arguments structure>
	returns: void * (just syntactically. It will never return as it has an infinite loop)
*/



void * sock_to_tun (void * ptr) {

	struct thread_args * args;
	args = (struct thread_args *) ptr;

	/* Extract the arguments from the structure */
	int tun_fd = args->tun_fd;
	int sock_fd = args->sock_fd;
	char buff[BUFF_SIZE];
	int read_bytes, wrote_bytes, stat;
	int len;

	in.peer_addr_len = sizeof(in.peer_addr);
	fd_set r_set;

	printf("Thread %ld: Starting operations on socket\n",pthread_self());
	while (1) {
		
		/* Wait till socket is ready to be read */
		FD_ZERO(&r_set);
		FD_SET(sock_fd, &r_set);

		stat = select(sock_fd+1, &r_set, NULL, NULL, NULL);

		if (stat < 0 && errno == EINTR)
			continue;

		if (stat < 0)
			raise_error("select() failed");


		if (FD_ISSET(sock_fd, &r_set)) {

			/* Socket is ready to be read. So read the data into buffer */
			stat = 0;
			len = 0;
			read_bytes = 0;
			wrote_bytes = 0;

			bzero(buff, BUFF_SIZE);

			if (in.over == SOCK_DGRAM) {

				/* In case of UDP, we need to read the data using recvfrom() call */
				if (in.mode == 'c')
					/* In case of UDP client, we already have server's address, so no sweat */
					read_bytes = recvfrom(sock_fd, buff, BUFF_SIZE, 0, NULL, NULL);
				else if (in.mode == 's')
					/* In case of UDP server, we need to store the address of client in a structure
					so that we can use it to send a response. We are assuming that server is always
					contacted by the client first */
					read_bytes = recvfrom(sock_fd, buff, BUFF_SIZE, 0, (struct sockaddr *) &in.peer_addr, &in.peer_addr_len);
				else
					raise_error("invalid mode");

				if (read_bytes < 0)
					raise_error("recvfrom() failed");


			} else {

				/* In case of TCP, we read the length of the packet first so that we
				can read the exact packet */
				read_bytes = read(sock_fd, (char *) &len, sizeof(len));
				if (read_bytes < 0)
					raise_error("read() failed on socket");

				/* Now read the packet */
				read_bytes = read(sock_fd, buff, ntohs(len));
				if (read_bytes < 0)
					raise_error("read() failed on socket");
			}

			/* Write what we read from socket onto the tun device */
			wrote_bytes = write(tun_fd, buff, read_bytes);
			if (wrote_bytes < read_bytes)
				raise_error("write() failed on tun");

			if (in.verbose == 1)
				printf("Read %d bytes on socket and wrote %d on tun\n",read_bytes, wrote_bytes);

		}
	}
}




/*
	tun_to_sock: This function is ran in a thread which checks if the tun device is ready
				 to be read and if it is, reads the data from tun and writes it 
				 immidiately on the socket 
	
	input: void * <pointer to the thread arguments structure>
	returns: void * (just syntactically. It will never return as it has an infinite loop)
*/


void * tun_to_sock (void * ptr) {

	struct thread_args * args;
	args = (struct thread_args *) ptr;

	/* Extract the arguments from structure */
	int tun_fd = args->tun_fd;
	int sock_fd = args->sock_fd;
	char buff[BUFF_SIZE];
	int stat, read_bytes, wrote_bytes;
	int len = 0;

	fd_set r_set;

	printf("Thread %ld: Starting operations on tun\n",pthread_self());
	while (1) {
		
		/* Wait till tun device is ready to be read, if it is, read the data
		into a buffer */
		FD_ZERO(&r_set);
		FD_SET(tun_fd, &r_set);
		stat = select(tun_fd+1, &r_set, NULL, NULL, NULL);

		if (stat < 0 && errno == EINTR)
			continue;

		if (stat < 0)
			raise_error("select() failed");


		if (FD_ISSET(tun_fd, &r_set)) {

			/* If the tun is ready to be read, read the data in a buffer */
			wrote_bytes = 0;
			stat = 0;
			read_bytes = 0;

			bzero(buff, BUFF_SIZE);

			read_bytes = read(tun_fd, buff, BUFF_SIZE);
			if (read_bytes < 0)
				raise_error("tun_io - read() failed");

			if (in.over == SOCK_DGRAM) {
				/* In case of UDP, we need to use sendto() call */
				if (in.mode == 'c')
					/* UDP client already knows server address */
					wrote_bytes = sendto(sock_fd, buff, read_bytes, 0, in.serv_ptr->ai_addr, in.serv_ptr->ai_addrlen);
				else if (in.mode == 's')
					/* UDP server has to use the address in the structure peer_addr. This must be populated
					by the recvfrom() call (hopefully) */
					wrote_bytes = sendto(sock_fd, buff, read_bytes, 0, (struct sockaddr *) &in.peer_addr, in.peer_addr_len);
				else
					raise_error("invalid mode");

			} else {

				/* In case of TCP, we first write the length of the packet on the socket and then 
				write the actual packet. This will help the receiver to find out packet boundries
				which is otherwise difficult as TCP makes the data appear as a stream */
				len = htons(read_bytes);
				wrote_bytes = write(sock_fd, &len, sizeof(len));
			}

			if (wrote_bytes < read_bytes)
				raise_error("write on socket failed");

			if (in.verbose == 1)
				printf("Read %d bytes on tun and wrote %d on scoket\n", read_bytes, wrote_bytes);
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
	

	while ((arg = getopt(argc, argv, "evhm:s:d:p:o:u:i:")) != -1) {

		switch (arg) {
			/* help */
			case 'h':	print_usage();
						break;

			/* persistence flag */
			case 'e':	in.dev.pers = 1;
						break;

			/* user name to be set as owner */
			case 'u':	strcpy(in.usr.uname, optarg);
						break;	

			/* verbosity flag */
			case 'v':	in.verbose = 1;
						break;

			case 'i':	strcpy(in.dev.ip_addr, optarg);
						break;

			/* Mode. This can be either of the three:
				'm': 'm'ake the device and set persistence and owner. This 
					 is not a tunnelling mode.
				'c': 'c'lient. Tunnelling mode client who initiates the communication
				's': 's'erver. Tunnelling mode server who waits for the communication */
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

			/* server name or ip. To be given in client tunnelling mode */
			case 's':	strcpy(in.serv,optarg);
						break;

			/* tun device name */
			case 'd':	strcpy(in.dev.device,optarg);
						break;

			/* port number used by server and client */
			case 'p':	strcpy(in.port_str,optarg);
						in.port = atoi(in.port_str);
						break;
			
			/* the transport layer protocol in tunnel over which communication happens */
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


	/* Check if mode is correct */
	if (in.mode != 'c' && in.mode != 's' && in.mode != 'm') {
		fprintf(stderr,"Operation mode is mandetory argument. use -m to specify the mode\n");
		exit(1);
	}

	/* All the modes need a device name */
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

	/* prepare for getaddrinfo() */
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
	
	/* Now try to create a socket and bind to it. */
	for (m = myptr; m != NULL; m = m->ai_next) {
		servsock = socket(m->ai_family, m->ai_socktype, m->ai_protocol);

		if (servsock == -1)
			continue;

		if (bind(servsock, m->ai_addr, m->ai_addrlen) == 0)
			break;

		close(servsock);
	}

	/* if m is NULL, that means we broke out of for loop because we exausted
	the linked list and not because bind() succeeded. Exit in hhat case */
	if (m == NULL)
		raise_error("bind()");

	/* If its not UDP, we have to do listen() and accept() */
	if (in.over != SOCK_DGRAM) {
		listen(servsock, 5);
		sock = accept(servsock, NULL, NULL);
		if (sock < 0)
			raise_error("accept()");
		/* In case of TCP, return descriptor of socket after accept() */	
		return sock;

	} else {
		/* In case of UDP, the socket we did bind() with is the one */
		return servsock;
	}
}



