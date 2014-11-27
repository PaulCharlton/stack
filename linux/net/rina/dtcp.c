/*
 * DTCP (Data Transfer Control Protocol)
 *
 *    Francesco Salvestrini <f.salvestrini@nextworks.it>
 *    Miquel Tarzan         <miquel.tarzan@i2cat.net>
 *    Leonardo Bergesio     <leonardo.bergesio@i2cat.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define RINA_PREFIX "dtcp"

#include "logs.h"
#include "utils.h"
#include "debug.h"
#include "dtcp.h"
#include "rmt.h"
#include "connection.h"
#include "dtp.h"
#include "dt-utils.h"
#include "dtcp-utils.h"
#include "ps-factory.h"
#include "dtcp-ps.h"

static struct policy_set_list policy_sets = {
        .head = LIST_HEAD_INIT(policy_sets.head)
};

/* This is the DT-SV part maintained by DTCP */
struct dtcp_sv {
        /* SV lock */
        spinlock_t   lock;

        /* TimeOuts */
        /*
         * When flow control is rate based this timeout may be
         * used to pace number of PDUs sent in TimeUnit
         */
        uint_t       pdus_per_time_unit;

        /* Sequencing */

        /*
         * Outbound: NextSndCtlSeq contains the Sequence Number to
         * be assigned to a control PDU
         */
        seq_num_t    next_snd_ctl_seq;

        /*
         * Inbound: LastRcvCtlSeq - Sequence number of the next
         * expected // Transfer(? seems an error in the spec’s
         * doc should be Control) PDU received on this connection
         */
        seq_num_t    last_rcv_ctl_seq;

        /*
         * Retransmission: There’s no retransmission queue,
         * when a lost PDU is detected a new one is generated
         */

        /* Outbound */
        seq_num_t    last_snd_data_ack;

        /*
         * Seq number of the lowest seq number expected to be
         * Acked. Seq number of the first PDU on the
         * RetransmissionQ. My LWE thus.
         */
        seq_num_t    snd_lft_win;

        /*
         * Maximum number of retransmissions of PDUs without a
         * positive ack before declaring an error
         */
        uint_t       data_retransmit_max;

        /* Inbound */
        seq_num_t    last_rcv_data_ack;

        /* Time (ms) over which the rate is computed */
        uint_t       time_unit;

        /* Flow Control State */

        /* Outbound */
        uint_t       sndr_credit;

        /* snd_rt_wind_edge = LastSendDataAck + PDU(credit) */
        seq_num_t    snd_rt_wind_edge;

        /* PDUs per TimeUnit */
        uint_t       sndr_rate;

        /* PDUs already sent in this time unit */
        uint_t       pdus_sent_in_time_unit;

        /* Inbound */

        /*
         * PDUs receiver believes sender may send before extending
         * credit or stopping the flow on the connection
         */
        uint_t       rcvr_credit;

        /* Value of credit in this flow */
        seq_num_t    rcvr_rt_wind_edge;

        /*
         * Current rate receiver has told sender it may send PDUs
         * at.
         */
        uint_t       rcvr_rate;

        /*
         * PDUs received in this time unit. When it equals
         * rcvr_rate, receiver is allowed to discard any PDUs
         * received until a new time unit begins
         */
        uint_t       pdus_rcvd_in_time_unit;

        /*
         * Control of duplicated control PDUs
         * */
        uint_t       acks;
        uint_t       flow_ctl;
};

struct dtcp {
        struct dt *            parent;

        /*
         * NOTE: The DTCP State Vector can be discarded during long periods of
         *       no traffic
         */
        struct dtcp_sv *       sv; /* The state-vector */
        struct rina_component  base;
        struct connection *    conn;
        struct rmt *           rmt;

        /* FIXME: Add QUEUE(flow_control_queue, pdu) */
        /* FIXME: Add QUEUE(closed_window_queue, pdu) */
        /* FIXME: Add QUEUE(rx_control_queue, ...) */
};

struct dt * dtcp_dt(struct dtcp * dtcp)
{
        return dtcp->parent;
}
EXPORT_SYMBOL(dtcp_dt);

