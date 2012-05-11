#ifndef _nat64_filtering_n_updating_h
#define _nat64_filtering_n_updating_h

#include <linux/string.h>
#include <linux/inet.h>
#include <linux/in6.h>

#define UDP_DEFAULT_ 5*60
#define ICMP_DEFAULT_ 1*60
#define BIB_ICMP 3
#define	NUM_EXPIRY_QUEUES 5

#ifndef _NF_NAT64_IPV4_POOL_H
#include "nf_nat64_ipv4_pool.h"
#endif

enum expiry_type {
	UDP_DEFAULT = 0,
	TCP_TRANS,
	TCP_EST,
	TCP_INCOMING_SYN,
	ICMP_DEFAULT
};

enum state_type {
	CLOSED = 0,
	V6_SYN_RCV,
	V4_SYN_RCV,
	FOUR_MIN,
	ESTABLISHED,
	V6_FIN_RCV,
	V4_FIN_RCV,
	V6_FIN_V4_FIN,
};

struct expiry_q
{
	struct list_head	queue;
	int			timeout;
};

struct nat64_bib_entry
{
	struct hlist_node	byremote;
	struct hlist_node	bylocal;

	int			type;
	struct in6_addr		remote6_addr; // X' addr
	__be32			local4_addr; // T' addr

	__be16			remote6_port; // x port
	__be16			local4_port; // t port

	struct list_head	sessions;
};

struct nat64_st_entry
{
	struct list_head	list;
	struct list_head	byexpiry;
	struct in6_addr		remote6_addr; // X' addr
	struct in6_addr		embedded6_addr; // Y' addr
	unsigned long		expires;
	int			state;
	__be32			local4_addr; // T' addr
	__be32			remote4_addr; // Z' addr
	__be16			remote6_port; // x port
	__be16			embedded6_port; // y port
	__be16			remote4_port; // z port
	__be16			local4_port; // t port
};

extern struct expiry_q expiry_base[NUM_EXPIRY_QUEUES];
extern struct kmem_cache *st_cache;
extern struct kmem_cache *st_cacheTCP;
extern struct kmem_cache *bib_cache;
extern struct kmem_cache *bib_cacheTCP;
extern struct hlist_head *hash6;
extern struct hlist_head *hash4;
extern __be32 ipv4_addr;
extern struct list_head free_udp_transport_addr;

static inline __be16 nat64_hash4(__be32 addr, __be16 port)
{
	return port;
}

static inline __be16 nat64_hash6(struct in6_addr addr6, __be16 port)
{
	__be32 addr4 = addr6.s6_addr32[1] ^ addr6.s6_addr32[2] ^ addr6.s6_addr32[3];
	return (addr4 >> 16) ^ addr4 ^ port;
}

static inline void session_renew(struct nat64_st_entry *session, enum expiry_type type)
{
	list_del(&session->byexpiry);
	session->expires = jiffies + expiry_base[type].timeout*HZ;
	list_add_tail(&session->byexpiry, &expiry_base[type].queue);
	printk("NAT64: [session] Renewing session %pI4:%hu (timeout %u sec).\n", &session->remote4_addr, ntohs(session->remote4_port), expiry_base[type].timeout);
}

static inline int tcp_timeout_fsm(struct nat64_st_entry *session)
{
	if(session->state == ESTABLISHED) {
		session_renew(session, TCP_TRANS);
		session->state = FOUR_MIN;
		return 1;
	}

	return 0;
}

static inline void tcp4_fsm(struct nat64_st_entry *session, struct tcphdr *tcph)
{
//	printk("nat64: [fsm4] Got packet state %d.\n", session->state);

	switch(session->state) {
	case CLOSED:
		break;
	case V6_SYN_RCV:
		if(tcph->syn) {
			session_renew(session, TCP_EST);
			session->state = ESTABLISHED;
		}
		break;
	case V4_SYN_RCV:
		//if(tcph->syn)
		//	session_renew(session, TCP_TRANS);
		break;
	case FOUR_MIN:
		if(!tcph->rst) {
			session_renew(session, TCP_EST);
			session->state = ESTABLISHED;
		}
		break;
	case ESTABLISHED:
		if(tcph->fin) {
			//session_renew(session, TCP_EST);
			session->state = V4_FIN_RCV;
		} else if(tcph->rst) {
			session_renew(session, TCP_TRANS);
			session->state = FOUR_MIN;
		} else {
			session_renew(session, TCP_EST);
		}
		break;
	case V6_FIN_RCV:
		if(tcph->fin) {
			session_renew(session, TCP_TRANS);
			session->state = V6_FIN_V4_FIN;
		} else {
			session_renew(session, TCP_EST);
		}
		break;
	case V4_FIN_RCV:
		session_renew(session, TCP_EST);
		break;
	case V6_FIN_V4_FIN:
		break;
	}
}

