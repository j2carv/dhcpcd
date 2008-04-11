/* 
 * dhcpcd - DHCP client daemon
 * Copyright 2006-2008 Roy Marples <roy@marples.name>
 * All rights reserved

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <arpa/inet.h>
#include <netinet/in_systm.h>
#ifdef __linux__
#include <netinet/ether.h>
#include <netpacket/packet.h>
#endif
#include <netinet/in.h>
#include <netinet/ip.h>
#define __FAVOR_BSD /* Nasty glibc hack so we can use BSD semantics for UDP */
#include <netinet/udp.h>
#undef __FAVOR_BSD
#include <arpa/inet.h>
#ifdef AF_LINK
# include <net/if_dl.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "common.h"
#include "dhcp.h"
#include "logger.h"
#include "net.h"
#include "signal.h"

int
inet_ntocidr(struct in_addr address)
{
	int cidr = 0;
	uint32_t mask = htonl(address.s_addr);

	while (mask) {
		cidr++;
		mask <<= 1;
	}

	return cidr;
}

int
inet_cidrtoaddr (int cidr, struct in_addr *addr)
{
	int ocets;

	if (cidr < 0 || cidr > 32) {
		errno = EINVAL;
		return -1;
	}
	ocets = (cidr + 7) / 8;

	addr->s_addr = 0;
	if (ocets > 0) {
		memset(&addr->s_addr, 255, (size_t)ocets - 1);
		memset((unsigned char *)&addr->s_addr + (ocets - 1),
		       (256 - (1 << (32 - cidr) % 8)), 1);
	}

	return 0;
}

uint32_t
get_netmask(uint32_t addr)
{
	uint32_t dst;

	if (addr == 0)
		return 0;

	dst = htonl(addr);
	if (IN_CLASSA(dst))
		return ntohl(IN_CLASSA_NET);
	if (IN_CLASSB (dst))
		return ntohl(IN_CLASSB_NET);
	if (IN_CLASSC (dst))
		return ntohl(IN_CLASSC_NET);

	return 0;
}

char *
hwaddr_ntoa(const unsigned char *hwaddr, size_t hwlen)
{
	static char buffer[(HWADDR_LEN * 3) + 1];
	char *p = buffer;
	size_t i;

	for (i = 0; i < hwlen && i < HWADDR_LEN; i++) {
		if (i > 0)
			*p ++= ':';
		p += snprintf(p, 3, "%.2x", hwaddr[i]);
	}

	*p ++= '\0';

	return buffer;
}

size_t
hwaddr_aton(unsigned char *buffer, const char *addr)
{
	char c[3];
	const char *p = addr;
	unsigned char *bp = buffer;
	size_t len = 0;

	c[2] = '\0';
	while (*p) {
		c[0] = *p++;
		c[1] = *p++;
		/* Ensure that next data is EOL or a seperator with data */
		if (!(*p == '\0' || (*p == ':' && *(p + 1) != '\0'))) {
			errno = EINVAL;
			return 0;
		}
		/* Ensure that digits are hex */
		if (isxdigit ((int)c[0]) == 0 || isxdigit((int)c[1]) == 0) {
			errno = EINVAL;
			return 0;
		}
		p++;
		if (bp)
			*bp++ = (unsigned char)strtol(c, NULL, 16);
		else
			len++;
	}

	if (bp)
		return bp - buffer;
	return len;
}

int
do_interface(const char *ifname,
	     _unused unsigned char *hwaddr, _unused size_t *hwlen,
	     struct in_addr *addr, struct in_addr *net, int get)
{
	int s;
	struct ifconf ifc;
	int retval = 0;
	int len = 10 * sizeof(struct ifreq);
	int lastlen = 0;
	char *p;
	union {
		char *buffer;
		struct ifreq *ifr;
	} ifreqs;
	struct sockaddr_in address;
	struct ifreq *ifr;
	struct sockaddr_in netmask;

#ifdef AF_LINK
	struct sockaddr_dl sdl;
#endif

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
		return -1;

