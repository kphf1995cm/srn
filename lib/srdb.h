#ifndef _SRDB_H
#define _SRDB_H

#include <netinet/in.h>

#define SLEN	128

enum srdb_type {
	SRDB_STR,
	SRDB_INT,
};

enum flowreq_status {
	STATUS_PENDING = 0,
	STATUS_ALLOWED = 1,
	STATUS_DENIED = 2,
};

struct srdb_descriptor {
	const char *name;
	enum srdb_type type;
	void *data;
	int index;
	size_t maxlen;
};

struct srdb_flow_entry {
	char _row[SLEN + 1];
	char _action[SLEN + 1];
	char destination[SLEN + 1];
	char bsid[SLEN + 1];
	char *segments;
	int __nsegs;
	int bandwidth;
	int delay;
	bool policing;
	char source[SLEN + 1];
	char router[SLEN + 1];
	char interface[SLEN + 1];
	bool reverse;
	char reverse_flow_uuid[SLEN + 1];
	char request_uuid[SLEN + 1];
	char _version[SLEN + 1];
};

struct srdb_flowreq_entry {
	char _row[SLEN + 1];
	char _action[SLEN + 1];
	char destination[SLEN + 1];
	char dstaddr[SLEN + 1];
	char source[SLEN + 1];
	int bandwidth;
	int delay;
	char router[SLEN + 1];
	int status;
	char _version[SLEN + 1];
};

struct srdb_linkstate_entry {
	char _row[SLEN + 1];
	char _action[SLEN + 1];
	char name1[SLEN + 1];
	char addr1[SLEN + 1];
	char name2[SLEN + 1];
	char addr2[SLEN + 1];
	int maxbw;
	int curbw;
	int delay;
	char _version[SLEN + 1];
};

struct srdb_nodestate_entry {
	char _row[SLEN + 1];
	char _action[SLEN + 1];
	char name[SLEN + 1];
	struct arraylist *prefix;
	char _version[SLEN + 1];
};

struct prefix {
	struct in6_addr addr;
	int len;
};
	
struct router {
	char name[SLEN + 1];
	struct prefix pbsid;
};

#endif