static inline void tcp6_fsm(struct nat64_st_entry *session, struct tcphdr *tcph)
{
//	printk("nat64: [fsm6] Got packet state %d.\n", session->state);

	switch(session->state) {
	case CLOSED:
		if(tcph->syn) {
			session_renew(session, TCP_TRANS);
			session->state = V6_SYN_RCV;
		}
		break;
	case V6_SYN_RCV:
		if(tcph->syn)
			session_renew(session, TCP_TRANS);
		break;
	case V4_SYN_RCV:
		if(tcph->syn) {
			session_renew(session, TCP_EST);
			session->state = ESTABLISHED;
		}
		break;
	case FOUR_MIN:
		if(!tcph->rst) {
			session_renew(session, TCP_EST);
			session->state = ESTABLISHED;
		}
		break;
	case ESTABLISHED:
		if(tcph->fin) {
			//session_renew(session, TCP_EST);
			session->state = V6_FIN_RCV;
		} else if(tcph->rst) {
			session_renew(session, TCP_TRANS);
			session->state = FOUR_MIN;
		} else {
			session_renew(session, TCP_EST);
		}
		break;
	case V6_FIN_RCV:
		session_renew(session, TCP_EST);
		break;
	case V4_FIN_RCV:
		if(tcph->fin) {
			session_renew(session, TCP_TRANS);
			session->state = V6_FIN_V4_FIN;
		} else {
			session_renew(session, TCP_EST);
		}
		break;
	case V6_FIN_V4_FIN:
		break;
	}
}

static inline void clean_expired_sessions(struct list_head *queue)
{
	struct list_head *pos;
	struct list_head *n;
	struct list_head *next_session;
	struct nat64_st_entry *session;
	struct nat64_bib_entry *bib;
	int i = 0;

	list_for_each_safe(pos, n, queue) {
		++i;
		session = list_entry(pos, struct nat64_st_entry, byexpiry);
		if(time_after(jiffies, session->expires)) {
			if(tcp_timeout_fsm(session))
				continue;
			printk("NAT64: [garbage-collector] removing session %pI4:%hu\n", &session->remote4_addr, ntohs(session->remote4_port));
			list_del(pos);
			next_session = session->list.next;
			list_del(&session->list);
			if(list_empty(next_session)) {
				bib = list_entry(next_session, struct nat64_bib_entry, sessions);
				printk("NAT64: [garbage-collector] removing bib %pI6c,%hu <--> %pI4:%hu\n", &bib->remote6_addr, ntohs(bib->remote6_port), &bib->local4_addr, ntohs(bib->local4_port));
				hlist_del(&bib->byremote);
				hlist_del(&bib->bylocal);
				kmem_cache_free(bib_cache, bib);
			}
			kmem_cache_free(st_cache, session);
		}
		else
			break;
	}
}


static inline void clean_expired_sessions_tcp(struct list_head *queue)
{
	struct list_head *pos;
	struct list_head *n;
	struct list_head *next_session;
	struct nat64_st_entry *session;
	struct nat64_bib_entry *bib;
	int i = 0;

	list_for_each_safe(pos, n, queue) {
		++i;
		session = list_entry(pos, struct nat64_st_entry, byexpiry);
		if(time_after(jiffies, session->expires)) {
			if(tcp_timeout_fsm(session))
				continue;
			printk("NAT64: [garbage-collector] removing session %pI4:%hu\n", &session->remote4_addr, ntohs(session->remote4_port));
			list_del(pos);
			next_session = session->list.next;
			list_del(&session->list);
			if(list_empty(next_session)) {
				bib = list_entry(next_session, struct nat64_bib_entry, sessions);
				printk("NAT64: [garbage-collector] removing bib %pI6c,%hu <--> %pI4:%hu\n", &bib->remote6_addr, ntohs(bib->remote6_port), &bib->local4_addr, ntohs(bib->local4_port));
				hlist_del(&bib->byremote);
				hlist_del(&bib->bylocal);
				kmem_cache_free(bib_cacheTCP, bib);
			}
			kmem_cache_free(st_cacheTCP, session);
		}
		else
			break;
	}
}