	/* Not all implementations return the needed buffer size for
	 * SIOGIFCONF so we loop like so for all until it works */
	memset(&ifc, 0, sizeof(ifc));
	for (;;) {
		ifc.ifc_len = len;
		ifc.ifc_buf = xmalloc((size_t)len);
		if (ioctl(s, SIOCGIFCONF, &ifc) == -1) {
			if (errno != EINVAL || lastlen != 0) {
				close(s);
				free(ifc.ifc_buf);	
				return -1;
			}
		} else {
			if (ifc.ifc_len == lastlen)
				break;
			lastlen = ifc.ifc_len;
		}

		free(ifc.ifc_buf);
		ifc.ifc_buf = NULL;
		len *= 2;
	}

	for (p = ifc.ifc_buf; p < ifc.ifc_buf + ifc.ifc_len;) {
		/* Cast the ifc buffer to an ifreq cleanly */
		ifreqs.buffer = p;
		ifr = ifreqs.ifr;

#ifndef __linux__
		if (ifr->ifr_addr.sa_len > sizeof(ifr->ifr_ifru))
			p += offsetof(struct ifreq, ifr_ifru) +
				ifr->ifr_addr.sa_len;
		else
#endif
			p += sizeof(*ifr);

		if (strcmp(ifname, ifr->ifr_name) != 0)
			continue;

#ifdef AF_LINK
		if (hwaddr && hwlen && ifr->ifr_addr.sa_family == AF_LINK) {
			memcpy(&sdl, &ifr->ifr_addr, sizeof(sdl));
			*hwlen = sdl.sdl_alen;
			memcpy(hwaddr, sdl.sdl_data + sdl.sdl_nlen,
			       (size_t)sdl.sdl_alen);
			retval = 1;
			break;
		}
#endif

		if (ifr->ifr_addr.sa_family == AF_INET)	{
			memcpy(&address, &ifr->ifr_addr, sizeof(address));
			if (ioctl(s, SIOCGIFNETMASK, ifr) == -1)
				continue;
			memcpy(&netmask, &ifr->ifr_addr, sizeof(netmask));
			if (get) {
				addr->s_addr = address.sin_addr.s_addr;
				net->s_addr = netmask.sin_addr.s_addr;
				retval = 1;
				break;
			} else {
				if (address.sin_addr.s_addr == addr->s_addr &&
				    (!net ||
				     netmask.sin_addr.s_addr == net->s_addr))
				{
					retval = 1;
					break;
				}
			}
		}

	}

	close(s);
	free(ifc.ifc_buf);
	return retval;
}

struct interface *
read_interface(const char *ifname, _unused int metric)
{
	int s;
	struct ifreq ifr;
	struct interface *iface = NULL;
	unsigned char *hwaddr = NULL;
	size_t hwlen = 0;
	sa_family_t family = 0;
	unsigned short mtu;
#ifdef __linux__
	char *p;
#endif

	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
		return NULL;

#ifdef __linux__
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(s, SIOCGIFHWADDR, &ifr) == -1)
		goto eexit;

	switch (ifr.ifr_hwaddr.sa_family) {
	case ARPHRD_ETHER:
	case ARPHRD_IEEE802:
		hwlen = ETHER_ADDR_LEN;
		break;
	case ARPHRD_IEEE1394:
		hwlen = EUI64_ADDR_LEN;
	case ARPHRD_INFINIBAND:
		hwlen = INFINIBAND_ADDR_LEN;
		break;
	}

	hwaddr = xmalloc(sizeof(unsigned char) * HWADDR_LEN);
	memcpy(hwaddr, ifr.ifr_hwaddr.sa_data, hwlen);
	family = ifr.ifr_hwaddr.sa_family;
#else
	ifr.ifr_metric = metric;
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(s, SIOCSIFMETRIC, &ifr) == -1)
		goto eexit;

	hwaddr = xmalloc(sizeof(unsigned char) * HWADDR_LEN);
	if (do_interface(ifname, hwaddr, &hwlen, NULL, NULL, 0) != 1)
		goto eexit;

	family = ARPHRD_ETHER;
