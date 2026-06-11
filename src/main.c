// NetX UDP/TCP DNS-to-DoH service
// (C) 2026 Alok Priyadarshi

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#if HAS_LIBSYSTEMD == 1
#include <systemd/sd-daemon.h>
#endif

#include "dns_listener.h"
#include "dns_listener_tcp.h"
#include "dns_listener_udp.h"
#include "dns_poller.h"
#include "doh_proxy.h"
#include "https_client.h"
#include "logging.h"
#include "options.h"
#include "stat.h"

typedef struct netx_worker netx_worker_t;

typedef struct {
  netx_worker_t *workers;
  int worker_count;
} shutdown_group_t;

struct netx_worker {
  int id;
  int primary;
  options_t *opt;
  shutdown_group_t *shutdown_group;
  struct ev_loop *loop;
  ev_async shutdown_async;
  pthread_t thread;
  char resolver_hostname[255];
};

static int is_ipv4_address(char *str) {
    struct in6_addr addr;
    return inet_pton(AF_INET, str, &addr) == 1;
}

static int hostname_from_url(const char* url_in,
                             char* hostname, const size_t hostname_len) {
  int res = 0;
  CURLU *url = curl_url();
  if (url != NULL) {
    CURLUcode rc = curl_url_set(url, CURLUPART_URL, url_in, 0);
    if (rc == CURLUE_OK) {
      char *host = NULL;
      rc = curl_url_get(url, CURLUPART_HOST, &host, 0);
      if (rc == CURLUE_OK && host != NULL) {
        const size_t host_len = strlen(host);
        if (hostname_len > 0 &&
            host_len < hostname_len &&
            host[0] != '[' && host[host_len-1] != ']' && // skip IPv6 address
            !is_ipv4_address(host)) {
          strncpy(hostname, host, hostname_len-1);
          hostname[hostname_len-1] = '\0';
          res = 1; // success
        }
      }
      curl_free(host);
    }
    curl_url_cleanup(url);
  }
  return res;
}

static void request_worker_shutdown(shutdown_group_t *group);

static void sigpipe_cb(struct ev_loop __attribute__((__unused__)) *loop,
                       ev_signal __attribute__((__unused__)) *w,
                       int __attribute__((__unused__)) revents) {
  ELOG("Received SIGPIPE. Ignoring.");
}

static void systemd_notify_ready(void __attribute__((__unused__)) *unused) {
#if HAS_LIBSYSTEMD == 1
  static uint8_t called_once = 0;
  if (called_once != 0) {
    DLOG("Systemd notify already called once!");
    return;
  }
  called_once = 1;
  const int result = sd_notify(0, "READY=1");
  if (result > 0) {
    DLOG("Systemd notify succeeded, service is ready!");
  } else if (result == 0) {
    WLOG("Systemd notify called, but NOTIFY_SOCKET not set. Running manually?");
  } else {
    ELOG("Systemd notify failed with: %s", strerror(result));
  }
#else
  DLOG("Systemd notify skipped, not compiled with libsystemd!");
#endif
}

static int proxy_supports_name_resolution(const char *proxy)
{
  size_t i = 0;
  const char *ptypes[] = {"http:", "https:", "socks4a:", "socks5h:"};

  if (proxy == NULL) {
    return 0;
  }
  for (i = 0; i < sizeof(ptypes) / sizeof(*ptypes); i++) {
    if (strncasecmp(proxy, ptypes[i], strlen(ptypes[i])) == 0) {
      return 1;
    }
  }
  return 0;
}

static struct addrinfo * get_listen_address(const char *listen_addr) {
  struct addrinfo *ai = NULL;
  struct addrinfo hints;
  memset(&hints, 0, sizeof(struct addrinfo));
  /* prevent DNS lookups if leakage is our worry */
  hints.ai_flags = AI_NUMERICHOST;

  int res = getaddrinfo(listen_addr, NULL, &hints, &ai);
  if (res != 0) {
    FLOG("Error parsing listen address %s, getaddrinfo error: %s",
         listen_addr, gai_strerror(res));
  }