struct dtcp_config * dtcp_config_get(struct dtcp * dtcp)
{
        if (!dtcp)
                return NULL;
        if (!dtcp->conn)
                return NULL;
        if (!dtcp->conn->policies_params)
                return NULL;
        return dtcp->conn->policies_params->dtcp_cfg;
}
EXPORT_SYMBOL(dtcp_config_get);

int pdu_send(struct dtcp * dtcp, struct pdu * pdu)
{
        ASSERT(dtcp);
        ASSERT(pdu);

        if (rmt_send(dtcp->rmt,
                     dtcp->conn->destination_address,
                     dtcp->conn->qos_id,
                     pdu))
                return -1;

        return 0;
}
EXPORT_SYMBOL(pdu_send);

static int last_rcv_ctrl_seq_set(struct dtcp * dtcp,
                                 seq_num_t     last_rcv_ctrl_seq)
{
        ASSERT(dtcp);
        ASSERT(dtcp->sv);

        spin_lock(&dtcp->sv->lock);
        dtcp->sv->last_rcv_ctl_seq = last_rcv_ctrl_seq;
        spin_unlock(&dtcp->sv->lock);

        return 0;
}

seq_num_t last_rcv_ctrl_seq(struct dtcp * dtcp)
{
        seq_num_t tmp;

        ASSERT(dtcp);
        ASSERT(dtcp->sv);

        spin_lock(&dtcp->sv->lock);
        tmp = dtcp->sv->last_rcv_ctl_seq;
        spin_unlock(&dtcp->sv->lock);

        return tmp;
}
EXPORT_SYMBOL(last_rcv_ctrl_seq);

static void flow_ctrl_inc(struct dtcp * dtcp)
{
        ASSERT(dtcp);
        ASSERT(dtcp->sv);

        spin_lock(&dtcp->sv->lock);
        dtcp->sv->flow_ctl++;
        spin_unlock(&dtcp->sv->lock);
}

static void acks_inc(struct dtcp * dtcp)
{
        ASSERT(dtcp);
        ASSERT(dtcp->sv);

        spin_lock(&dtcp->sv->lock);
        dtcp->sv->acks++;
        spin_unlock(&dtcp->sv->lock);
}

static int snd_rt_wind_edge_set(struct dtcp * dtcp, seq_num_t new_rt_win)
{
        ASSERT(dtcp);
        ASSERT(dtcp->sv);

        spin_lock(&dtcp->sv->lock);
        dtcp->sv->snd_rt_wind_edge = new_rt_win;
        spin_unlock(&dtcp->sv->lock);

        return 0;
}

seq_num_t snd_rt_wind_edge(struct dtcp * dtcp)
{
        seq_num_t tmp;

        ASSERT(dtcp);
        ASSERT(dtcp->sv);

        spin_lock(&dtcp->sv->lock);
        tmp = dtcp->sv->snd_rt_wind_edge;
        spin_unlock(&dtcp->sv->lock);

        return tmp;
}
EXPORT_SYMBOL(snd_rt_wind_edge);

seq_num_t snd_lft_win(struct dtcp * dtcp)
{
        seq_num_t tmp;

        ASSERT(dtcp);
        ASSERT(dtcp->sv);

        spin_lock(&dtcp->sv->lock);
        tmp = dtcp->sv->snd_lft_win;
        spin_unlock(&dtcp->sv->lock);

        return tmp;
}
EXPORT_SYMBOL(snd_lft_win);

seq_num_t rcvr_rt_wind_edge(struct dtcp * dtcp)
{
        seq_num_t tmp;

        ASSERT(dtcp);
        ASSERT(dtcp->sv);

        spin_lock(&dtcp->sv->lock);
        tmp = dtcp->sv->rcvr_rt_wind_edge;
        spin_unlock(&dtcp->sv->lock);

        return tmp;
}
EXPORT_SYMBOL(rcvr_rt_wind_edge);

static seq_num_t next_snd_ctl_seq(struct dtcp * dtcp)
{
        seq_num_t tmp;

        ASSERT(dtcp);
        ASSERT(dtcp->sv);

        spin_lock(&dtcp->sv->lock);
        tmp = ++dtcp->sv->next_snd_ctl_seq;
        spin_unlock(&dtcp->sv->lock);

        return tmp;
}