#endif

	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(s, SIOCGIFMTU, &ifr) == -1)
		goto eexit;

	/* Ensure that the MTU is big enough for DHCP */
	if (ifr.ifr_mtu < MTU_MIN) {
		ifr.ifr_mtu = MTU_MIN;
		strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
		if (ioctl(s, SIOCSIFMTU, &ifr) == -1)
			goto eexit;
	}
	mtu = ifr.ifr_mtu;

	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
#ifdef __linux__
	/* We can only bring the real interface up */
	if ((p = strchr(ifr.ifr_name, ':')))
		*p = '\0';
#endif
	if (ioctl(s, SIOCGIFFLAGS, &ifr) == -1)
		goto eexit;

	if (!(ifr.ifr_flags & IFF_UP) || !(ifr.ifr_flags & IFF_RUNNING)) {
		ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
		if (ioctl(s, SIOCSIFFLAGS, &ifr) != 0)
			goto eexit;
	}

	iface = xzalloc(sizeof(*iface));
	strlcpy(iface->name, ifname, IF_NAMESIZE);
	snprintf(iface->leasefile, PATH_MAX, LEASEFILE, ifname);
#ifdef ENABLE_INFO
	snprintf(iface->infofile, PATH_MAX, INFOFILE, ifname);
#endif
	memcpy(&iface->hwaddr, hwaddr, hwlen);
	iface->hwlen = hwlen;

	iface->family = family;
	iface->arpable = !(ifr.ifr_flags & (IFF_NOARP | IFF_LOOPBACK));
	iface->mtu = iface->initial_mtu = mtu;

	/* 0 is a valid fd, so init to -1 */
	iface->fd = -1;
	iface->udp_fd = -1;

eexit:
	close(s);
	free(hwaddr);
	return iface;
}

int
do_mtu(const char *ifname, short int mtu)
{
	struct ifreq ifr;
	int r;
	int s;

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
		return -1;

	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	ifr.ifr_mtu = mtu;
	r = ioctl(s, mtu ? SIOCSIFMTU : SIOCGIFMTU, &ifr);
	close(s);
	if (r == -1)
		return -1;
	return ifr.ifr_mtu;
}

void
free_routes(struct rt *routes)
{
	struct rt *r;

	while (routes) {
		r = routes->next;
		free(routes);
		routes = r;
	}
}

int
open_udp_socket(struct interface *iface)
{
        int s;
        union sockunion {
                struct sockaddr sa;
                struct sockaddr_in sin;
        } su;
        int n = 1;

        if ((s = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
                return -1;

        memset(&su, 0, sizeof(su));
        su.sin.sin_family = AF_INET;
        su.sin.sin_port = htons(DHCP_CLIENT_PORT);
	su.sin.sin_addr.s_addr = iface->addr.s_addr;
        if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &n, sizeof(n)) == -1)
                goto eexit;
	/* As we don't actually use this socket for anything, set
	 * the receiver buffer to 1 */
        if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n)) == -1)
                goto eexit;
        if (bind(s, &su.sa, sizeof(su)) == -1)
                goto eexit;

        iface->udp_fd = s;
        close_on_exec(s);
        return 0;

eexit:
        close(s);
        return -1;
}

struct udp_dhcp_packet
{
	struct ip ip;
	struct udphdr udp;
	struct dhcp_message dhcp;
};

static uint16_t
checksum(uint8_t *addr, uint16_t len)
{
	uint32_t sum = 0;
	union
	{
		uint8_t *addr;
		uint16_t *i;
	} p;
	uint16_t nleft = len;
	uint8_t a = 0;

	p.addr = addr;
	while (nleft > 1) {
		sum += *p.i++;
		nleft -= 2;
	}

	if (nleft == 1) {
		memcpy(&a, p.i, 1);
		sum += ntohs(a) << 8;
	}

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);

	return ~sum;
}

