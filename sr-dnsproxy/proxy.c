#include <sys/types.h>
#include <sys/select.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

#include <ares.h>
#include <ares_dns.h>

#include "proxy.h"

pthread_t server_producer_thread;
pthread_t server_consumer_thread;
pthread_t client_producer_thread;
pthread_t client_consumer_thread;
pthread_t monitor_flowreqs_thread;
pthread_t monitor_flows_thread;

volatile sig_atomic_t stop;

void inthand(int signum) {

  if (signum == SIGINT) {
    stop = 1;

    /* Unblocking threads waiting for these queues */
    mqueue_close(&queries, 1, 1);
    mqueue_close(&replies, 1, 1);
    mqueue_close(&replies_waiting_controller, 1, 1);

    /* Unblocking threads waiting for blocking system calls (e.g., select()) */
    pthread_kill(server_producer_thread, SIGUSR1);
    pthread_kill(server_consumer_thread, SIGUSR1);
    pthread_kill(client_producer_thread, SIGUSR1);
    pthread_kill(client_consumer_thread, SIGUSR1);
    //pthread_kill(monitor_flowreqs_thread, SIGUSR1); TODO ADD !
    pthread_kill(monitor_flows_thread, SIGUSR1);

  } else if (signum == SIGUSR1) {
    printf("Thread is stopped gracefully\n");
  } else {
    fprintf(stderr, "Does not understand signal number %d\n", signum);
  }
}

int main(int argc, char *argv[]) {

  int err = EXIT_SUCCESS;

  sigset_t set;
  struct sigaction sa;

	struct monitor_arg args[3];

  char *listen_port = NULL;
  struct ares_addr_node *servers = NULL;
  char *remote_port = NULL;

  int optmask = ARES_OPT_FLAGS;

  if (parse_arguments(argc, argv, &optmask, &listen_port, &remote_port, &servers)) {
    goto out_err;
  }

  /* Block SIGINT here to make this property inherited by the child process */
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  err = pthread_sigmask(SIG_BLOCK, &set, NULL);
  if (err) {
    perror("Cannot block SIGINT");
    goto out_err_free_args;
  }

  /* Allow threads to be interrupted by SIGUSR1 */
  sa.sa_handler = inthand;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGUSR1, &sa, NULL) == -1) {
    perror("Cannot change handling of SIGUSR1 signal");
    goto out_err;
  }

  /* Setup of the controller monitoring */
  err = init_monitor(listen_port, args, &monitor_flowreqs_thread, &monitor_flows_thread);
  if (err) {
    goto out_err_free_args;
  }

  /* Setup of the client threads */
  err = init_client(optmask, servers, &client_consumer_thread, &client_producer_thread);
  if (err) {
    goto out_err_free_args;
  }

  /* Setup of the server threads */
  err = init_server(&server_consumer_thread, &server_producer_thread);
  if (err) {
    goto out_err_free_args;
  }

  /* Get rid of memory allocated for arguments */
  FREE_POINTER(listen_port);
  FREE_POINTER(remote_port);
  destroy_addr_list(servers);
  servers = NULL;

  /* Allow SIGINT */
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  err = pthread_sigmask(SIG_UNBLOCK, &set, NULL);
  if (err) {
    perror("Cannot unblock SIGINT");
    goto out_err;
  }

  /* Gracefully kill the program when SIGINT is received */
  sa.sa_flags = SA_RESTART;
  if (sigaction(SIGINT, &sa, NULL) == -1) {
    perror("Cannot change handling of SIGINT signal");
    goto out_err;
  }

  /* Wait fo threads to finish */
  pthread_join(server_consumer_thread, NULL);
  pthread_join(server_producer_thread, NULL);
  pthread_join(client_consumer_thread, NULL);
  pthread_join(client_producer_thread, NULL);
  pthread_join(monitor_flowreqs_thread, NULL);
  pthread_join(monitor_flows_thread, NULL);

  close_server();
  close_client();
  close_monitor();

out:
  exit(err);
out_err_free_args:
  FREE_POINTER(listen_port);
  FREE_POINTER(remote_port);
  destroy_addr_list(servers);
  servers = NULL;
out_err:
  err = EXIT_FAILURE;
  goto out;
}