static seq_num_t last_snd_data_ack(struct dtcp * dtcp)
{
        seq_num_t tmp;

        ASSERT(dtcp);
        ASSERT(dtcp->sv);

        spin_lock(&dtcp->sv->lock);
        tmp = dtcp->sv->last_snd_data_ack;
        spin_unlock(&dtcp->sv->lock);

        return tmp;
}

static void last_snd_data_ack_set(struct dtcp * dtcp, seq_num_t seq_num)
{
        ASSERT(dtcp);
        ASSERT(dtcp->sv);

        spin_lock(&dtcp->sv->lock);
        dtcp->sv->last_snd_data_ack = seq_num;
        spin_unlock(&dtcp->sv->lock);
}

static int push_pdus_rmt(struct dtcp * dtcp)
{
        struct cwq *  q;

        ASSERT(dtcp);

        q = dt_cwq(dtcp->parent);
        if (!q) {
                LOG_ERR("No Closed Window Queue");
                return -1;
        }

        cwq_deliver(q,
                    dtcp->parent,
                    dtcp->rmt,
                    dtcp->conn->destination_address,
                    dtcp->conn->qos_id);

        return 0;
}

struct pdu * pdu_ctrl_create_ni(struct dtcp * dtcp, pdu_type_t type)
{
        struct pdu *    pdu;
        struct pci *    pci;
        struct buffer * buffer;
        seq_num_t       seq;

        buffer = buffer_create_ni(1);
        if (!buffer)
                return NULL;

        pdu = pdu_create_ni();
        if (!pdu) {
                buffer_destroy(buffer);
                return NULL;
        }

        pci = pci_create_ni();
        if (!pci) {
                pdu_destroy(pdu);
                return NULL;
        }

        seq = next_snd_ctl_seq(dtcp);
        if (pci_format(pci,
                       dtcp->conn->source_cep_id,
                       dtcp->conn->destination_cep_id,
                       dtcp->conn->source_address,
                       dtcp->conn->destination_address,
                       seq,
                       dtcp->conn->qos_id,
                       type)) {
                pdu_destroy(pdu);
                pci_destroy(pci);
                return NULL;
        }

        if (pdu_pci_set(pdu, pci)) {
                pdu_destroy(pdu);
                pci_destroy(pci);
                return NULL;
        }

        if (pdu_buffer_set(pdu, buffer)) {
                pdu_destroy(pdu);
                return NULL;
        }

        return pdu;
}
EXPORT_SYMBOL(pdu_ctrl_create_ni);

/* This is 0x8803 PDU type */
/* NOTE: Specs do not detail it structure */
struct pdu * pdu_ctrl_ack_create(struct dtcp * dtcp,
                                 seq_num_t     last_ctrl_seq_rcvd,
                                 seq_num_t     snd_left_wind_edge,
                                 seq_num_t     snd_rt_wind_edge)
{
        struct pdu * pdu;
        struct pci * pci;

        pdu = pdu_ctrl_create_ni(dtcp, PDU_TYPE_ACK_AND_FC);
        if (!pdu)
                return NULL;

        pci = pdu_pci_get_rw(pdu);
        if (!pci)
                return NULL;

        if (pci_control_last_seq_num_rcvd_set(pci, last_ctrl_seq_rcvd)) {
                pdu_destroy(pdu);
                return NULL;
        }

        return pdu;
}
EXPORT_SYMBOL(pdu_ctrl_ack_create);

/* not a policy according to specs */
static int rcv_nack_ctl(struct dtcp * dtcp, seq_num_t seq_num)
{
        struct rtxq * q;

        if (dtcp_rtx_ctrl(dtcp_config_get(dtcp))) {
                q = dt_rtxq(dtcp->parent);
                if (!q) {
                        LOG_ERR("Couldn't find the Retransmission queue");
                        return -1;
                }
                rtxq_nack(q, seq_num, dt_sv_tr(dtcp->parent));
        }
        return 0;
}