ssize_t
make_udp_packet(uint8_t **packet, const uint8_t *data, size_t length,
		struct in_addr source, struct in_addr dest)
{
	struct udp_dhcp_packet *udpp;
	struct ip *ip;
	struct udphdr *udp;

	udpp = xzalloc(sizeof(*udpp));
	ip = &udpp->ip;
	udp = &udpp->udp;

	/* OK, this is important :)
	 * We copy the data to our packet and then create a small part of the
	 * ip structure and an invalid ip_len (basically udp length).
	 * We then fill the udp structure and put the checksum
	 * of the whole packet into the udp checksum.
	 * Finally we complete the ip structure and ip checksum.
	 * If we don't do the ordering like so then the udp checksum will be
	 * broken, so find another way of doing it! */

	memcpy(&udpp->dhcp, data, length);

	ip->ip_p = IPPROTO_UDP;
	ip->ip_src.s_addr = source.s_addr;
	if (dest.s_addr == 0)
		ip->ip_dst.s_addr = INADDR_BROADCAST;
	else
		ip->ip_dst.s_addr = dest.s_addr;

	udp->uh_sport = htons(DHCP_CLIENT_PORT);
	udp->uh_dport = htons(DHCP_SERVER_PORT);
	udp->uh_ulen = htons(sizeof(*udp) + length);
	ip->ip_len = udp->uh_ulen;
	udp->uh_sum = checksum((uint8_t *)udpp, sizeof(*udpp));

	ip->ip_v = IPVERSION;
	ip->ip_hl = 5;
	ip->ip_id = 0;
	ip->ip_tos = IPTOS_LOWDELAY;
	ip->ip_len = htons (sizeof(*ip) + sizeof(*udp) + length);
	ip->ip_id = 0;
	ip->ip_off = htons(IP_DF); /* Don't fragment */
	ip->ip_ttl = IPDEFTTL;

	ip->ip_sum = checksum((uint8_t *)ip, sizeof(*ip));

	*packet = (uint8_t *)udpp;
	return sizeof(*ip) + sizeof(*udp) + length;
}

ssize_t
get_udp_data(const uint8_t **data, const uint8_t *udp)
{
	union
	{
		const uint8_t *data;
		const struct udp_dhcp_packet *packet;
	} d;

	d.data = udp;
	*data = (const uint8_t *)&d.packet->dhcp;
	return ntohs(d.packet->ip.ip_len) -
		sizeof(d.packet->ip) -
		sizeof(d.packet->udp);
}

int
valid_udp_packet(uint8_t *data)
{
	union
	{
		uint8_t *data;
		struct udp_dhcp_packet *packet;
	} d;
	uint16_t bytes;
	uint16_t ipsum;
	uint16_t iplen;
	uint16_t udpsum;
	struct in_addr source;
	struct in_addr dest;
	int retval = 0;

	d.data = data;
	bytes = ntohs(d.packet->ip.ip_len);
	ipsum = d.packet->ip.ip_sum;
	iplen = d.packet->ip.ip_len;
	udpsum = d.packet->udp.uh_sum;

	d.data = data;
	d.packet->ip.ip_sum = 0;
	if (ipsum != checksum((uint8_t *)&d.packet->ip, sizeof(d.packet->ip))) {
		errno = EINVAL;
		retval = -1;
		goto eexit;
	}

	memcpy(&source, &d.packet->ip.ip_src, sizeof(d.packet->ip.ip_src));
	memcpy(&dest, &d.packet->ip.ip_dst, sizeof(d.packet->ip.ip_dst));
	memset(&d.packet->ip, 0, sizeof(d.packet->ip));
	d.packet->udp.uh_sum = 0;

	d.packet->ip.ip_p = IPPROTO_UDP;
	memcpy(&d.packet->ip.ip_src, &source, sizeof(d.packet->ip.ip_src));
	memcpy(&d.packet->ip.ip_dst, &dest, sizeof(d.packet->ip.ip_dst));
	d.packet->ip.ip_len = d.packet->udp.uh_ulen;
	if (udpsum && udpsum != checksum(d.data, bytes)) {
		errno = EINVAL;
		retval = -1;
	}

eexit:
	d.packet->ip.ip_sum = ipsum;
	d.packet->ip.ip_len = iplen;
	d.packet->udp.uh_sum = udpsum;

	return retval;
}

