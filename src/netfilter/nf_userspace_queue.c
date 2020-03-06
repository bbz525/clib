#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>

#include <libmnl/libmnl.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nfnetlink.h>

#include <linux/types.h>
#include <linux/netfilter/nfnetlink_queue.h>

#include <libnetfilter_queue/libnetfilter_queue.h>

/* only for NFQA_CT, not needed otherwise: */
#include <linux/netfilter/nfnetlink_conntrack.h>

static struct mnl_socket *nl;

/**
 * @brief 构建nlmsghdr并放入指定内存
 * @param buf 指定数据内存指针
 * @param type 消息类型
 * @param queue_num 队列号
 * @return 构建的nlmsghdr指针，该指针指向的数据内存区域就是在buf的内存区域
 */
static struct nlmsghdr *
nfq_hdr_put(char *buf, int type, uint32_t queue_num)
{
    struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type    = (NFNL_SUBSYS_QUEUE << 8) | type;
    nlh->nlmsg_flags = NLM_F_REQUEST;

    struct nfgenmsg *nfg = mnl_nlmsg_put_extra_header(nlh, sizeof(*nfg));
    nfg->nfgen_family = AF_UNSPEC;
    nfg->version = NFNETLINK_V0;
    nfg->res_id = htons(queue_num);

    return nlh;
}

/**
 * @brief 发送决策
 * @param queue_num 队列号
 * @param id id
 * @param plen 数据长度
 * @param sendData ip数据报文，要符合ip报文规范
 */
static void
nfq_send_verdict(int queue_num, uint32_t id, uint16_t plen, void *sendData)
{
    char buf[MNL_SOCKET_BUFFER_SIZE];
    struct nlmsghdr *nlh;
    struct nlattr *nest, *data;

    nlh = nfq_hdr_put(buf, NFQNL_MSG_VERDICT, queue_num);
    nfq_nlmsg_verdict_put(nlh, id, NF_ACCEPT);

    /* example to set the connmark. First, start NFQA_CT section: */
    nest = mnl_attr_nest_start(nlh, NFQA_CT);
    /* then, add the connmark attribute: */
    mnl_attr_put_u32(nlh, CTA_MARK, htonl(42));
    /* more conntrack attributes, e.g. CTA_LABELS could be set here */
    /* end conntrack section */
    mnl_attr_nest_end(nlh, nest);

    // 放入数据
    mnl_attr_put(nlh, NFQA_PAYLOAD, plen, sendData);

    if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
        perror("mnl_socket_send");
        exit(EXIT_FAILURE);
    }
}

static void printIp(char *ip){
    char start = 0;
    printf("\n");
    for(; start < 4; start++){
      unsigned char num = *ip;
      printf("%d.", num);
      ip++;
    }
    printf("\n");
}


static unsigned int getIp(unsigned char *ip){
    unsigned int *data;
    data = (unsigned int *)ip;
    return ntohl(*data);
}

/**
 * @brief 收到内核消息的回调，当内核收到报文然后放入队列后会发出消息，回调到这里
 * @param nlh 消息头
 * @param data 回传数据，mnl_cb_run函数传进来的指针，可以用于返回数据等，这里传的是null，不需要回传数据
 * @return 返回大于等于1表示成功，小于等于-1表示失败，0表示要停止回调
 */
