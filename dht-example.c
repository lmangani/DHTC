/* Original example code was written by Juliusz Chroboczek. */
/* DHT Discovery modification by Matthew Di Ferrante */

/* For crypt */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/signal.h>
#include <time.h>

#include "dht.h"
#include "sha1.h"

#define MAX_BOOTSTRAP_NODES 20
static struct sockaddr_storage bootstrap_nodes[MAX_BOOTSTRAP_NODES];
static int num_bootstrap_nodes = 0;

static volatile sig_atomic_t exiting = 0;

static void sigexit(int signo) {
    exiting = 1;
}

static void init_signals(void) {
    struct sigaction sa;
    sigset_t ss;

    sigemptyset(&ss);
    sa.sa_handler = sigexit;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
}

unsigned char hash[20];

/* The call-back function is called by the DHT whenever something
   interesting happens. Right now, it only happens when we get a new value or
   when a search completes, but this may be extended in future versions. */
static void callback(void *closure,
                     int event,
                     const unsigned char *info_hash,
                     const void *data, size_t data_len) {
    int i = 0;
    if (event == DHT_EVENT_SEARCH_DONE)
        printf("Search done.\n");
    else if (event == DHT_EVENT_VALUES) {
        printf("Received %d values.\n", (int)(data_len / 6));
        for (i = 0; i < (int)(data_len / 6); i++) {
            fprintf(stdout, "%hhu.%hhu.%hhu.%hhu:%hu\n",
                    *(((char*)data)+(i*6)+0),
                    *(((char*)data)+(i*6)+1),
                    *(((char*)data)+(i*6)+2),
                    *(((char*)data)+(i*6)+3),
                    ntohs(*((unsigned short int *)(((char*)data)+(i*6)+4))));
        }
    }
}

static unsigned char buf[4096];

int run_resolver(const unsigned char *hash, time_t *tosleep, int s, int s6, int port) {
    int rc;
    struct timeval tv;
    fd_set readfds;
    tv.tv_sec = *tosleep;
    tv.tv_usec = random() % 1000000;
    struct sockaddr_storage from;
    socklen_t fromlen;
    static int last_run = 0;

    FD_ZERO(&readfds);
    if (s >= 0)
        FD_SET(s, &readfds);
    if (s6 >= 0)
        FD_SET(s6, &readfds);
    rc = select(s > s6 ? s + 1 : s6 + 1, &readfds, NULL, NULL, &tv);
    if (rc < 0) {
        if (errno != EINTR) {
            perror("select");
            sleep(1);
        }
    }

    if (exiting)
        return 1;

    if (rc > 0) {
        fromlen = sizeof(from);
        if (s >= 0 && FD_ISSET(s, &readfds))
            rc = recvfrom(s, buf, sizeof(buf) - 1, 0,
                          (struct sockaddr*)&from, &fromlen);
        else if (s6 >= 0 && FD_ISSET(s6, &readfds))
            rc = recvfrom(s6, buf, sizeof(buf) - 1, 0,
                          (struct sockaddr*)&from, &fromlen);
        else
            abort();
    }

    if (rc > 0) {
        buf[rc] = '\0';
        rc = dht_periodic(buf, rc, (struct sockaddr*)&from, fromlen,
                          tosleep, callback, NULL);
    } else {
        rc = dht_periodic(NULL, 0, NULL, 0, tosleep, callback, NULL);
    }
    if (rc < 0) {
        if (errno == EINTR)
            return 0;
        else {
            perror("dht_periodic");
            if (rc == EINVAL || rc == EFAULT)
                abort();
            *tosleep = 1;
        }
    }

    /* This is how you trigger a search for a torrent hash.  If port
       (the second argument) is non-zero, it also performs an announce.
       Since peers expire announced data after 30 minutes, it's a good
       idea to reannounce every 28 minutes or so. */
    if ((time(NULL) - last_run) > 300) {
        printf("triggering search\n");
        if (s >= 0)
            dht_search(hash, port, AF_INET, callback, NULL);
        if (s6 >= 0)
            dht_search(hash, port, AF_INET6, callback, NULL);
        last_run = time(NULL);
    }

    return 0;
}

