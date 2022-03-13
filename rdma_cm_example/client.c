/*
 * build:
 * cc -o client client.c -lrdmacm -libverbs
 *
 * usage:
 * client <clientip> <serverip> <serverport> <val1> <val2>
 *
 * connects to server, sends val1 via RDMA write and val2 via send,
 * and receives val1+val2 back from the server.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <rdma/rdma_cma.h>
enum
{
    RESOLVE_TIMEOUT_MS = 5000,
};
struct pdata
{
    uint64_t buf_va;
    uint32_t buf_rkey;
};

static char serverip[INET_ADDRSTRLEN];
static char clientip[INET_ADDRSTRLEN];
union my_address
{
    struct sockaddr_in addr_in;
    struct sockaddr addr;
};


/*static int parse_sockaddr_from_string(struct sockaddr_in *saddr, char *ip)
{
  uint8_t *addr = (uint8_t *)&saddr->sin_addr.s_addr;
  size_t buflen = strlen(ip);

  if (buflen > INET_ADDRSTRLEN)
    return -EINVAL;
  if (in4_pton(ip, buflen, addr, '\0', NULL) == 0)
    return -EINVAL;
  saddr->sin_family = AF_INET;
  return 0;
}*/

int main(int argc, char *argv[])
{
    struct pdata server_pdata;
    struct rdma_event_channel *cm_channel;
    struct rdma_cm_id *cm_id;
    struct rdma_cm_event *event;
    struct rdma_conn_param conn_param = {};
    struct ibv_pd *pd;
    struct ibv_comp_channel *comp_chan;
    struct ibv_cq *cq;
    struct ibv_cq *evt_cq;
    struct ibv_mr *mr;
    struct ibv_qp_init_attr qp_attr = {};
    struct ibv_sge sge;
    struct ibv_send_wr send_wr = {};
    struct ibv_send_wr *bad_send_wr;
    struct ibv_recv_wr recv_wr = {};
    struct ibv_recv_wr *bad_recv_wr;
    struct ibv_wc wc;
    void *cq_context;
    struct addrinfo *res, *t;
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM};
    int n;
    uint32_t *buf;
    int err;
    /* Set up RDMA CM structures */
    cm_channel = rdma_create_event_channel();
    if (!cm_channel)
    {
        printf("rdma_create_event_channel failed!\n");
        return 1;
    }
    err = rdma_create_id(cm_channel, &cm_id, NULL, RDMA_PS_TCP);
    if (err)
    {
        printf("rdma_create_id failed!\n");
        return err;
    }
    /*n = getaddrinfo(argv[1], "20079", &hints, &res);
    if (n < 0)
    {
        printf("getaddrinfo failed!\n");
        return 1;
    }*/

    /* Resolve server address and route */
    /*for (t = res; t; t = t->ai_next)
    {
        err = rdma_resolve_addr(cm_id, NULL, t->ai_addr, RESOLVE_TIMEOUT_MS);
        if (!err)
            break;
    }
    if (err)
    {
        printf("rdma_resolve_addr failed!\n");
        return err;
    }*/
    union my_address cli_addr;
    union my_address srv_addr;
    char *cli_ip = argv[1];
    char *srv_ip = argv[2];
    long srv_port = strtol(argv[3], NULL, 10);
    //err = parse_sockaddr_from_string(&cli_addr, cli_ip);
    cli_addr.addr_in.sin_addr.s_addr = inet_addr(cli_ip);
	cli_addr.addr_in.sin_family = AF_INET;
    srv_addr.addr_in.sin_addr.s_addr = inet_addr(srv_ip);
	srv_addr.addr_in.sin_family = AF_INET;
    /*if (err) {
        printf("parse client addr %s failed\n", cli_ip);
        return err;
    }
    err = parse_sockaddr_from_string(&srv_addr, srv_ip);
    if (err) {
        printf("parse server addr %s failed\n", cli_ip);
        return err;
    }*/
    srv_addr.addr_in.sin_port = htons(srv_port);
    printf("server port input is %s, long value is %d, sin_port:%d\n",argv[3], srv_port, srv_addr.addr_in.sin_port);
    err  = rdma_resolve_addr(cm_id, &cli_addr, &srv_addr, RESOLVE_TIMEOUT_MS);
    if (err) {
        printf("rdma_resolve_addr failed, cli_ip:%s ,srv_ip:%s\n",cli_ip, srv_ip);
		printf("errno: %d, error msg: %s\n", errno, strerror(errno));	
        return err;
    }
    err = rdma_get_cm_event(cm_channel, &event);
    if (err)
    {
        printf("rdma_get_cm_event failed!\n");
        return err;
    }
    if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED)
    {
        printf("RDMA_CM_EVENT_ADDR_RESOLVED failed!\n");
        return 1;
    }
    rdma_ack_cm_event(event);
    err = rdma_resolve_route(cm_id, RESOLVE_TIMEOUT_MS);
    if (err)
    {
        printf("rdma_resolve_route failed!\n");
        return err;
    }
    err = rdma_get_cm_event(cm_channel, &event);
    if (err)
    {
        printf("rdma_get_cm_event failed!\n");
        return err;
    }
    if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED)
    {
        printf("RDMA_CM_EVENT_ROUTE_RESOLVED failed!\n");
        return 1;
    }
    rdma_ack_cm_event(event);

    /* Create verbs objects now that we know which device to use */
    pd = ibv_alloc_pd(cm_id->verbs);
    if (!pd)
    {
        printf("ibv_alloc_pd failed!\n");
        return 1;
    }
    comp_chan = ibv_create_comp_channel(cm_id->verbs);
    if (!comp_chan)
    {
        printf("ibv_create_comp_channel failed!\n");
        return 1;
    }
    cq = ibv_create_cq(cm_id->verbs, 2, NULL, comp_chan, 0);
    if (!cq)
    {
        printf("ibv_create_cq failed!\n");
        return 1;
    }

    if (ibv_req_notify_cq(cq, 0))
    {
        printf("ibv_req_notify_cq failed!\n");
        return 1;
    }

    buf = calloc(2, sizeof(uint32_t));
    if (!buf)
    {
        printf("calloc failed!\n");
        return 1;
    }
    mr = ibv_reg_mr(pd, buf, 2 * sizeof(uint32_t), IBV_ACCESS_LOCAL_WRITE);
    if (!mr)
    {
        printf("ibv_reg_mr failed!\n");
        return 1;
    }
    qp_attr.cap.max_send_wr = 2;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_wr = 1;
    qp_attr.cap.max_recv_sge = 1;
    qp_attr.send_cq = cq;
    qp_attr.recv_cq = cq;
    qp_attr.qp_type = IBV_QPT_RC;
    err = rdma_create_qp(cm_id, pd, &qp_attr);
    if (err)
    {
        printf("rdma_create_qp failed!\n");
        return err;
    }
    conn_param.initiator_depth = 1;
    conn_param.retry_count = 7;

    /* Connect to server */
    err = rdma_connect(cm_id, &conn_param);
    if (err)
    {
        printf("rdma_connect failed!\n");
        return err;
    }
    err = rdma_get_cm_event(cm_channel, &event);
    if (err)
    {
        printf("rdma_get_cm_event failed!\n");
        return err;
    }
    if (event->event != RDMA_CM_EVENT_ESTABLISHED)
    {
        printf("RDMA_CM_EVENT_ESTABLISHED failed!\n");
        return 1;
    }
    memcpy(&server_pdata, event->param.conn.private_data, sizeof(server_pdata));
    rdma_ack_cm_event(event);

    /* Prepost receive */
    sge.addr = (uintptr_t)buf;
    sge.length = sizeof(uint32_t);
    sge.lkey = mr->lkey;
    recv_wr.wr_id = 0;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;
    if (ibv_post_recv(cm_id->qp, &recv_wr, &bad_recv_wr))
    {
        printf("ibv_post_recv failed!\n");
        return 1;
    }

    /* Write/send two integers to be added */
    buf[0] = strtoul(argv[2], NULL, 0);
    buf[1] = strtoul(argv[3], NULL, 0);
    printf("%d + %d = ", buf[0], buf[1]);
    buf[0] = htonl(buf[0]);
    buf[1] = htonl(buf[1]);

    sge.addr = (uintptr_t)buf;
    sge.length = sizeof(uint32_t);
    sge.lkey = mr->lkey;
    send_wr.wr_id = 1;
    send_wr.opcode = IBV_WR_RDMA_WRITE;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.wr.rdma.rkey = ntohl(server_pdata.buf_rkey);
    // FIXME: Here should be ntohll?
    send_wr.wr.rdma.remote_addr = ntohl(server_pdata.buf_va);
    if (ibv_post_send(cm_id->qp, &send_wr, &bad_send_wr))
    {
        printf("ibv_post_send failed!\n");
        return 1;
    }
    sge.addr = (uintptr_t)buf + sizeof(uint32_t);
    sge.length = sizeof(uint32_t);
    sge.lkey = mr->lkey;
    send_wr.wr_id = 2;
    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    if (ibv_post_send(cm_id->qp, &send_wr, &bad_send_wr))
    {
        printf("ibv_post_send failed!\n");
        return 1;
    }

    /* Wait for receive completion */
    while (1)
    {
        if (ibv_get_cq_event(comp_chan, &evt_cq, &cq_context))
        {
            printf("ibv_get_cq_event failed!\n");
            return 1;
        }
        if (ibv_req_notify_cq(cq, 0))
        {
            printf("ibv_req_notify_cq failed!\n");
            return 1;
        }
        if (ibv_poll_cq(cq, 1, &wc) != 1)
        {
            printf("ibv_poll_cq failed!\n");
            return 1;
        }
        if (wc.status != IBV_WC_SUCCESS)
        {
            printf("wc.status != IBV_WC_SUCCESS failed!\n");
            return 1;
        }
        if (wc.wr_id == 0)
        {
            printf("%d\n", ntohl(buf[0]));

            return 0;
        }
    }

    printf(" failed!\n");
    return 0;
}