static int queue_cb(const struct nlmsghdr *nlh, void *data)
{
    struct nfqnl_msg_packet_hdr *ph = NULL;
    struct nlattr *attr[NFQA_MAX+1] = {};
    uint32_t id = 0, skbinfo;
    struct nfgenmsg *nfg;
    uint16_t plen;
    int queue_num, hook;
    unsigned char *src, *dest;
    unsigned int srcAdd, destAdd;

    // 解析参数列表
    if (nfq_nlmsg_parse(nlh, attr) < 0) {
        perror("problems parsing");
        return MNL_CB_ERROR;
    }

    // 获取消息体
    nfg = mnl_nlmsg_get_payload(nlh);
    // 队列号
    queue_num = ntohs(nfg->res_id);

    if (attr[NFQA_PACKET_HDR] == NULL) {
        fputs("metaheader not set\n", stderr);
        return MNL_CB_ERROR;
    }

    ph = mnl_attr_get_payload(attr[NFQA_PACKET_HDR]);
    // 对应的内核hook点，有可能内核多个hook点都加入到了该队列，这时可以用这个区分
    hook = ph->hook;

    // ip报文长度
    plen = mnl_attr_get_payload_len(attr[NFQA_PAYLOAD]);
    // 标准的ip报文
    unsigned char *payload = mnl_attr_get_payload(attr[NFQA_PAYLOAD]);

    src = payload + 12;
    dest = payload + 16;

    srcAdd = getIp(src);
    destAdd = getIp(dest);

    skbinfo = attr[NFQA_SKB_INFO] ? ntohl(mnl_attr_get_u32(attr[NFQA_SKB_INFO])) : 0;

    if (attr[NFQA_CAP_LEN]) {
        uint32_t orig_len = ntohl(mnl_attr_get_u32(attr[NFQA_CAP_LEN]));
        if (orig_len != plen)
            printf("truncated ");
    }

    if (skbinfo & NFQA_SKB_GSO)
        printf("GSO ");

    id = ntohl(ph->packet_id);

    // 转发到192.168.199.189
    unsigned int forwarded = 0xc0a8c7bd;

    if (srcAdd != -1062680643){
        // 本机地址：3232286594
        printf("源地址：");
        printIp(src);
        printf("目标地址：");
        printIp(dest);
        printf("源地址int类型是：%d\n", srcAdd);
        printf("目标地址int类型是：%d\n", destAdd);
        // 修改目标地址为主机
        struct in_addr rcv;
        inet_aton("192.168.199.189", &rcv);
        memcpy(payload + 16, &rcv, sizeof(rcv));
        printf("packet received (id=%u hw=0x%04x hook=%u, payload len %u\n",
              id, ntohs(ph->hw_protocol), ph->hook, plen);
        printf("\n\n\n");
    }


    /*
     * ip/tcp checksums are not yet valid, e.g. due to GRO/GSO.
     * The application should behave as if the checksums are correct.
     *
     * If these packets are later forwarded/sent out, the checksums will
     * be corrected by kernel/hardware.
     */
    if (skbinfo & NFQA_SKB_CSUMNOTREADY)
        printf(", checksum not ready");

    nfq_send_verdict(ntohs(nfg->res_id), id, plen, payload);

    return MNL_CB_OK;
}

int main(int argc, char *argv[])
{
    char *buf;
    /* largest possible packet payload, plus netlink data overhead: */
    size_t sizeof_buf = 0xffff + (MNL_SOCKET_BUFFER_SIZE/2);
    struct nlmsghdr *nlh;
    int ret;
    unsigned int portid, queue_num;

    if (argc != 2) {
        printf("Usage: %s [queue_num]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    queue_num = atoi(argv[1]);
    printf("queue_num : %d \n", queue_num);

    nl = mnl_socket_open(NETLINK_NETFILTER);
    if (nl == NULL) {
        perror("mnl_socket_open");
        exit(EXIT_FAILURE);
    }

    if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
        perror("mnl_socket_bind");
        exit(EXIT_FAILURE);
    }
    portid = mnl_socket_get_portid(nl);

    buf = malloc(sizeof_buf);
    if (!buf) {
        perror("allocate receive buffer");
        exit(EXIT_FAILURE);
    }

    nlh = nfq_hdr_put(buf, NFQNL_MSG_CONFIG, queue_num);
    nfq_nlmsg_cfg_put_cmd(nlh, AF_INET, NFQNL_CFG_CMD_BIND);

    if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
        perror("mnl_socket_send");
        exit(EXIT_FAILURE);
    }

    nlh = nfq_hdr_put(buf, NFQNL_MSG_CONFIG, queue_num);
    nfq_nlmsg_cfg_put_params(nlh, NFQNL_COPY_PACKET, 0xffff);

    mnl_attr_put_u32(nlh, NFQA_CFG_FLAGS, htonl(NFQA_CFG_F_GSO));
    mnl_attr_put_u32(nlh, NFQA_CFG_MASK, htonl(NFQA_CFG_F_GSO));

    if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
        perror("mnl_socket_send");
        exit(EXIT_FAILURE);
    }

    /* ENOBUFS is signalled to userspace when packets were lost
     * on kernel side.  In most cases, userspace isn't interested
     * in this information, so turn it off.
     */
    ret = 1;
    mnl_socket_setsockopt(nl, NETLINK_NO_ENOBUFS, &ret, sizeof(int));

    for (;;) {
        ret = mnl_socket_recvfrom(nl, buf, sizeof_buf);
        if (ret == -1) {
            perror("mnl_socket_recvfrom");
            exit(EXIT_FAILURE);
        }

        ret = mnl_cb_run(buf, ret, 0, portid, queue_cb, NULL);
        if (ret < 0){
            perror("mnl_cb_run");
            exit(EXIT_FAILURE);
        }
    }

    mnl_socket_close(nl);

    return 0;
}