void dump_we(struct dtcp * dtcp, struct pci *  pci)
{
        struct dtp * dtp;
        seq_num_t    snd_rt_we;
        seq_num_t    snd_lf_we;
        seq_num_t    cwq_lf_we;
        seq_num_t    rcv_rt_we;
        seq_num_t    rcv_lf_we;
        seq_num_t    new_rt_we;
        seq_num_t    new_lf_we;
        seq_num_t    pci_seqn;
        seq_num_t    my_rt_we;
        seq_num_t    my_lf_we;

        ASSERT(dtcp);
        ASSERT(pci);

        dtp = dt_dtp(dtcp->parent);
        ASSERT(dtp);

        snd_rt_we = snd_rt_wind_edge(dtcp);
        snd_lf_we = dtcp_snd_lf_win(dtcp);
        cwq_lf_we = cwq_peek(dt_cwq(dtcp->parent));
        rcv_rt_we = rcvr_rt_wind_edge(dtcp);
        rcv_lf_we = dt_sv_rcv_lft_win(dtcp->parent);
        new_rt_we = pci_control_new_rt_wind_edge(pci);
        new_lf_we = pci_control_new_left_wind_edge(pci);
        my_lf_we  = pci_control_my_left_wind_edge(pci);
        my_rt_we  = pci_control_my_rt_wind_edge(pci);
        pci_seqn  = pci_sequence_number_get(pci);

        LOG_INFO("SEQN: %u SndRWE: %u SndLWE: %u RcvRWE: %u RcvLWE: %u"
                 " newRWE: %u newLWE: %u myRWE: %u myLWE: %u cwqLWE: %u",
                 pci_seqn, snd_rt_we, snd_lf_we, rcv_rt_we, rcv_lf_we,
                 new_rt_we, new_lf_we, my_rt_we, my_lf_we, cwq_lf_we);

}
EXPORT_SYMBOL(dump_we);

static int rcv_flow_ctl(struct dtcp * dtcp,
                        struct pci *  pci,
                        struct pdu *  pdu)
{
        struct cwq * q;
        struct dtp * dtp;

        ASSERT(dtcp);
        ASSERT(pci);
        ASSERT(pdu);

        dump_we(dtcp, pci);
        snd_rt_wind_edge_set(dtcp, pci_control_new_rt_wind_edge(pci));
        pdu_destroy(pdu);
        push_pdus_rmt(dtcp);

        dtp = dt_dtp(dtcp->parent);
        if (!dtp) {
                LOG_ERR("No DTP");
                return -1;
        }
        q = dt_cwq(dtcp->parent);
        if (!q) {
                LOG_ERR("No Closed Window Queue");
                return -1;
        }
        if (cwq_is_empty(q) &&
            (dtp_sv_last_seq_nr_sent(dtp) < snd_rt_wind_edge(dtcp))) {
                dt_sv_window_closed_set(dtcp->parent, false);
        }

        return 0;
}

static int rcv_ack_and_flow_ctl(struct dtcp * dtcp,
                                struct pci *  pci,
                                struct pdu *  pdu)
{
        struct dtcp_ps *ps;
        seq_num_t seq;

        ASSERT(dtcp);
        ASSERT(pci);
        ASSERT(pdu);

        LOG_DBG("Updating Window Edges for DTCP: %pK", dtcp);

        dump_we(dtcp, pci);
        seq = pci_control_ack_seq_num(pci);
        LOG_DBG("Ack/Nack SEQ NUM: %u", seq);

        rcu_read_lock();
        ps = container_of(rcu_dereference(dtcp->base.ps),
                          struct dtcp_ps, base);
        /* This updates sender LWE */
        if (ps->sender_ack(ps, seq))
                LOG_ERR("Could not update RTXQ and LWE");
        rcu_read_unlock();

        snd_rt_wind_edge_set(dtcp, pci_control_new_rt_wind_edge(pci));
        LOG_DBG("Right Window Edge: %d", snd_rt_wind_edge(dtcp));
        pdu_destroy(pdu);

        LOG_DBG("Calling CWQ_deliver for DTCP: %pK", dtcp);
        push_pdus_rmt(dtcp);

        /* FIXME: Verify values for the receiver side */

        return 0;
}