static inline struct nat64_st_entry *session_ipv4_lookup(struct nat64_bib_entry *bib, __be32 remote4_addr, __be16 remote4_port)
{
	struct nat64_st_entry	*session;
	struct list_head	*pos;

	list_for_each(pos, &bib->sessions) {
		session = list_entry(pos, struct nat64_st_entry, list);
		if(session->remote4_addr == remote4_addr && session->remote4_port == remote4_port)
			return session;
	}

	return NULL;
}

static inline struct nat64_st_entry *session_create(struct nat64_bib_entry *bib, struct in6_addr *in6_daddr, __be32 addr, __be16 port, enum expiry_type type)
{
	struct nat64_st_entry *s;

	s = kmem_cache_zalloc(st_cache, GFP_ATOMIC);
	if(!s) {
		printk("NAT64: [session] Unable to allocate memory for new session entry.\n");
		return NULL;
	}
	s->state = CLOSED;

	s->remote6_addr = bib->remote6_addr; // X' addr
	s->embedded6_addr = *(in6_daddr); // Y' addr
	s->local4_addr = bib->local4_addr; // T' addr
	s->remote4_addr = addr; // Z' addr

	s->remote6_port = bib->remote6_port; // x port
	s->embedded6_port = port; // y port
	s->local4_port = bib->local4_port; // t port
	s->remote4_port = port; // z port

	list_add(&s->list, &bib->sessions);

	s->expires = jiffies + expiry_base[type].timeout*HZ;
	list_add_tail(&s->byexpiry, &expiry_base[type].queue);

	printk("NAT64: [session] New session (timeout %u sec).\n", expiry_base[type].timeout);
	printk("NAT64: [session] x:%hu \tX':%pI6c.\n", 	ntohs(s->remote6_port),  &s->remote6_addr);
	printk("NAT64: [session] y:%hu \tY':%pI6c.\n", 	ntohs(s->embedded6_port),&s->embedded6_addr);
	printk("NAT64: [session] t:%hu \tT:%pI4.\n",	ntohs(s->local4_port),   &s->local4_addr);
	printk("NAT64: [session] z:%hu \tZ(Y'):%pI4.\n",ntohs(s->remote4_port),  &s->remote4_addr);
	
	return s;	
}
static inline struct nat64_st_entry *session_create_tcp(struct nat64_bib_entry *bib, struct in6_addr *in6_daddr, __be32 addr, __be16 port, enum expiry_type type)
{
	struct nat64_st_entry *s;

	s = kmem_cache_zalloc(st_cacheTCP, GFP_ATOMIC);
	if(!s) {
		printk("NAT64: [session] Unable to allocate memory for new session entry.\n");
		return NULL;
	}
	s->state = CLOSED;

	s->remote6_addr = bib->remote6_addr; // X' addr
	s->embedded6_addr = *(in6_daddr); // Y' addr
	s->local4_addr = bib->local4_addr; // T' addr
	s->remote4_addr = addr; // Z' addr

	s->remote6_port = bib->remote6_port; // x port
	s->embedded6_port = port; // y port
	s->local4_port = bib->local4_port; // t port
	s->remote4_port = port; // z port

	list_add(&s->list, &bib->sessions);

	s->expires = jiffies + expiry_base[type].timeout*HZ;
	list_add_tail(&s->byexpiry, &expiry_base[type].queue);

	pr_debug("NAT64: [session] New session (timeout %u sec).", expiry_base[type].timeout);
	pr_debug("NAT64: [session] x:%hu\tX':%pI6c.", ntohs(s->remote6_port), &s->remote6_addr);
	pr_debug("NAT64: [session] y:%hu\tY':%pI6c.", ntohs(s->embedded6_port), &s->embedded6_addr);
	pr_debug("NAT64: [session] t:%hu\tT':%pI4.", ntohs(s->local4_port), &s->local4_addr);
	pr_debug("NAT64: [session] z:%hu\tZ':%pI4.", ntohs(s->remote4_port), &s->remote4_addr);
	
	return s;	
}

