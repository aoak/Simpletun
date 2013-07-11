Simpletun
=========

This is a simple tunnelling program written to test tunnelling of IPv4 and IPv6 over IPv4. It can use TCP or UDP as underlying protocol over which tunnelling happens.

Although the code is written by me, I have referred to and am influenced by article and code snippets on http://backreference.org/2010/03/26/tuntap-interface-tutorial/ 


simple-tun.c
-------------


This program operates in two modes, viz. server and client. The client connects to the server program
on remote machine using TCP or UDP over the network. Then the client and servers read the data from
tun device and write it on network and read data from network and write it on tun device. 

This creates a tunnel between the two tun devices which can be used by other programs. Here is a 
snippet how it can be used.

	# local machine (where client runs)

	[aoak12@aoak12-2 ~]$ ifconfig
	tun3      Link encap:UNSPEC  HWaddr 00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00  
	inet6 addr: aaaa::bbba/112 Scope:Global
	UP POINTOPOINT RUNNING NOARP MULTICAST  MTU:1500  Metric:1


	# remote machine (where server runs)
	[aoak12@aoak12-1 ~]$ ifconfig 
	tun3      Link encap:UNSPEC  HWaddr 00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00  
	inet6 addr: aaaa::bbbb/112 Scope:Global
	UP POINTOPOINT RUNNING NOARP MULTICAST  MTU:1500  Metric:1


	# local machine
	[aoak12@aoak12-2 ~]$ ping6 aaaa::bbbb
	PING aaaa::bbbb(aaaa::bbbb) 56 data bytes
	64 bytes from aaaa::bbbb: icmp_seq=1 ttl=64 time=1.12 ms
	64 bytes from aaaa::bbbb: icmp_seq=2 ttl=64 time=0.159 ms
	64 bytes from aaaa::bbbb: icmp_seq=3 ttl=64 time=0.138 ms
	64 bytes from aaaa::bbbb: icmp_seq=4 ttl=64 time=0.139 ms
	^C
	--- aaaa::bbbb ping statistics ---
	4 packets transmitted, 4 received, 0% packet loss, time 3971ms
	rtt min/avg/max/mdev = 0.138/0.391/1.129/0.426 ms
	[aoak12@aoak12-2 ~]$ 


Note that for this version, a tun device has to be already setup using following commands. 

	ip link set
	ip addr add

The program cannot add and set ip addresses for you (yet).

Usage of the program is as follows:

	Usage: ./simple-tun -m [mode] -d [device name] -p [port] -o [underlying prot] -s [server]
	where,
		-m:	mode		: either of 's' or 'c' signifying whether to act as client or server.
		-d:	device name	: tun device name
		-p:	port		: port number used by client and server (for listening in case of server)
		-o:	protocol	: name of the underlying protocol over which tunneling happens. can be 'tcp' or 'udp'
		-s:	server name	: name or ip address of the server. (Only considered in case of client)
		-h:	help		: print this usage