int dtcp_common_rcv_control(struct dtcp * dtcp, struct pdu * pdu)
{
        struct dtcp_ps *ps;
        struct pci * pci;
        pdu_type_t   type;
        seq_num_t    seq_num;
        seq_num_t    seq;
        seq_num_t    last_ctrl;
        int ret;

        if (!pdu_is_ok(pdu)) {
                LOG_ERR("PDU is not ok");
                pdu_destroy(pdu);
                return -1;
        }

        if (!dtcp) {
                LOG_ERR("DTCP instance bogus");
                pdu_destroy(pdu);
                return -1;
        }

        pci = pdu_pci_get_rw(pdu);
        if (!pci_is_ok(pci)) {
                LOG_ERR("PCI couldn't be retrieved");
                pdu_destroy(pdu);
                return -1;
        }

        type = pci_type(pci);

        if (!pdu_type_is_control(type)) {
                LOG_ERR("CommonRCVControl policy received a non-control PDU");
                pdu_destroy(pdu);
                return -1;
        }

        seq_num = pci_sequence_number_get(pci);
        last_ctrl = last_rcv_ctrl_seq(dtcp);

        if (seq_num > (last_ctrl + 1)) {
                rcu_read_lock();
                ps = container_of(rcu_dereference(dtcp->base.ps),
                                  struct dtcp_ps, base);
                ps->lost_control_pdu(ps);
                rcu_read_unlock();
        }

        if (seq_num <= last_ctrl) {
                switch (type) {
                case PDU_TYPE_FC:
                        flow_ctrl_inc(dtcp);
                        break;
                case PDU_TYPE_ACK:
                        acks_inc(dtcp);
                        break;
                case PDU_TYPE_ACK_AND_FC:
                        acks_inc(dtcp);
                        flow_ctrl_inc(dtcp);
                        break;
                default:
                        break;
                }

                pdu_destroy(pdu);
                return 0;

        }

        /* We are in seq_num == last_ctrl + 1 */

        last_rcv_ctrl_seq_set(dtcp, seq_num);

        /*
         * FIXME: Missing step described in the specs: retrieve the time
         *        of this Ack and calculate the RTT with RTTEstimator policy
         */

        switch (type) {
        case PDU_TYPE_ACK:
                seq = pci_control_ack_seq_num(pci);

                rcu_read_lock();
                ps = container_of(rcu_dereference(dtcp->base.ps),
                                  struct dtcp_ps, base);
                ret = ps->sender_ack(ps, seq);
                rcu_read_unlock();
                return ret;
        case PDU_TYPE_NACK:
                seq = pci_control_ack_seq_num(pdu_pci_get_ro(pdu));

                return rcv_nack_ctl(dtcp,
                                    seq);
        case PDU_TYPE_FC:
                return rcv_flow_ctl(dtcp, pci, pdu);
        case PDU_TYPE_ACK_AND_FC:
                return rcv_ack_and_flow_ctl(dtcp, pci, pdu);
        default:
                return -1;
        }
}

pdu_type_t pdu_ctrl_type_get(struct dtcp * dtcp, seq_num_t seq)
{
        struct dtcp_config * dtcp_cfg;
        seq_num_t    LWE;
        timeout_t    a;

        ASSERT(dtcp);
        ASSERT(dtcp->parent);

        dtcp_cfg = dtcp_config_get(dtcp);
        ASSERT(dtcp_cfg);

        a = dt_sv_a(dtcp->parent);

        LWE = dt_sv_rcv_lft_win(dtcp->parent);
        if (last_snd_data_ack(dtcp) < LWE) {
                last_snd_data_ack_set(dtcp, LWE);
                if (!a) {
#if 0
                        if (seq > LWE) {
                                LOG_DBG("This is a NACK, "
                                        "LWE couldn't be updated");
                                if (dtcp_flow_ctrl(dtcp_cfg)) {
                                        return PDU_TYPE_NACK_AND_FC;
                                }
                                return PDU_TYPE_NACK;
                        }
#endif
                        LOG_DBG("This is an ACK");
                        if (dtcp_flow_ctrl(dtcp_cfg)) {
                                return PDU_TYPE_ACK_AND_FC;
                        }
                        return PDU_TYPE_ACK;
                }
#if 0
                if (seq > LWE) {
                        /* FIXME: This should be a SEL ACK */
                        LOG_DBG("This is a NACK, "
                                "LWE couldn't be updated");
                        if (dtcp_flow_ctrl(dtcp_cfg)) {
                                return PDU_TYPE_NACK_AND_FC;
                        }
                        return PDU_TYPE_NACK;
                }
#endif
                LOG_DBG("This is an ACK");
                if (dtcp_flow_ctrl(dtcp_cfg)) {
                        return PDU_TYPE_ACK_AND_FC;
                }
                return PDU_TYPE_ACK;
        }

        return 0;
}
EXPORT_SYMBOL(pdu_ctrl_type_get);

