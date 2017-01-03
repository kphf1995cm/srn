#include <stdio.h>
#include <sys/select.h>
#include <stdlib.h>
#include <string.h>

#include <ares.h>
#include <ares_dns.h>

#include "proxy.h"

ares_channel channel;
struct queue_thread replies;
pthread_mutex_t channel_mutex;

struct queue inner_queue;

void client_callback(void *arg, int status, __attribute__((unused)) int timeouts, unsigned char *abuf, int alen) {

  if (status != ARES_SUCCESS) {
    fprintf(stderr, "DNS server error: %s\n", ares_strerror(status));
    goto out;
  }

  struct callback_args *call_args = (struct callback_args *) arg;
  struct reply *reply = malloc(REPLY_ALLOC);
  if (!reply) {
    fprintf(stderr, "Out of memory !\n"); /* Ignore reply */
  } else {
    reply->data_length = alen;
    reply->buffer_size = REPLY_ALLOC;
    reply->additional_record_count = DNS_HEADER_ARCOUNT(abuf);
    reply->addr = call_args->addr;
    reply->addr_len = call_args->addr_len;
    reply->bandwidth_req = call_args->bandwidth_req;
    reply->latency_req = call_args->latency_req;
    reply->app_name_req = call_args->app_name_req;
    memcpy(reply->data, abuf, alen);
    DNS_HEADER_SET_QID((char *) reply->data, call_args->qid);
    if (queue_append(&inner_queue, (struct node *) reply)) {
      /* Dropping reply */
      FREE_POINTER(reply);
    }
  }
out:
  FREE_POINTER(arg);
}

static void *client_producer_main(__attribute__((unused)) void *args) {

  int err = 0;
  int nfds = 0;
  fd_set read_fds, write_fds;
  struct timeval timeout;
  struct reply *reply = NULL;

  print_debug("A client producer thread has started\n");

  queue_init(&inner_queue);
  while (!stop) {
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    timeout.tv_sec = TIMEOUT_LOOP;
    timeout.tv_usec = 0;
    nfds = ares_fds(channel, &read_fds, &write_fds);
    err = select(nfds, &read_fds, &write_fds, NULL, &timeout);
    if (err < 0) {
      perror("Select fail");
      break;
    }

    if (pthread_mutex_lock(&channel_mutex)) {
      perror("Cannot lock the mutex in client producer");
      break;
    }
    ares_process(channel, &read_fds, &write_fds);
    pthread_mutex_unlock(&channel_mutex);

    /* Transfer replies to the multi-threaded queue */
    queue_walk_dequeue(&inner_queue, reply, struct reply *) {
      print_debug("Client producer will append a reply to the appropriate queue\n");
      if (mqueue_append(&replies, (struct node *) reply)) {
        /* Dropping reply */
        FREE_POINTER(reply);
      }
    }
  }
  queue_destroy(&inner_queue);
  print_debug("A client producer thread has finished\n");
  return NULL;
}

static void *client_consumer_main(__attribute__((unused)) void *args) {

  struct srdb_table *router_tbl = srdb_table_by_name(srdb->tables, "RouterIds");
  struct srdb_router_entry router_entry;
  memset(&router_entry, 0, sizeof(router_entry));
  char thread_id [SLEN + 1];
  memset(&thread_id, 0, SLEN + 1);
  unsigned long req_counter = 0;

  struct srdb_table *tbl = srdb_table_by_name(srdb->tables, "FlowReq");
  struct srdb_flowreq_entry entry;
  memset(&entry, 0, sizeof(entry));

  struct reply *reply = NULL;

  print_debug("A client consumer thread has started\n");

  /* Get the OpenFlow ID of this thread */
  strncpy(router_entry.router, cfg.router_name, SLEN + 1);
  if (srdb_insert(srdb, router_tbl, (struct srdb_entry *) &router_entry, thread_id)) {
    fprintf(stderr, "Problem during extraction of thread ID -> stop thread\n");
    return NULL;
  }

  print_debug("This client consumer thread got the ID %s\n", thread_id);

  mqueue_walk_dequeue(&replies, reply, struct reply *) {
    print_debug("Client consumer dequeues a reply\n");

    snprintf(reply->ovsdb_req_uuid, SLEN + 1, "%s-%ld", thread_id, req_counter);
    if (mqueue_append(&replies_waiting_controller, (struct node *) reply)) {
      FREE_POINTER(reply);
      break;
    }
    print_debug("Client consumer forwards a reply to the monitor's queue with id %s\n", reply->ovsdb_req_uuid);

    strncpy(entry.destination, DEFAULT_DEST, SLEN + 1); /* TODO Extract */
    strncpy(entry.dstaddr, DEFAULT_DEST_ADDR, SLEN + 1); /* TODO Extract */
    strncpy(entry.source, reply->app_name_req, SLEN + 1);
    entry.bandwidth = reply->bandwidth_req;
    entry.delay = reply->latency_req;
    strncpy(entry.router, cfg.router_name, SLEN + 1);
    strncpy(entry.request_id, reply->ovsdb_req_uuid, SLEN + 1);

    srdb_insert(srdb, tbl, (struct srdb_entry *) &entry, NULL);
    print_debug("Client consumer makes the insertion in the OVSDB table\n");

    /* TODO Put in cache */

    req_counter++; /* The next request will have another id */
  }
  print_debug("A client consumer thread has finished\n");
  return NULL;
}

int init_client(int optmask, struct ares_addr_node *servers, pthread_t *client_consumer_thread, pthread_t *client_producer_thread) {

  int status = ARES_SUCCESS;
  struct ares_options options;
  memset(&options, 0, sizeof(struct ares_options));

  status = ares_library_init(ARES_LIB_INIT_ALL);
  if (status != ARES_SUCCESS) {
    fprintf(stderr, "ares_library_init: %s\n", ares_strerror(status));
    goto out_err;
  }

  status = ares_init_options(&channel, &options, optmask);
  if (status != ARES_SUCCESS) {
    fprintf(stderr, "ares_init_options: %s\n", ares_strerror(status));
    goto out_cleanup_cares;
  }

  if(servers) {
    status = ares_set_servers(channel, servers);
    if (status != ARES_SUCCESS) {
      fprintf(stderr, "ares_set_servers: %s\n", ares_strerror(status));
      goto out_cleanup_cares;
    }
  }

  mqueue_init(&replies, max_queries);

  pthread_mutex_init(&channel_mutex, NULL);

  /* Thread launching */
  status = pthread_create(client_consumer_thread, NULL, client_consumer_main, NULL);
  if (status) {
    perror("Cannot create client consumer thread");
    goto out_cleanup_queue_mutex;
  }
  status = pthread_create(client_producer_thread, NULL, client_producer_main, NULL);
  if (status) {
    perror("Cannot create client producer thread");
    goto out_cleanup_queue_mutex;
  }

out:
  return status;
out_cleanup_queue_mutex:
  mqueue_destroy(&replies);
  pthread_mutex_destroy(&channel_mutex);
out_cleanup_cares:
  ares_library_cleanup();
out_err:
  if (channel) {
    ares_destroy(channel);
    channel = NULL;
  }
  goto out;
}

void close_client() {
  mqueue_destroy(&replies);
  if (channel) {
    ares_destroy(channel);
    channel = NULL;
  }
  ares_library_cleanup();
}