int main(int argc, char **argv) {
    int i, rc, fd;
    int s = -1, s6 = -1, port;
    int have_id = 0;
    unsigned char myid[20];
    time_t tosleep = 0;
    char *hash_input = "default";
    int opt;
    int quiet = 0, ipv4 = 1, ipv6 = 1;
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;
    SHA1_CTX sha;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;

    while (1) {
        opt = getopt(argc, argv, "q46b:h:");
        if (opt < 0)
            break;

        switch (opt) {
            case 'q': quiet = 1; break;
            case '4': ipv6 = 0; break;
            case '6': ipv4 = 0; break;
            case 'b': {
                char buf[16];
                int rc;
                rc = inet_pton(AF_INET, optarg, buf);
                if (rc == 1) {
                    memcpy(&sin.sin_addr, buf, 4);
                    break;
                }
                rc = inet_pton(AF_INET6, optarg, buf);
                if (rc == 1) {
                    memcpy(&sin6.sin6_addr, buf, 16);
                    break;
                }
                goto usage;
            }
            case 'h':
                hash_input = optarg;
                break;
            default:
                goto usage;
        }
    }

    SHA1Init(&sha);
    SHA1Update(&sha, (uint8_t *)hash_input, strlen(hash_input));
    SHA1Final(hash, &sha);

    printf("Peering with infohash: ");
    for (i = 0; i < 20; i++) {
        printf("%02x", hash[i]);
    }
    putchar('\n');

    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        perror("open(random)");
        exit(1);
    }

    rc = read(fd, myid, 20);
    if (rc < 0) {
        perror("read(random)");
        exit(1);
    }
    close(fd);

    {
        unsigned seed;
        read(fd, &seed, sizeof(seed));
        srandom(seed);
    }

    if (argc < 2)
        goto usage;

    i = optind;

    if (argc < i + 1)
        goto usage;

    port = atoi(argv[i++]);
    if (port <= 0 || port >= 0x10000)
        goto usage;

    while (i < argc) {
        struct addrinfo hints, *info, *infop;
        memset(&hints, 0, sizeof(hints));
        hints.ai_socktype = SOCK_DGRAM;
        if (!ipv6)
            hints.ai_family = AF_INET;
        else if (!ipv4)
            hints.ai_family = AF_INET6;
        else
            hints.ai_family = 0;
        rc = getaddrinfo(argv[i], argv[i + 1], &hints, &info);
        if (rc != 0) {
            fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
            exit(1);
        }

        i++;
        if (i >= argc)
            goto usage;

        infop = info;
        while (infop) {
            memcpy(&bootstrap_nodes[num_bootstrap_nodes],
                   infop->ai_addr, infop->ai_addrlen);
            infop = infop->ai_next;
            num_bootstrap_nodes++;
        }
        freeaddrinfo(info);

        i++;
    }

    /* If you set dht_debug to a stream, every action taken by the DHT will
       be logged. */
    if (!quiet)
        dht_debug = stdout;

    /* We need an IPv4 and an IPv6 socket, bound to a stable port.  Rumour
       has it that uTorrent works better when it is the same as your
       Bittorrent port. */
    if (ipv4) {
        s = socket(PF_INET, SOCK_DGRAM, 0);
        if (s < 0) {
            perror("socket(IPv4)");
        }
    }

    if (ipv6) {
        s6 = socket(PF_INET6, SOCK_DGRAM, 0);
        if (s6 < 0) {
            perror("socket(IPv6)");
        }
    }

    if (s < 0 && s6 < 0) {
        fprintf(stderr, "Eek!");
        exit(1);
    }

    if (s >= 0) {
        sin.sin_port = htons(port);
        rc = bind(s, (struct sockaddr*)&sin, sizeof(sin));
        if (rc < 0) {
            perror("bind(IPv4)");
            exit(1);
        }
    }

    if (s6 >= 0) {
        int rc;
        int val = 1;

        rc = setsockopt(s6, IPPROTO_IPV6, IPV6_V6ONLY,
                        (char *)&val, sizeof(val));
        if (rc < 0) {
            perror("setsockopt(IPV6_V6ONLY)");
            exit(1);
        }

        /* BEP-32 mandates that we should bind this socket to one of our
           global IPv6 addresses.  In this simple example, this only
           happens if the user used the -b flag. */

        sin6.sin6_port = htons(port);
        rc = bind(s6, (struct sockaddr*)&sin6, sizeof(sin6));
        if (rc < 0) {
            perror("bind(IPv6)");
            exit(1);
        }
    }

    /* Init the dht.  This sets the socket into non-blocking mode. */
    rc = dht_init(s, s6, myid, (unsigned char*)"NT\0\0");
    if (rc < 0) {
        perror("dht_init");
        exit(1);
    }

    init_signals();

    /* For bootstrapping, we need an initial list of nodes.  This could be
       hard-wired, but can also be obtained from the nodes key of a torrent
       file, or from the PORT bittorrent message.

       Dht_ping_node is the brutal way of bootstrapping -- it actually
       sends a message to the peer.  If you're going to bootstrap from
       a massive number of nodes (for example because you're restoring from
       a dump) and you already know their ids, it's better to use
       dht_insert_node.  If the ids are incorrect, the DHT will recover. */
    for (i = 0; i < num_bootstrap_nodes; i++) {
        dht_insert_node(myid, (struct sockaddr*)&bootstrap_nodes[i], sizeof(bootstrap_nodes[i]));
        usleep(random() % 100000);
    }

    while (!run_resolver(hash, &tosleep, s, s6, port)) {
        struct sockaddr_in sin[500];
        struct sockaddr_in6 sin6[500];
        int num = 500, num6 = 500;
        int i;
        i = dht_get_nodes(sin, &num, sin6, &num6);
        printf("Found %d (%d + %d) good nodes.\n", i, num, num6);
    }

    dht_uninit();
    return 0;