void update_rt_wind_edge(struct dtcp * dtcp)
{
        seq_num_t seq;

        ASSERT(dtcp);
        ASSERT(dtcp->sv);

        seq = dt_sv_rcv_lft_win(dtcp->parent);
        spin_lock(&dtcp->sv->lock);
        seq += dtcp->sv->rcvr_credit;
        dtcp->sv->rcvr_rt_wind_edge = seq;
        spin_unlock(&dtcp->sv->lock);
}
EXPORT_SYMBOL(update_rt_wind_edge);

static struct dtcp_sv default_sv = {
        .pdus_per_time_unit     = 0,
        .next_snd_ctl_seq       = 0,
        .last_rcv_ctl_seq       = 0,
        .last_snd_data_ack      = 0,
        .snd_lft_win            = 0,
        .data_retransmit_max    = 0,
        .last_rcv_data_ack      = 0,
        .time_unit              = 0,
        .sndr_credit            = 1,
        .snd_rt_wind_edge       = 100,
        .sndr_rate              = 0,
        .pdus_sent_in_time_unit = 0,
        .rcvr_credit            = 1,
        .rcvr_rt_wind_edge      = 100,
        .rcvr_rate              = 0,
        .pdus_rcvd_in_time_unit = 0,
        .acks                   = 0,
        .flow_ctl               = 0,
};

/* FIXME: this should be completed with other parameters from the config */
static int dtcp_sv_init(struct dtcp * instance, struct dtcp_sv sv)
{
        struct dtcp_config * cfg;

        if (!instance) {
                LOG_ERR("Bogus instance passed");
                return -1;
        }

        if (!instance->sv) {
                LOG_ERR("Bogus sv passed");
                return -1;
        }

        cfg = dtcp_config_get(instance);
        if (!cfg)
                return -1;

        *instance->sv = sv;
        spin_lock_init(&instance->sv->lock);

        if (dtcp_rtx_ctrl(cfg))
                instance->sv->data_retransmit_max =
                        dtcp_data_retransmit_max(cfg);

        instance->sv->sndr_credit         = dtcp_initial_credit(cfg);
        instance->sv->snd_rt_wind_edge    = dtcp_initial_credit(cfg);
        instance->sv->rcvr_credit         = dtcp_initial_credit(cfg);
        instance->sv->rcvr_rt_wind_edge   = dtcp_initial_credit(cfg);

        LOG_DBG("DTCP SV initialized with dtcp_conf:");
        LOG_DBG("  data_retransmit_max: %d",
                instance->sv->data_retransmit_max);
        LOG_DBG("  sndr_credit:         %d",
                instance->sv->sndr_credit);
        LOG_DBG("  snd_rt_wind_edge:    %d",
                instance->sv->snd_rt_wind_edge);
        LOG_DBG("  rcvr_credit:         %d",
                instance->sv->rcvr_credit);
        LOG_DBG("  rcvr_rt_wind_edge:   %d",
                instance->sv->rcvr_rt_wind_edge);

        return 0;
}

struct dtcp *
dtcp_from_component(struct rina_component * component)
{
        return container_of(component, struct dtcp, base);
}
EXPORT_SYMBOL(dtcp_from_component);

int dtcp_select_policy_set(struct dtcp * dtcp,
                           const string_t * path,
                           const string_t * name)
{
        if (path && strcmp(path, "")) {
                LOG_ERR("This component has no selectable subcomponents");
                return -1;
        }

        return base_select_policy_set(&dtcp->base, &policy_sets, name);
}
EXPORT_SYMBOL(dtcp_select_policy_set);

int dtcp_set_policy_set_param(struct dtcp* dtcp,
                              const char * path,
                              const char * name,
                              const char * value)
{
        struct dtcp_ps *ps;
        int ret = -1;

        if (!dtcp|| !path || !name || !value) {
                LOG_ERRF("NULL arguments %p %p %p %p", dtcp, path, name, value);
                return -1;
        }

        LOG_DBG("set-policy-set-param '%s' '%s' '%s'", path, name, value);

        if (strcmp(path, "") == 0) {
                /* The request addresses this DTP instance. */
                rcu_read_lock();
                ps = container_of(rcu_dereference(dtcp->base.ps), struct dtcp_ps, base);
                LOG_ERR("Unknown DTP parameter policy '%s'", name);
                rcu_read_unlock();
        } else {
                ret = base_set_policy_set_param(&dtcp->base, path, name, value);
        }

        return ret;
}
EXPORT_SYMBOL(dtcp_set_policy_set_param);