static inline struct nat64_bib_entry *bib_ipv4_lookup(__be32 local_addr, __be16 local_port, int type)
{
	struct hlist_node	*pos;
	struct nat64_bib_entry	*bib;
	__be16			h = nat64_hash4(local_addr, local_port);
	struct hlist_head	*hlist = &hash4[h];

	hlist_for_each(pos, hlist) {
		bib = hlist_entry(pos, struct nat64_bib_entry, bylocal);
		if(bib->type == type && bib->local4_addr == local_addr && bib->local4_port == local_port)
			return bib;
	}

	//return (pos ? bib : NULL);
	return NULL;
}

static inline struct nat64_bib_entry *bib_ipv6_lookup(struct in6_addr *remote_addr, __be16 remote_port, int type)
{
	struct hlist_node	*pos;
	struct nat64_bib_entry	*bib;
	__be16 			h = nat64_hash6(*remote_addr, remote_port);
	struct hlist_head	*hlist = &hash6[h];

	hlist_for_each(pos, hlist) {
		bib = hlist_entry(pos, struct nat64_bib_entry, byremote);
		if(bib->type == type && bib->remote6_port == remote_port && memcmp(&bib->remote6_addr, remote_addr, sizeof(*remote_addr)) == 0)
			return bib;
	}

	//return (pos ? bib : NULL);
	return NULL;
}

static inline __be16 bib_allocate_local4_port(__be16 port, int type)
{
	// FIXME: This should give a different port than the one it originally came from.
	struct hlist_node *node;
	struct nat64_bib_entry *entry;
	__be16 min, max, i;
	int flag = 0;
	port = ntohs(port);
	min = port < 1024 ? 0 : 1024;
	max = port < 1024 ? 1023 : 65535;

	for (i = port; i <= max; i += 2, flag = 0) {
		hlist_for_each(node, &hash4[htons(i)]) {
			entry = hlist_entry(node, struct nat64_bib_entry, bylocal);
			if(entry->type == type) {
				flag = 1;
				break;
			}
		}
		if(!flag)
			return htons(i);
	}

	flag = 0;
	for (i = port - 2; i >= min; i -=2, flag = 0) {
		hlist_for_each(node, &hash4[htons(i)]) {
			entry = hlist_entry(node, struct nat64_bib_entry, bylocal);
			if(entry->type == type) {
				flag = 1;
				break;
			}
		}
		if(!flag)
			return htons(i);
	}

	return -1;
}

static inline struct nat64_bib_entry *bib_create(struct in6_addr *remote6_addr, __be16 remote6_port,
			     __be32 local4_addr, __be16 local4_port, int type)
{
	struct nat64_bib_entry *bib;

	bib = kmem_cache_zalloc(bib_cache, GFP_ATOMIC);
	if (!bib) {
		printk("NAT64: [bib] Unable to allocate memory for new bib entry.\n");
		return NULL;
	}

	bib->type = type;
	memcpy(&bib->remote6_addr, remote6_addr, sizeof(struct in6_addr));
	bib->local4_addr = local4_addr;
	bib->remote6_port = remote6_port;
	bib->local4_port = local4_port; // FIXME: Should be different than the remote6_port.
	INIT_LIST_HEAD(&bib->sessions);
	pr_debug("NAT64: [bib] New bib %pI6c,%hu <--> %pI4:%hu.\n", 
			remote6_addr, ntohs(remote6_port), 
			&local4_addr, ntohs(local4_port));

	return bib;
}


static inline struct nat64_bib_entry *bib_create_tcp(struct in6_addr *remote6_addr, __be16 remote6_port,
			     __be32 local4_addr, __be16 local4_port, int type)
{
	struct nat64_bib_entry *bib;

	bib = kmem_cache_zalloc(bib_cacheTCP, GFP_ATOMIC);
	if (!bib) {
		printk("NAT64: [bib] Unable to allocate memory for new TCP bib entry.\n");
		return NULL;
	}

	bib->type = type;
	memcpy(&bib->remote6_addr, remote6_addr, sizeof(struct in6_addr));
	bib->local4_addr = local4_addr;
	bib->remote6_port = remote6_port;
	bib->local4_port = local4_port; // FIXME: Should be different than the remote6_port.
	INIT_LIST_HEAD(&bib->sessions);
	pr_debug("NAT64: [bib] New TCP bib %pI6c,%hu <--> %pI4:%hu.", remote6_addr, ntohs(remote6_port), &local4_addr, ntohs(local4_port));

	return bib;
}