  return ai;
}

static const char * sw_version(void) {
#ifdef SW_VERSION
  return SW_VERSION;
#else
  return "2025.8.26-atLeast";  // update date sometimes, like 1-2 times a year
#endif
}

static void set_listen_port(struct addrinfo *listen_addrinfo, int listen_port) {
  if (listen_addrinfo->ai_family == AF_INET) {
    ((struct sockaddr_in*) listen_addrinfo->ai_addr)->sin_port = htons((uint16_t)listen_port);
  } else if (listen_addrinfo->ai_family == AF_INET6) {
    ((struct sockaddr_in6*) listen_addrinfo->ai_addr)->sin6_port = htons((uint16_t)listen_port);
  }
}

static void shutdown_async_cb(struct ev_loop *loop,
                              ev_async __attribute__((__unused__)) *w,
                              int __attribute__((__unused__)) revents) {
  ev_break(loop, EVBREAK_ALL);
}

static void request_worker_shutdown(shutdown_group_t *group) {
  for (int i = 0; i < group->worker_count; i++) {
    if (group->workers[i].loop != NULL) {
      ev_async_send(group->workers[i].loop, &group->workers[i].shutdown_async);
    }
  }
}

static void signal_shutdown_cb(struct ev_loop __attribute__((__unused__)) *loop,
                               ev_signal *w,
                               int __attribute__((__unused__)) revents) {
  shutdown_group_t *group = (shutdown_group_t *)w->data;
  ILOG("Shutting down gracefully. To force exit, send signal again.");
  request_worker_shutdown(group);
}

static void worker_start_dns_poller(netx_worker_t *worker,
                                    doh_proxy_t *proxy,
                                    dns_poller_t *dns_poller,
                                    uint8_t *using_dns_poller) {
  options_t *opt = worker->opt;

  if (proxy_supports_name_resolution(opt->curl_proxy)) {
    if (worker->primary) {
      systemd_notify_ready(NULL);
    }
    return;
  }

  if (hostname_from_url(opt->resolver_url, worker->resolver_hostname,
                        sizeof(worker->resolver_hostname))) {
    *using_dns_poller = 1;
    doh_proxy_await_bootstrap(proxy);
    if (worker->primary) {
      doh_proxy_set_on_ready(proxy, systemd_notify_ready, NULL);
    }
    dns_poller_init(dns_poller, worker->loop, opt->bootstrap_dns,
                    opt->bootstrap_dns_polling_interval, opt->source_addr,
                    worker->resolver_hostname,
                    opt->ipv4 ? AF_INET : AF_UNSPEC,
                    doh_proxy_handle_resolver_update, proxy);
    ILOG("Worker %d DNS polling initialized for '%s'",
         worker->id, worker->resolver_hostname);
  } else {
    ILOG("Resolver prefix '%s' doesn't appear to contain a "
         "hostname. DNS polling disabled.", opt->resolver_url);
    if (worker->primary) {
      systemd_notify_ready(NULL);
    }
  }
}

