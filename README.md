Simpletun
=========

This is a simple tunnelling program written to test tunnelling of IPv4 and IPv6 over IPv4. It can use TCP or UDP as underlying protocol over which tunnelling happens.
It can also create a persistent tun device and set its owner.

Although the code is written by me, I have referred to and am influenced by article and code snippets on http://backreference.org/2010/03/26/tuntap-interface-tutorial/ 


simple-tun.c
-------------


This program operates in three modes, the 'make' mode is for creation of tun device and setting its owner
to specific user. This tun device can be then used by subsequent invokations for tunnelling.

The 'tunnelling' mode has two modes of operation viz. server and client. The client connects to the 
server program on remote machine using TCP or UDP over the network. Then the client and servers read 
the data from tun device and write it on network and read data from network and write it on tun device. 

This creates a tunnel between the two tun devices which can be used by other programs. Here is a 
snippet how it can be used.

	# local machine (where client runs)

	$ ifconfig
	tun3      Link encap:UNSPEC  HWaddr 00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00  
	inet6 addr: aaaa::bbba/112 Scope:Global
	UP POINTOPOINT RUNNING NOARP MULTICAST  MTU:1500  Metric:1


	# remote machine (where server runs)
	$ ifconfig 
	tun3      Link encap:UNSPEC  HWaddr 00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00  
	inet6 addr: aaaa::bbbb/112 Scope:Global
	UP POINTOPOINT RUNNING NOARP MULTICAST  MTU:1500  Metric:1


	# local machine
	$ ping6 aaaa::bbbb
	PING aaaa::bbbb(aaaa::bbbb) 56 data bytes
	64 bytes from aaaa::bbbb: icmp_seq=1 ttl=64 time=1.12 ms
	64 bytes from aaaa::bbbb: icmp_seq=2 ttl=64 time=0.159 ms
	64 bytes from aaaa::bbbb: icmp_seq=3 ttl=64 time=0.138 ms
	64 bytes from aaaa::bbbb: icmp_seq=4 ttl=64 time=0.139 ms
	^C
	--- aaaa::bbbb ping statistics ---
	4 packets transmitted, 4 received, 0% packet loss, time 3971ms
	rtt min/avg/max/mdev = 0.138/0.391/1.129/0.426 ms
	$ 


For creation of the tun device, the same program can be used as follows:

	$ sudo ./simple-tun -m m -e -d tun5 -u aniket
	Created tun device(3): tun5
	tun device: tun5, uid: 1000, gid: 1000, persistence: 1

This will create a tun device which can be then used for tunnelling.
Note that for this version, a tun device ip address needs to be setup using following commands. 

	# ip link set
	# ip addr add

The program cannot add and set ip addresses for you (yet).

Usage of the program is as follows:

	Usage: ./simple-tun -m [mode] -d [device name] -p [port] -o [underlying prot] -s [server] -v 
						-m [mode] -d [device name] -e -u [user]
			where,
				-m:	mode		: either of 's' or 'c' signifying whether to act as client or server while tunnelling,
									OR
								  can be 'm' which tells the program that it is supposed to create new tun device.
				-d:	device name	: tun device name
				-p:	port		: port number used by client and server for tunnelling (for listening in case of server).
								  only significant in case mode isn't 'm'
				-o:	protocol	: name of the underlying protocol over which tunneling happens. can be 'tcp' or 'udp'
								  only significant in case mode isn't 'm'
				-s:	server name	: name or ip address of the server. (Only considered in case of client)
								  only significant in case mode isn't 'm'
				-e: persistence : Whether to set device persistent or not
								  only significant in case mode is 'm'
				-u: user		: User to set as owner of the device
								  only significant in case mode is 'm'
				-v: verbose		: print the info messages which may slow down the performance
				-h:	help		: print this usage