static inline struct nat64_bib_entry
	*bib_session_create(struct in6_addr *saddr, struct in6_addr *in6_daddr,
						__be32 daddr, __be16 sport, __be16 dport,
						int protocol, enum expiry_type type)
{
	struct nat64_bib_entry *bib;
	struct nat64_st_entry *session;
	__be16 local4_port;
	__be32 local4_addr;

  //local4_port = bib_allocate_local4_port(sport, protocol); // FIXME: Should be different than sport
  struct transport_addr_struct *transport_addr;
  transport_addr = get_tranport_addr(&free_udp_transport_addr);
  
  if(transport_addr == NULL){
    printk("pool out of ipv4 address\n");
    local4_port = -1;
    local4_addr = -1;
  }else{
    INIT_LIST_HEAD(&transport_addr->list);
    local4_port = ntohs(transport_addr->port);
    in4_pton(transport_addr->address, -1, (u8 *)&local4_addr, '\x0', NULL);
    pr_debug("NAT: IPv4 Pool: using address %s and port %u.\n", transport_addr->address, transport_addr->port);
  }  
  
	if (local4_port < 0) {
		pr_debug("NAT64: [bib] Unable to allocate new local IPv4 port. Dropping connection.\n");
		return NULL;
	}

	//bib = bib_create(saddr, sport, ipv4_addr, local4_port, protocol);
	bib = bib_create(saddr, sport, local4_addr, local4_port, protocol);

	if (!bib)
		return NULL;

	hlist_add_head(&bib->byremote, &hash6[nat64_hash6(*saddr, sport)]);
	hlist_add_head(&bib->bylocal, &hash4[local4_port]);
//	hlist_add_head(&bib->bylocal, &hash4[sport]);

	session = session_create(bib, in6_daddr, daddr, dport, type);
	if(!session) {
		kmem_cache_free(bib_cache, bib);
		return NULL;
	}

	return bib;
}

static inline struct nat64_bib_entry *bib_session_create_tcp(struct in6_addr *saddr, struct in6_addr *in6_daddr, __be32 daddr, __be16 sport, __be16 dport, int protocol, enum expiry_type type)
{
	struct nat64_bib_entry *bib;
	struct nat64_st_entry *session;
	__be16 local4_port;
	__be32 local4_addr;
	
	pr_debug("NAT64: [bib1] source PORT %hu .\n", ntohs(sport));
  // local4_port = bib_allocate_local4_port(sport, protocol); // FIXME: Should be different than sport
  
  struct transport_addr_struct *transport_addr;
  transport_addr = get_tranport_addr(&free_udp_transport_addr);
  
  if(transport_addr == NULL){
    printk("pool out of ipv4 address\n");
    local4_port = -1;
    local4_addr = -1;
  }else{
    INIT_LIST_HEAD(&transport_addr->list);
    local4_port = ntohs(transport_addr->port);
    in4_pton(transport_addr->address, -1, (u8 *)&local4_addr, '\x0', NULL);
    pr_debug("NAT: IPv4 Pool: using address %s and port %u.\n", transport_addr->address, transport_addr->port);
  }  
  
	if (local4_port < 0) {
		pr_debug("NAT64: [bib] Unable to allocate new local IPv4 port. Dropping connection.");
		return NULL;
	}
	pr_debug("NAT64: [bib2] destination PORT %hu .\n", ntohs(dport));

	bib = bib_create_tcp(saddr, sport, local4_addr, local4_port, protocol);
	if (!bib)
		return NULL;

	hlist_add_head(&bib->byremote, &hash6[nat64_hash6(*saddr, sport)]);
	hlist_add_head(&bib->bylocal, &hash4[local4_port]);
	
	session = session_create_tcp(bib, in6_daddr, daddr, dport, type);
	if(!session) {
		kmem_cache_free(bib_cacheTCP, bib);
		return NULL;
	}

	return bib;
}