struct dtcp * dtcp_create(struct dt *         dt,
                          struct connection * conn,
                          struct rmt *        rmt)
{
        struct dtcp * tmp;

        if (!dt) {
                LOG_ERR("No DT passed, bailing out");
                return NULL;
        }
        if (!conn) {
                LOG_ERR("No connection, bailing out");
                return NULL;
        }
        if (!rmt) {
                LOG_ERR("No RMT, bailing out");
                return NULL;
        }

        tmp = rkzalloc(sizeof(*tmp), GFP_KERNEL);
        if (!tmp) {
                LOG_ERR("Cannot create DTCP state-vector");
                return NULL;
        }

        tmp->parent = dt;

        tmp->sv = rkzalloc(sizeof(*tmp->sv), GFP_KERNEL);
        if (!tmp->sv) {
                LOG_ERR("Cannot create DTCP state-vector");
                dtcp_destroy(tmp);
                return NULL;
        }

        tmp->conn = conn;
        tmp->rmt  = rmt;

        if (dtcp_sv_init(tmp, default_sv)) {
                LOG_ERR("Could not load DTCP config in the SV");
                dtcp_destroy(tmp);
                return NULL;
        }
        /* FIXME: fixups to the state-vector should be placed here */

        rina_component_init(&tmp->base);

        /* Try to select the default policy-set. */
        if (dtcp_select_policy_set(tmp, "", RINA_PS_DEFAULT_NAME)) {
                dtcp_destroy(tmp);
                return NULL;
        }

        LOG_DBG("Instance %pK created successfully", tmp);

        return tmp;
}

int dtcp_destroy(struct dtcp * instance)
{
        if (!instance) {
                LOG_ERR("Bad instance passed, bailing out");
                return -1;
        }

        if (instance->sv)       rkfree(instance->sv);
        rina_component_fini(&instance->base);
        rkfree(instance);

        LOG_DBG("Instance %pK destroyed successfully", instance);

        return 0;
}

int dtcp_sv_update(struct dtcp * instance,
                   seq_num_t     seq)
{
        struct dtcp_ps * ps;

        if (!instance) {
                LOG_ERR("Bogus instance passed");
                return -1;
        }

        rcu_read_lock();
        ps = container_of(rcu_dereference(instance->base.ps),
                          struct dtcp_ps, base);

        ASSERT(ps);
        ASSERT(ps->sv_update);

        if (ps->sv_update(ps, seq)) {
                rcu_read_unlock();
                return -1;
        }

        rcu_read_unlock();

        return 0;
}

int dtcp_ack_flow_control_pdu_send(struct dtcp * dtcp)
{
        if (!dtcp)
                return -1;

        LOG_MISSING;

        return 0;
}

seq_num_t dtcp_snd_rt_win(struct dtcp * dtcp)
{
        if (!dtcp || !dtcp->sv)
                return -1;

        return snd_rt_wind_edge(dtcp);
}

seq_num_t dtcp_snd_lf_win(struct dtcp * dtcp)
{
        if (!dtcp || !dtcp->sv)
                return -1;

        return snd_lft_win(dtcp);
}

int dtcp_snd_lf_win_set(struct dtcp * instance, seq_num_t seq_num)
{
        if (!instance)
                return -1;

        spin_lock(&instance->sv->lock);
        instance->sv->snd_lft_win = seq_num;
        spin_unlock(&instance->sv->lock);

        return 0;
}
EXPORT_SYMBOL(dtcp_snd_lf_win_set);

int dtcp_ps_publish(struct ps_factory * factory)
{
        if (factory == NULL) {
                LOG_ERR("%s: NULL factory", __func__);
                return -1;
        }

        return ps_publish(&policy_sets, factory);
}
EXPORT_SYMBOL(dtcp_ps_publish);

int dtcp_ps_unpublish(const char * name)
{ return ps_unpublish(&policy_sets, name); }
EXPORT_SYMBOL(dtcp_ps_unpublish);