static void run_worker(netx_worker_t *worker) {
  options_t *opt = worker->opt;
  worker->loop = ev_loop_new(0);
  if (worker->loop == NULL) {
    FLOG("Worker %d failed to create event loop", worker->id);
  }

  ev_async_init(&worker->shutdown_async, shutdown_async_cb);
  ev_async_start(worker->loop, &worker->shutdown_async);

  ev_signal sigpipe;
  ev_signal sigint;
  ev_signal sigterm;
  if (worker->primary) {
    ev_signal_init(&sigpipe, sigpipe_cb, SIGPIPE);
    ev_signal_start(worker->loop, &sigpipe);

    ev_signal_init(&sigint, signal_shutdown_cb, SIGINT);
    sigint.data = worker->shutdown_group;
    ev_signal_start(worker->loop, &sigint);

    ev_signal_init(&sigterm, signal_shutdown_cb, SIGTERM);
    sigterm.data = worker->shutdown_group;
    ev_signal_start(worker->loop, &sigterm);

    logging_events_init(worker->loop);
  }

  stat_t stat;
  stat_init(&stat, worker->loop, opt->stats_interval);
  stat_t *stat_ptr = (opt->stats_interval ? &stat : NULL);

  https_client_t https_client;
  https_client_init(&https_client, opt, stat_ptr, worker->loop);

  doh_proxy_t *proxy = doh_proxy_create(worker->loop, &https_client,
                                        opt->resolver_url, stat_ptr);

  struct addrinfo *listen_addrinfo = get_listen_address(opt->listen_addr);
  set_listen_port(listen_addrinfo, opt->listen_port);

  dns_listener_t *udp_listener =
      dns_udp_listener_create(worker->loop, listen_addrinfo,
                              doh_proxy_handle_request, proxy);

  dns_listener_t *tcp_listener = NULL;
  if (opt->tcp_client_limit > 0) {
    tcp_listener = dns_tcp_listener_create(worker->loop, listen_addrinfo,
                                           (uint16_t)opt->tcp_client_limit,
                                           doh_proxy_handle_request, proxy);
  }

  freeaddrinfo(listen_addrinfo);
  listen_addrinfo = NULL;

  dns_poller_t dns_poller;
  uint8_t using_dns_poller = 0;
  worker_start_dns_poller(worker, proxy, &dns_poller, &using_dns_poller);

  ILOG("Worker %d started", worker->id);
  ev_run(worker->loop, 0);
  DLOG("Worker %d loop breaked", worker->id);

  if (using_dns_poller) {
    dns_poller_cleanup(&dns_poller);
  }

  if (worker->primary) {
    logging_events_cleanup(worker->loop);
    ev_signal_stop(worker->loop, &sigterm);
    ev_signal_stop(worker->loop, &sigint);
    ev_signal_stop(worker->loop, &sigpipe);
  }
  ev_async_stop(worker->loop, &worker->shutdown_async);
  udp_listener->stop(udp_listener);
  if (tcp_listener != NULL) {
    tcp_listener->stop(tcp_listener);
  }
  stat_stop(&stat);

  DLOG("Worker %d re-entering loop", worker->id);
  ev_run(worker->loop, 0);
  DLOG("Worker %d loop finished all events", worker->id);

  udp_listener->destroy(udp_listener);
  if (tcp_listener != NULL) {
    tcp_listener->destroy(tcp_listener);
  }
  // The CURLOPT_RESOLVE list owned by NetX must outlive in-flight curl
  // easy handles, which is why https_client_cleanup runs first.
  https_client_cleanup(&https_client);
  doh_proxy_destroy(proxy);
  stat_cleanup(&stat);

  ev_loop_destroy(worker->loop);
  worker->loop = NULL;
  DLOG("Worker %d loop destroyed", worker->id);
}

static void *worker_thread_main(void *arg) {
  run_worker((netx_worker_t *)arg);
  return NULL;
}