#ifdef ENABLE_ARP
/* These are really for IPV4LL */
#define NPROBES                 3
#define PROBE_INTERVAL          200
#define NCLAIMS                 2
#define CLAIM_INTERVAL          200

/* Linux does not seem to define these handy macros */
#ifndef ar_sha
#define ar_sha(ap) (((caddr_t)((ap) + 1)) + 0)
#define ar_spa(ap) (((caddr_t)((ap) + 1)) + (ap)->ar_hln)
#define ar_tha(ap) (((caddr_t)((ap) + 1)) + (ap)->ar_hln + (ap)->ar_pln)
#define ar_tpa(ap) (((caddr_t)((ap) + 1)) + 2 * (ap)->ar_hln + (ap)->ar_pln)
#endif

#ifndef arphdr_len
#define arphdr_len2(ar_hln, ar_pln) (sizeof(struct arphdr) + \
				     2 * (ar_hln) + 2 * (ar_pln))
#define arphdr_len(ap) (arphdr_len2((ap)->ar_hln, (ap)->ar_pln))
#endif

static int
send_arp(const struct interface *iface, int op, struct in_addr sip,
	 const unsigned char *taddr, struct in_addr tip)
{
	struct arphdr *arp;
	size_t arpsize = arphdr_len2(iface->hwlen, sizeof(sip));
	caddr_t tha;
	int retval;

	arp = xzalloc(arpsize);
	arp->ar_hrd = htons(iface->family);
	arp->ar_pro = htons(ETHERTYPE_IP);
	arp->ar_hln = iface->hwlen;
	arp->ar_pln = sizeof(sip);
	arp->ar_op = htons(op);
	memcpy(ar_sha(arp), iface->hwaddr, (size_t)arp->ar_hln);
	memcpy(ar_spa(arp), &sip, (size_t)arp->ar_pln);
	if (taddr) {
		/* NetBSD can return NULL from ar_tha, which is probably wrong
		 * but we still need to deal with it */
		if (! (tha = ar_tha(arp))) {
			free(arp);
			errno = EINVAL;
			return -1;
		}
		memcpy(tha, taddr, (size_t)arp->ar_hln);
	}
	memcpy(ar_tpa(arp), &tip, (size_t)arp->ar_pln);

	retval = send_packet(iface, ETHERTYPE_ARP,
			     (unsigned char *) arp, arphdr_len(arp));
	if (retval == -1)
		logger(LOG_ERR,"send_packet: %s", strerror(errno));
	free(arp);
	return retval;
}

