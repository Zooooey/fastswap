/*
 * build:
 * cc -o server server.c -lrdmacm -libverbs
 *
 * usage:
 * server
 *
 * Waiting client using one-side rdma_read to read a num. 
 * After this num was retrived from client, client will change this num to zero by one-side rdma_write.
 */
#include <stdlib.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>
#include <unistd.h>

enum
{
    RESOLVE_TIMEOUT_MS = 5000,
};

struct pdata
{
    uint64_t buf_va;
    uint32_t buf_rkey;
};
int main(int argc, char *argv[])
{
    struct pdata rep_pdata;
    struct rdma_event_channel *cm_channel;
    struct rdma_cm_id *listen_id;
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
    struct ibv__end_wr *bad_send_wr;
    struct ibv_recv_wr recv_wr = {};
    struct recv_wr *bad_recv_wr;
    struct ibv_wc wc;
    void *cq_context;
    struct sockaddr_in sin;
    uint32_t *buf;
    int err;

    /* Set up RDMA CM structures */
    cm_channel = rdma_create_event_channel();
    if (!cm_channel)
    {
        printf("cm_channel init failed!\n");
        return 1;
    }
    err = rdma_create_id(cm_channel, &listen_id, NULL, RDMA_PS_TCP);
    if (err)
    {
        printf("rdma_create_id  failed!\n");
        return err;
    }
    sin.sin_family = AF_INET;
    sin.sin_port = htons(20079);
    sin.sin_addr.s_addr = INADDR_ANY;
    /* Bind to local port and listen for connection request */
    err = rdma_bind_addr(listen_id, (struct sockaddr *)&sin);
    if (err)
    {
        printf("rdma_bind_addr failed! port:%s\n", sin.sin_port);
        return err;
    }
    err = rdma_listen(listen_id, 1);
    if (err)
    {
        printf("rdma_listen failed! \n");
        return err;
    }
    err = rdma_get_cm_event(cm_channel, &event);
    if (err)
    {
        printf("rdma_get_cm_event failed! \n");
        return err;
    }
    if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST)
    {
        printf("event from client is not RDMA_CM_EVENT_CONNECT_REQUEST \n");
        return 1;
    }

    cm_id = event->id;
    rdma_ack_cm_event(event);

    /* Create verbs objects now that we know which device to use */
    pd = ibv_alloc_pd(cm_id->verbs);
    if (!pd)
    {
        printf("ibv_alloc_pd failed\n");
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
        printf("ibv_create_cq failed\n");
        return 1;
    }

    if (ibv_req_notify_cq(cq, 0))
    {
        printf("ibv_req_notify_cq faild!\n");
        return 1;
    }
    buf = calloc(2, sizeof(uint32_t));
    if (!buf)
    {
        printf("calloc faild!\n");
        return 1;
    }

    mr = ibv_reg_mr(pd, buf, 2 * sizeof(uint32_t),
                    IBV_ACCESS_LOCAL_WRITE |
                        IBV_ACCESS_REMOTE_READ |
                        IBV_ACCESS_REMOTE_WRITE);
    if (!mr)
    {
        printf("ibv_reg_mr failed\n");
        return 1;
    }
    buf = 3072;
    printf("The num waiting to client retrieving is %d\n",buf);


    printf("The original buf[0] s %d\n",buf[0]);
    qp_attr.cap.max_send_wr = 10;
    qp_attr.cap.max_send_sge = 10;
    qp_attr.cap.max_recv_wr = 10;
    qp_attr.cap.max_recv_sge = 10;
    qp_attr.send_cq = cq;
    qp_attr.recv_cq = cq;
    qp_attr.qp_type = IBV_QPT_RC;

    err = rdma_create_qp(cm_id, pd, &qp_attr);
    if (err)
    {
        printf("rdma_create_qp failed\n");
        return err;
    }


    // FIXME:should be htonl?
    rep_pdata.buf_va = htonl((uintptr_t)buf);
    rep_pdata.buf_rkey = htonl(mr->rkey);
    conn_param.responder_resources = 1;
    conn_param.private_data = &rep_pdata;
    conn_param.private_data_len = sizeof rep_pdata;

    /* Accept connection */
    err = rdma_accept(cm_id, &conn_param);
    if (err)
    {
        printf("rdma_accept failed\n");
        return 1;
    }
    err = rdma_get_cm_event(cm_channel, &event);
    if (err)
    {
        printf("rdma_get_cm_event failed\n");
        return err;
    }

    if (event->event != RDMA_CM_EVENT_ESTABLISHED)
    {
        printf("rdma_get_cm_event failed\n");
        return 1;
    }

    rdma_ack_cm_event(event);
    
    printf("Wating the client to change the num....\n");
    while(1){
	sleep(1);
    	printf("The number is:%d\n",buf[0]);
    }
    printf("The number is:%d\n",buf[0]);
    printf("test success!\n");
    return 0;
}