int main(int argc, char *argv[]) {
  struct Options opt;
  options_init(&opt);
  switch (options_parse_args(&opt, argc, argv)) {
    case OPR_SUCCESS:
      break;
    case OPR_HELP:
      options_show_usage(argc, argv);
      exit(0);  // asking for help is not a problem
    case OPR_VERSION: {
      printf("%s\n", sw_version());
      curl_version_info_data *curl_ver = curl_version_info(CURLVERSION_NOW);
      if (curl_ver != NULL) {
        printf("Using: ev/%d.%d c-ares/%s %s\n",
               ev_version_major(), ev_version_minor(),
               ares_version(NULL), curl_version());
        printf("Features: %s%s%s%s\n",
               curl_ver->features & CURL_VERSION_HTTP2 ? "HTTP2 " : "",
               curl_ver->features & CURL_VERSION_HTTP3 ? "HTTP3 " : "",
               curl_ver->features & CURL_VERSION_HTTPS_PROXY ? "HTTPS-proxy " : "",
               curl_ver->features & CURL_VERSION_IPV6 ? "IPv6" : "");
        exit(0);
      } else {
        printf("\nFailed to get curl version info!\n");
        exit(1);
      }
    }
    case OPR_PARSING_ERROR:
      printf("Failed to parse options!\n");
      __attribute__((fallthrough));
    case OPR_OPTION_ERROR:
      printf("\n");
      options_show_usage(argc, argv);
      exit(1);
    default:
      abort();  // must not happen
  }

  logging_init(opt.logfd, opt.loglevel, (uint32_t)opt.flight_recorder_size);

  ILOG("Version: %s", sw_version());
  ILOG("Built: " __DATE__ " " __TIME__);
  ILOG("System ev library: %d.%d", ev_version_major(), ev_version_minor());
  ILOG("System c-ares library: %s", ares_version(NULL));
  ILOG("System curl library: %s", curl_version());

  // Note: curl intentionally uses uninitialized stack variables and similar
  // tricks to increase it's entropy pool. This confuses valgrind and leaks
  // through to errors about use of uninitialized values in our code. :(
  CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (code != CURLE_OK) {
    FLOG("Failed to initialize curl, error code %d: %s",
         code, curl_easy_strerror(code));
  }

  curl_version_info_data *curl_ver = curl_version_info(CURLVERSION_NOW);
  if (curl_ver == NULL) {
    FLOG("Failed to get curl version info!");
  }
  if (!(curl_ver->features & CURL_VERSION_HTTP2)) {
    WLOG("HTTP/2 is not supported by current libcurl");
  }
  if (!(curl_ver->features & CURL_VERSION_HTTP3)) {
    WLOG("HTTP/3 is not supported by current libcurl");
  }
  if (!(curl_ver->features & CURL_VERSION_IPV6)) {
    WLOG("IPv6 is not supported by current libcurl");
  }

  if (opt.gid != (uid_t)-1 && setgroups(1, &opt.gid)) {
    FLOG("Failed to set groups");
  }
  if (opt.gid != (uid_t)-1 && setgid(opt.gid)) {
    FLOG("Failed to set gid");
  }
  if (opt.uid != (uid_t)-1 && setuid(opt.uid)) {
    FLOG("Failed to set uid");
  }

  if (opt.daemonize) {
    // daemon() is non-standard. If needed, see OpenSSH openbsd-compat/daemon.c
    if (daemon(0, 0) == -1) {
      FLOG("daemon failed: %s", strerror(errno));
    }
  }

  netx_worker_t *workers = (netx_worker_t *)calloc((size_t)opt.worker_count,
                                                   sizeof(netx_worker_t));
  if (workers == NULL) {
    FLOG("Out of mem");
  }
  shutdown_group_t shutdown_group = {
    .workers = workers,
    .worker_count = opt.worker_count,
  };

  for (int i = 0; i < opt.worker_count; i++) {
    workers[i].id = i + 1;
    workers[i].primary = (i == 0);
    workers[i].opt = &opt;
    workers[i].shutdown_group = &shutdown_group;
  }

  ILOG("Starting %d worker thread%s", opt.worker_count,
       opt.worker_count == 1 ? "" : "s");

  for (int i = 1; i < opt.worker_count; i++) {
    int pthread_res = pthread_create(&workers[i].thread, NULL,
                                     worker_thread_main, &workers[i]);
    if (pthread_res != 0) {
      FLOG("Failed to start worker %d: %s", workers[i].id, strerror(pthread_res));
    }
  }

  run_worker(&workers[0]);

  for (int i = 1; i < opt.worker_count; i++) {
    int pthread_res = pthread_join(workers[i].thread, NULL);
    if (pthread_res != 0) {
      ELOG("Failed to join worker %d: %s", workers[i].id, strerror(pthread_res));
    }
  }

  free(workers);

  curl_global_cleanup();
  logging_cleanup();
  options_cleanup(&opt);

  return 0;
}