/*
 * strtok_r - extract tokens from strings
 * @s:  The string to be searched
 * @ct: The characters to deliminate the tokens
 * @saveptr: The pointer to the next token
 *
 * It returns the next token found outside of the @ct delimiters.
 * Multiple occurrences of @ct characters will be considered
 * a single delimiter. In other words, the returned token will
 * always have a size greater than 0 (or NULL if no token found).
 *
 * A '\0' is placed at the end of the found token, and
 * @saveptr is updated to point to the location after that.
 */
static inline char *strtokr(char *s, const char *ct, char **saveptr){
	char *ret;
	int skip;

	if (!s)
		s = *saveptr;

	/* Find start of first token */
	skip = strspn(s, ct);
	*saveptr = s + skip;

	/* return NULL if we found no token */
	if (!*saveptr[0])
		return NULL;

	/*
	 * strsep is different than strtok, where as saveptr will be NULL
	 * if token not found. strtok makes it point to the end of the string.
	 */
	ret = strsep(saveptr, ct);
	if (!*saveptr)
		*saveptr = &ret[strlen(ret)];
	return ret;
}


static inline __be32 nat64_extract2(struct in6_addr addr, int prefix)
{
	switch(prefix) {
		case 32:
			return addr.s6_addr32[3];
		case 40:
			return 0;	//FIXME
		case 48:
			return 0;	//FIXME
		case 56:
			return 0;	//FIXME
		case 64:
			return 0;	//FIXME
		case 96:
			return addr.s6_addr32[1];
		default:
			return 0;
	}
}

static inline void print_bufu(char *b){
	struct nat64_bib_entry *bib;
	char *token, *subtoken, *str1, *str2;
	char *saveptr1, *saveptr2;
	int j, ret;
	int cont = 0; 
	int proto =0;
	int con = -1;
	uint16_t p1 =0; 
	uint16_t p2=0;
	long unsigned int res;
	struct in6_addr addr1 = IN6ADDR_ANY_INIT;
	struct in6_addr addr2 = IN6ADDR_ANY_INIT;
	for (j = 1, str1 = b; ; j++, str1 = NULL) {
		token = strtokr(str1, "&", &saveptr1);
		if (token == NULL)
		    break;
		//printk("%d: %s\n", j, token);
	    	if (strcmp (token,"tcp") == 0)
	    		proto = 6;
		else if (strcmp (token,"udp") == 0)
	    		proto = 17;
		else if (strcmp (token,"icmp") == 0)
	    		proto = 1;
		for (str2 = token; ; str2 = NULL) {
			subtoken = strtokr(str2, "#", &saveptr2);
		    	if (subtoken == NULL)
		        	break;
			if (str2 == NULL){
				if (cont==0){
					kstrtoul(subtoken, 10, &res);
					//printk("port 1 %lu\n", res);
					p1 = res;
					//printk("port short %d\n", p1);
					cont++;
				} else{
					kstrtoul(subtoken, 10, &res);
					//printk("port 2 %lu\n", res);
					p2 = res;
					//printk("port short %d\n", p1);
				}
			} else {
				if (con==0){
					//inet_pton6(subtoken, &addr1.s6_addr);
					ret = in6_pton(subtoken, -1, (u8 *)&addr1.s6_addr, '\x0', NULL);
					//printk("KERN_DEBUG2 Address: %pI6 \n", &addr1.s6_addr);
				} else if (con > 0){
					//inet_pton6(subtoken, &addr2.s6_addr);
					//
					ret = in6_pton(subtoken, -1, (u8 *)&addr2.s6_addr, '\x0', NULL);
					//printk("KERN_DEBUG Address: %pI6 \n", addr2.s6_addr);
				}
			con++;
			}
		    	//printk(" --> %s\n", subtoken);
		}
	}

	switch(proto) {
		case 1:
			break;
		case 6:
			//printk("port %d\n", p1);
			//printk("port %d\n", p2);
			//printk("hola tcp ");
			bib = bib_session_create_tcp(&addr1,&addr2,nat64_extract2(addr2,32),ntohs(p1),ntohs(p2),proto,TCP_TRANS);
			break;
		case 17:
			bib = bib_session_create(&addr1,&addr2,nat64_extract2(addr2,32),ntohs(p1),ntohs(p2),proto,UDP_DEFAULT);
			break;
		default:
			break;

	}

}

#endif