usage:
    printf("Usage: dht-example [-q] [-4] [-6] [-b address] [-h infohash]...\n"
           "                   port [address port]...\n");
    exit(1);
}

/* Functions called by the DHT. */

int dht_blacklisted(const struct sockaddr *sa, int salen) {
    return 0;
}

/* We need to provide a reasonably strong cryptographic hashing function.
   Here's how we'd do it if we had RSA's MD5 code. */
#if 0
void dht_hash(void *hash_return, int hash_size,
              const void *v1, int len1,
              const void *v2, int len2,
              const void *v3, int len3) {
    static MD5_CTX ctx;
    MD5Init(&ctx);
    MD5Update(&ctx, v1, len1);
    MD5Update(&ctx, v2, len2);
    MD5Update(&ctx, v3, len3);
    MD5Final(&ctx);
    if (hash_size > 16)
        memset((char*)hash_return + 16, 0, hash_size - 16);
    memcpy(hash_return, ctx.digest, hash_size > 16 ? 16 : hash_size);
}
#else
/* But for this example, we might as well use something weaker. */
void dht_hash(void *hash_return, int hash_size,
              const void *v1, int len1,
              const void *v2, int len2,
              const void *v3, int len3) {
    const char *c1 = v1, *c2 = v2, *c3 = v3;
    char key[9];                /* crypt is limited to 8 characters */
    int i;

    memset(key, 0, 9);
#define CRYPT_HAPPY(c) ((c % 0x60) + 0x20)

    for (i = 0; i < 2 && i < len1; i++)
        key[i] = CRYPT_HAPPY(c1[i]);
    for (i = 0; i < 4 && i < len1; i++)
        key[2 + i] = CRYPT_HAPPY(c2[i]);
    for (i = 0; i < 2 && i < len1; i++)
        key[6 + i] = CRYPT_HAPPY(c3[i]);
    strncpy(hash_return, crypt(key, "jc"), hash_size);
}
#endif

int dht_random_bytes(void *buf, size_t size) {
    int fd, rc, save;

    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;

    rc = read(fd, buf, size);

    save = errno;
    close(fd);
    errno = save;

    return rc;
}