int
arp_claim(struct interface *iface, struct in_addr address)
{
	struct arphdr *reply = NULL;
	long timeout = 0;
	unsigned char *buffer;
	int retval = -1;
	int nprobes = 0;
	int nclaims = 0;
	struct in_addr null_address;
	struct pollfd fds[] = {
		{ -1, POLLIN, 0 },
		{ -1, POLLIN, 0 }
	};
	ssize_t bufpos = 0;
	ssize_t buflen = iface->buffer_length;
	int bytes;
	int s = 0;
	struct timeval stopat;
	struct timeval now;
	union {
		unsigned char *c;
		struct in_addr *a;
	} rp;
	union {
		unsigned char *c;
		struct ether_addr *a;
	} rh;

	if (!iface->arpable) {
		logger(LOG_DEBUG, "interface `%s' is not ARPable", iface->name);
		return 0;
	}

	if (!IN_LINKLOCAL(ntohl(iface->addr.s_addr)) &&
	    !IN_LINKLOCAL(ntohl(address.s_addr)))
		logger(LOG_INFO,
		       "checking %s is available on attached networks",
		       inet_ntoa(address));

	if (!open_socket(iface, ETHERTYPE_ARP)) {
		logger (LOG_ERR, "open_socket: %s", strerror(errno));
		return -1;
	}

	fds[0].fd = signal_fd();
	fds[1].fd = iface->fd;
	memset(&null_address, 0, sizeof(null_address));
	buffer = xmalloc(iface->buffer_length);
	reply = xmalloc(iface->buffer_length);

	for (;;) {
		bufpos = 0;
		buflen = iface->buffer_length;
		s = 0;

		/* Only poll if we have a timeout */
		if (timeout > 0) {
			s = poll(fds, 2, timeout);
			if (s == -1) {
				if (errno == EINTR) {
					if (signal_exists(NULL) == -1) {
						errno = 0;
						continue;
					} else
						break;
				}

				logger(LOG_ERR, "poll: `%s'", strerror(errno));
				break;
			}
		}

		/* Timed out */
		if (s == 0) {
			if (nprobes < NPROBES) {
				nprobes ++;
				timeout = PROBE_INTERVAL;
				logger(LOG_DEBUG, "sending ARP probe #%d",
				       nprobes);
				if (send_arp(iface, ARPOP_REQUEST,
					     null_address, NULL,
					     address) == -1)
					break;

				/* IEEE1394 cannot set ARP target address
				 * according to RFC2734 */
				if (nprobes >= NPROBES &&
				    iface->family == ARPHRD_IEEE1394)
					nclaims = NCLAIMS;
			} else if (nclaims < NCLAIMS) {
				nclaims ++;
				timeout = CLAIM_INTERVAL;
				logger(LOG_DEBUG, "sending ARP claim #%d",
				       nclaims);
				if (send_arp(iface, ARPOP_REQUEST,
					     address, iface->hwaddr,
					     address) == -1)
					break;
			} else {
				/* No replies, so done */
				retval = 0;
				break;
			}

			/* Setup our stop time */
			if (get_time(&stopat) != 0)
				break;
			stopat.tv_usec += timeout;

			continue;
		}

		/* We maybe ARP flooded, so check our time */
		if (get_time(&now) != 0)
			break;
		if (timercmp(&now, &stopat, >)) {
			timeout = 0;
			continue;
		}

		if (!(fds[1].revents & POLLIN))
			continue;

		memset(buffer, 0, buflen);
		do {
			memset(reply, 0, iface->buffer_length);
			if ((bytes = get_packet(iface, (unsigned char *) reply,
						buffer,
						&buflen, &bufpos)) == -1)
				break;

			/* Only these types are recognised */
			if (reply->ar_op != htons(ARPOP_REPLY))
				continue;

			/* Protocol must be IP. */
			if (reply->ar_pro != htons(ETHERTYPE_IP))
				continue;
			if (reply->ar_pln != sizeof(address))
				continue;
			if ((unsigned)bytes < sizeof(reply) +
			    2 * (4 +reply->ar_hln))
				continue;

			rp.c = (unsigned char *)ar_spa(reply);
			rh.c = (unsigned char *)ar_sha(reply);

			/* Ensure the ARP reply is for the our address */
			if (rp.a->s_addr != address.s_addr)
				continue;

			/* Some systems send a reply back from our hwaddress,
			 * which is wierd */
			if (reply->ar_hln == iface->hwlen &&
			    memcmp(rh.c, iface->hwaddr, iface->hwlen) == 0)
				continue;

			logger(LOG_ERR, "ARPOP_REPLY received from %s (%s)",
			       inet_ntoa(*rp.a),
			       hwaddr_ntoa(rh.c, (size_t)reply->ar_hln));
			retval = -1;
			goto eexit;
		} while (bufpos != 0);
	}

eexit:
	close(iface->fd);
	iface->fd = -1;
	free(buffer);
	free(reply);
	return retval;
}
#endif