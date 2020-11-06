/* MPTCP Scheduler module selector. Highly inspired by tcp_cong.c */

#include <linux/module.h>
#include <net/mptcp.h>

/* Same as mptcp_dss_len from mptcp_output.c */
static const int mptcp_dss_len = MPTCP_SUB_LEN_DSS_ALIGN +
				 MPTCP_SUB_LEN_ACK_ALIGN +
				 MPTCP_SUB_LEN_SEQ_ALIGN;

static DEFINE_SPINLOCK(mptcp_sched_list_lock);
static LIST_HEAD(mptcp_sched_list);

struct defsched_priv {
	u32	last_rbuf_opti;
	struct timer_list fast_retransmission_timer;

	struct tcp_sock *activetp;
	struct tcp_sock *backuptp;

	u32 rtt_penalty;
	u32 rtt_penalty_expires;
	bool rtt_penalty_on;
};

static struct defsched_priv *defsched_get_priv(const struct tcp_sock *tp)
{
	return (struct defsched_priv *)&tp->mptcp->mptcp_sched[0];
}

static u32 get_rtt(struct tcp_sock* tp) {
	struct defsched_priv *dsp = defsched_get_priv(tp);
	if (dsp->rtt_penalty_on) {
		if (dsp->rtt_penalty_expires > tcp_time_stamp) {
			return tp->srtt_us + dsp->rtt_penalty;
		} else {
			dsp->rtt_penalty_on = false;
			return tp->srtt_us;
		}
	} else {
		return tp->srtt_us;
	}
}

static bool mptcp_is_def_unavailable(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	/* Set of states for which we are allowed to send data */
	if (!mptcp_sk_can_send(sk))
		return true;

	/* We do not send data on this subflow unless it is
	 * fully established, i.e. the 4th ack has been received.
	 */
	if (tp->mptcp->pre_established)
		return true;

	if (tp->pf)
		return true;

	return false;
}

static bool mptcp_is_temp_unavailable(struct sock *sk,
				      const struct sk_buff *skb,
				      bool zero_wnd_test)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	unsigned int mss_now, space, in_flight;

	if (inet_csk(sk)->icsk_ca_state == TCP_CA_Loss) {
		/* If SACK is disabled, and we got a loss, TCP does not exit
		 * the loss-state until something above high_seq has been
		 * acked. (see tcp_try_undo_recovery)
		 *
		 * high_seq is the snd_nxt at the moment of the RTO. As soon
		 * as we have an RTO, we won't push data on the subflow.
		 * Thus, snd_una can never go beyond high_seq.
		 */
		if (!tcp_is_reno(tp))
			return true;
		else if (tp->snd_una != tp->high_seq)
			return true;
	}

	if (!tp->mptcp->fully_established) {
		/* Make sure that we send in-order data */
		if (skb && tp->mptcp->second_packet &&
		    tp->mptcp->last_end_data_seq != TCP_SKB_CB(skb)->seq)
			return true;
	}

	/* If TSQ is already throttling us, do not send on this subflow. When
	 * TSQ gets cleared the subflow becomes eligible again.
	 */
	if (test_bit(TSQ_THROTTLED, &tp->tsq_flags))
		return true;

	in_flight = tcp_packets_in_flight(tp);
	/* Not even a single spot in the cwnd */
	if (in_flight >= tp->snd_cwnd)
		return true;

	/* Now, check if what is queued in the subflow's send-queue
	 * already fills the cwnd.
	 */
	space = (tp->snd_cwnd - in_flight) * tp->mss_cache;

	if (tp->write_seq - tp->snd_nxt > space)
		return true;

	if (zero_wnd_test && !before(tp->write_seq, tcp_wnd_end(tp)))
		return true;

	mss_now = tcp_current_mss(sk);

	/* Don't send on this subflow if we bypass the allowed send-window at
	 * the per-subflow level. Similar to tcp_snd_wnd_test, but manually
	 * calculated end_seq (because here at this point end_seq is still at
	 * the meta-level).
	 */
	if (skb && !zero_wnd_test &&
	    after(tp->write_seq + min(skb->len, mss_now), tcp_wnd_end(tp)))
		return true;

	return false;
}

/* Is the sub-socket sk available to send the skb? */
static bool mptcp_is_available(struct sock *sk, const struct sk_buff *skb,
			       bool zero_wnd_test)
{
	return !mptcp_is_def_unavailable(sk) &&
	       !mptcp_is_temp_unavailable(sk, skb, zero_wnd_test);
}

/* Are we not allowed to reinject this skb on tp? */
static int mptcp_dont_reinject_skb(const struct tcp_sock *tp, const struct sk_buff *skb)
{
	/* If the skb has already been enqueued in this sk, try to find
	 * another one.
	 */
	return skb &&
		/* Has the skb already been enqueued into this subsocket? */
		mptcp_pi_to_flag(tp->mptcp->path_index) & TCP_SKB_CB(skb)->path_mask;
}

/* Function that returns the slowest pass of established subflows */
static struct sock *get_slow_socket(struct mptcp_cb *mpcb) {
	struct sock *sk;

	/* Initialization to find fast flow*/
	struct sock *fastsk = NULL, *slowsk = NULL;
	u32 fast_srtt = 0xffffffff, slow_srtt = 0;

	mptcp_for_each_sk(mpcb, sk) {
		struct tcp_sock *tp = tcp_sk(sk);
		bool unused = false;

		/* Find Fast Flow in selection flows */
		if (!mptcp_is_def_unavailable(sk) && get_rtt(tp)) {
			if (fast_srtt > get_rtt(tp)) {
				fast_srtt = get_rtt(tp);
				fastsk = sk;
			}
			if (slow_srtt < get_rtt(tp)) {
				slow_srtt = get_rtt(tp);
				slowsk = sk;
			}
		}
	}
	if (mpcb->cnt_established >= 2 && (fastsk && slowsk) && fastsk != slowsk)
		return slowsk;
	return NULL;
}

static bool subflow_is_backup(const struct tcp_sock *tp)
{
	return tp->mptcp->rcv_low_prio || tp->mptcp->low_prio;
}

static bool subflow_is_active(const struct tcp_sock *tp)
{
	return !tp->mptcp->rcv_low_prio && !tp->mptcp->low_prio;
}

/* Generic function to iterate over used and unused subflows and to select the
 * best one
 */
static struct sock
*get_subflow_from_selectors(struct mptcp_cb *mpcb, struct sk_buff *skb,
			    bool (*selector)(const struct tcp_sock *),
			    bool zero_wnd_test, bool *force)
{
	struct sock *bestsk = NULL;
	u32 min_srtt = 0xffffffff;
	bool found_unused = false;
	bool found_unused_una = false;
	struct sock *sk;

	mptcp_for_each_sk(mpcb, sk) {
		struct tcp_sock *tp = tcp_sk(sk);
		bool unused = false;

		/* First, we choose only the wanted sks */
		if (!(*selector)(tp))
			continue;

		if (!mptcp_dont_reinject_skb(tp, skb))
			unused = true;
		else if (found_unused)
			/* If a unused sk was found previously, we continue -
			 * no need to check used sks anymore.
			 */
			continue;

		if (mptcp_is_def_unavailable(sk))
			continue;

		if (mptcp_is_temp_unavailable(sk, skb, zero_wnd_test)) {
			if (unused)
				found_unused_una = true;
			continue;
		}

		if (unused) {
			if (!found_unused) {
				/* It's the first time we encounter an unused
				 * sk - thus we reset the bestsk (which might
				 * have been set to a used sk).
				 */
				min_srtt = 0xffffffff;
				bestsk = NULL;
			}
			found_unused = true;
		}

		if (get_rtt(tp) < min_srtt) {
			min_srtt = get_rtt(tp);
			bestsk = sk;
		}
	}

	if (bestsk) {
		/* The force variable is used to mark the returned sk as
		 * previously used or not-used.
		 */
		if (found_unused)
			*force = true;
		else
			*force = false;
	} else {
		/* The force variable is used to mark if there are temporally
		 * unavailable not-used sks.
		 */
		if (found_unused_una)
			*force = true;
		else
			*force = false;
	}
	if (skb && TCP_SKB_CB(skb)->path_mask == 0 && bestsk == get_slow_socket(mpcb))
		return NULL;

	return bestsk;
}

/* This is the scheduler. This function decides on which flow to send
 * a given MSS. If all subflows are found to be busy, NULL is returned
 * The flow is selected based on the shortest RTT.
 * If all paths have full cong windows, we simply return NULL.
 *
 * Additionally, this function is aware of the backup-subflows.
 */
static struct sock *get_available_subflow(struct sock *meta_sk,
					  struct sk_buff *skb,
					  bool zero_wnd_test)
{
	struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
	struct sock *sk;
	bool force;

	/* if there is only one subflow, bypass the scheduling function */
	if (mpcb->cnt_subflows == 1) {
		sk = (struct sock *)mpcb->connection_list;
		if (!mptcp_is_available(sk, skb, zero_wnd_test))
			sk = NULL;
		return sk;
	}

	/* Answer data_fin on same subflow!!! */
	if (meta_sk->sk_shutdown & RCV_SHUTDOWN &&
	    skb && mptcp_is_data_fin(skb)) {
		mptcp_for_each_sk(mpcb, sk) {
			if (tcp_sk(sk)->mptcp->path_index == mpcb->dfin_path_index &&
			    mptcp_is_available(sk, skb, zero_wnd_test))
				return sk;
		}
	}

	/* Find the best subflow */
	sk = get_subflow_from_selectors(mpcb, skb, &subflow_is_active,
					zero_wnd_test, &force);
	if (force)
		/* one unused active sk or one NULL sk when there is at least
		 * one temporally unavailable unused active sk
		 */
		return sk;

	sk = get_subflow_from_selectors(mpcb, skb, &subflow_is_backup,
					zero_wnd_test, &force);
	if (!force && skb)
		/* one used backup sk or one NULL sk where there is no one
		 * temporally unavailable unused backup sk
		 *
		 * the skb passed through all the available active and backups
		 * sks, so clean the path mask
		 */
		TCP_SKB_CB(skb)->path_mask = 0;
	return sk;
}

static struct sk_buff *mptcp_rcv_buf_optimization(struct sock *sk, int penal)
{
	struct sock *meta_sk;
	const struct tcp_sock *tp = tcp_sk(sk);
	struct tcp_sock *tp_it;
	struct sk_buff *skb_head;
	struct defsched_priv *dsp = defsched_get_priv(tp);

	if (tp->mpcb->cnt_subflows == 1)
		return NULL;

	meta_sk = mptcp_meta_sk(sk);
	skb_head = tcp_write_queue_head(meta_sk);

	if (!skb_head || skb_head == tcp_send_head(meta_sk))
		return NULL;

	/* If penalization is optional (coming from mptcp_next_segment() and
	 * We are not send-buffer-limited we do not penalize. The retransmission
	 * is just an optimization to fix the idle-time due to the delay before
	 * we wake up the application.
	 */
	if (!penal && sk_stream_memory_free(meta_sk))
		goto retrans;

	/* Only penalize again after an RTT has elapsed */
	if (tcp_time_stamp - dsp->last_rbuf_opti < usecs_to_jiffies(tp->srtt_us >> 3))
		goto retrans;

	/* Half the cwnd of the slow flow */
	mptcp_for_each_tp(tp->mpcb, tp_it) {
		if (tp_it != tp &&
		    TCP_SKB_CB(skb_head)->path_mask & mptcp_pi_to_flag(tp_it->mptcp->path_index)) {
			if (tp->srtt_us < tp_it->srtt_us && inet_csk((struct sock *)tp_it)->icsk_ca_state == TCP_CA_Open) {
				u32 prior_cwnd = tp_it->snd_cwnd;

				tp_it->snd_cwnd = max(tp_it->snd_cwnd >> 1U, 1U);

				/* If in slow start, do not reduce the ssthresh */
				if (prior_cwnd >= tp_it->snd_ssthresh)
					tp_it->snd_ssthresh = max(tp_it->snd_ssthresh >> 1U, 2U);

				dsp->last_rbuf_opti = tcp_time_stamp;
			}
			break;
		}
	}

retrans:

	/* Segment not yet injected into this path? Take it!!! */
	if (!(TCP_SKB_CB(skb_head)->path_mask & mptcp_pi_to_flag(tp->mptcp->path_index))) {
		bool do_retrans = false;
		mptcp_for_each_tp(tp->mpcb, tp_it) {
			if (tp_it != tp &&
			    TCP_SKB_CB(skb_head)->path_mask & mptcp_pi_to_flag(tp_it->mptcp->path_index)) {
				if (tp_it->snd_cwnd <= 4) {
					do_retrans = true;
					break;
				}

				if (4 * tp->srtt_us >= tp_it->srtt_us) {
					do_retrans = false;
					break;
				} else {
					do_retrans = true;
				}
			}
		}

		if (do_retrans && mptcp_is_available(sk, skb_head, false))
			return skb_head;
	}
	return NULL;
}

/* Returns the next segment to be sent from the mptcp meta-queue.
 * (chooses the reinject queue if any segment is waiting in it, otherwise,
 * chooses the normal write queue).
 * Sets *@reinject to 1 if the returned segment comes from the
 * reinject queue. Sets it to 0 if it is the regular send-head of the meta-sk,
 * and sets it to -1 if it is a meta-level retransmission to optimize the
 * receive-buffer.
 */
static struct sk_buff *__mptcp_next_segment(struct sock *meta_sk, int *reinject)
{
	struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
	struct sk_buff *skb = NULL;
begin:
	*reinject = 0;

	/* If we are in fallback-mode, just take from the meta-send-queue */
	if (mpcb->infinite_mapping_snd || mpcb->send_infinite_mapping)
		return tcp_send_head(meta_sk);

	skb = skb_peek(&mpcb->reinject_queue);

	if (skb) {
		*reinject = 1;
		if (TCP_SKB_CB(skb)->dss[1] == 1) {
			struct sock *subsk = get_available_subflow(meta_sk,
									skb,
									false);
			struct tcp_sock *tp = tcp_sk(subsk);
			if (!subsk) {
				/* There is no available subflow */
				skb_unlink(skb, &mpcb->reinject_queue);
				__kfree_skb(skb);
				goto begin;
			}

			if (TCP_SKB_CB(skb)->path_mask == 0 ||
				TCP_SKB_CB(skb)->path_mask &
				mptcp_pi_to_flag(tp->mptcp->path_index)) {
				/* The specified path cannot be used. */
				skb_unlink(skb, &mpcb->reinject_queue);
				__kfree_skb(skb);
				goto begin;
			}
		}
	} else {
		skb = tcp_send_head(meta_sk);

		if (!skb && meta_sk->sk_socket &&
		    test_bit(SOCK_NOSPACE, &meta_sk->sk_socket->flags) &&
		    sk_stream_wspace(meta_sk) < sk_stream_min_wspace(meta_sk)) {
			struct sock *subsk = get_available_subflow(meta_sk, NULL,
								   false);
			if (!subsk)
				return NULL;

			skb = mptcp_rcv_buf_optimization(subsk, 0);
			if (skb)
				*reinject = -1;
		}
	}
	return skb;
}

void check_ack(struct defsched_priv *dsp) {
	/* 3 Case:
		1. if active flow was changed very low speed
		2. Change fast path
		3. Can't send new packet because all data was sended.
	*/
	struct tcp_sock *meta_sk = 	mptcp_meta_sk(dsp->activetp);
	struct tcp_sock *meta_tp = 	tcp_sk(meta_sk);
	struct sk_buff *skb = tcp_write_queue_head(meta_sk);
  	struct mptcp_cb *mpcb = meta_tp->mpcb;

	dsp->rtt_penalty = get_rtt(dsp->backuptp) * 1.5;
	dsp->rtt_penalty_expires = tcp_time_stamp + usecs_to_jiffies(get_rtt(dsp->backuptp) >> 3) * 5;
	if (!skb)
		return;
	struct skb_mstamp now;
	skb_mstamp_get(&now);
	// Check skb, had sended backup flow(slow)
	bool reinject_start_check = false;
	while (skb != NULL && skb != tcp_send_head(meta_sk)) {
		if (mptcp_dont_reinject_skb(dsp->activetp, skb) && TCP_SKB_CB(skb)->dss[1] != 2)
		{
			if (reinject_start_check == false) {

    			u32 seq_rtt_us = skb_mstamp_us_delta(&now, &(skb->skb_mstamp));
				if (seq_rtt_us < (dsp->backuptp->srtt_us >> 3) * 2)
				{
					unsigned long expires = tcp_time_stamp + usecs_to_jiffies(get_rtt(dsp->backuptp) >> 3) * 0.2;
					struct timer_list *timer = &(dsp->fast_retransmission_timer);
					if (!timer_pending(timer)) {
						init_timer(timer);
						timer->function = check_ack;
						timer->data = (unsigned long)dsp;
						timer->expires = expires;// + 0.1 * HZ;
						add_timer(timer);
					}
					return;

				}
				else 
					reinject_start_check = true;
			}
			/* Copy SKB */
			if (dsp->rtt_penalty_on == false) {
				dsp->rtt_penalty_on = true;
				// dsp->activetp->srtt_us = dsp->activetp->srtt_us + dsp->rtt_penalty * 5;
			}
			struct sk_buff *copy_skb = pskb_copy_for_clone(skb, GFP_ATOMIC);
			if (likely(copy_skb)) { /* SKB isn't NULL */
				copy_skb->sk = meta_sk;
				if (!after(TCP_SKB_CB(copy_skb)->end_seq, meta_tp->snd_una)) {
					__kfree_skb(copy_skb);
				} else {

					TCP_SKB_CB(skb)->dss[1] = 2;
					memset(TCP_SKB_CB(copy_skb)->dss, 0 , mptcp_dss_len);
					TCP_SKB_CB(copy_skb)->path_mask = mptcp_pi_to_flag(tcp_sk(dsp->backuptp)->mptcp->path_index);
					TCP_SKB_CB(copy_skb)->path_mask ^= -1u;
					TCP_SKB_CB(copy_skb)->dss[1] = 2;
					skb_queue_tail(&mpcb->reinject_queue, copy_skb);
					tcp_log(0, "copyskb", 1);
				}
			}
		}
		if (skb == tcp_write_queue_tail(meta_sk)) break;
		else skb = skb->next;
		// Check 
	}
	mptcp_write_xmit(meta_sk, tcp_current_mss(meta_sk), tcp_sk(meta_sk)->nonagle, 0, GFP_ATOMIC);

	//if (slowtp->rcv_rtt_est.time)
	// struct sk_buff *skb_head = tcp_write_queue_head(meta_sk);
	//if (!skb_head || skb_head == tcp_send_head(meta_sk))
	// 	return NULL;

}
static struct sk_buff *mptcp_next_segment(struct sock *meta_sk,
					  int *reinject,
					  struct sock **subsk,
					  unsigned int *limit)
{
	struct sk_buff *skb = __mptcp_next_segment(meta_sk, reinject);
	unsigned int mss_now;
	struct tcp_sock *subtp;
	u16 gso_max_segs;
	u32 max_len, max_segs, window, needed;

	/* As we set it, we have to reset it as well. */
	*limit = 0;

	if (!skb)
		return NULL;

	*subsk = get_available_subflow(meta_sk, skb, false);
	if (!*subsk)
		return NULL;

	subtp = tcp_sk(*subsk);
	mss_now = tcp_current_mss(*subsk);

	if (!*reinject && unlikely(!tcp_snd_wnd_test(tcp_sk(meta_sk), skb, mss_now))) {
		skb = mptcp_rcv_buf_optimization(*subsk, 1);
		if (skb)
			*reinject = -1;
		else
			return NULL;
	}

	const struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct mptcp_cb *mpcb = meta_tp->mpcb;
	struct sock *slowsk = get_slow_socket(mpcb);
	if (slowsk != NULL) {
		struct tcp_sock *slowtp = tcp_sk(slowsk);

		/* If there are no packets in flight on the slow path, and the packet to be sent now is not an injected packet */
		if (!tcp_packets_in_flight(slowtp) && TCP_SKB_CB(skb)->path_mask == 0) {
			struct mptcp_cb *mpcb = meta_tp->mpcb;
			struct sock *slowsk = get_slow_socket(mpcb);
			/* Copy SKB */
			struct sk_buff *copy_skb = pskb_copy_for_clone(skb, GFP_ATOMIC);
			if (likely(copy_skb)) { /* SKB isn't NULL */
				copy_skb->sk = meta_sk;
				if (!after(TCP_SKB_CB(copy_skb)->end_seq, meta_tp->snd_una)) {
					__kfree_skb(copy_skb);
				} else {
					memset(TCP_SKB_CB(copy_skb)->dss, 0 , mptcp_dss_len);
					TCP_SKB_CB(copy_skb)->path_mask = mptcp_pi_to_flag(tcp_sk(slowsk)->mptcp->path_index);
					TCP_SKB_CB(copy_skb)->path_mask ^= -1u;
					TCP_SKB_CB(copy_skb)->dss[1] = 1;
					skb_queue_tail(&mpcb->reinject_queue, copy_skb);
				}
			}
		}

		if (subsk != slowsk) {
			struct defsched_priv *dsp = defsched_get_priv(subtp);
			dsp->activetp = subtp;
			dsp->backuptp = slowtp;
			unsigned long expires = tcp_time_stamp + usecs_to_jiffies(get_rtt(slowtp) >> 3) * 1.5;
			struct timer_list *timer = &(dsp->fast_retransmission_timer);

			if (timer_pending(timer)) {
				// mod_timer(timer, expires);
			} else {
				init_timer(timer);
				timer->function = check_ack;
				timer->data = (unsigned long)dsp;
				timer->expires = expires;// + 0.1 * HZ;
				add_timer(timer);
			}
		}
	}

	/* No splitting required, as we will only send one single segment */
	if (skb->len <= mss_now)
		return skb;

	/* The following is similar to tcp_mss_split_point, but
	 * we do not care about nagle, because we will anyways
	 * use TCP_NAGLE_PUSH, which overrides this.
	 *
	 * So, we first limit according to the cwnd/gso-size and then according
	 * to the subflow's window.
	 */

	gso_max_segs = (*subsk)->sk_gso_max_segs;
	if (!gso_max_segs) /* No gso supported on the subflow's NIC */
		gso_max_segs = 1;
	max_segs = min_t(unsigned int, tcp_cwnd_test(subtp, skb), gso_max_segs);
	if (!max_segs)
		return NULL;

	max_len = mss_now * max_segs;
	window = tcp_wnd_end(subtp) - subtp->write_seq;

	needed = min(skb->len, window);
	if (max_len <= skb->len)
		/* Take max_win, which is actually the cwnd/gso-size */
		*limit = max_len;
	else
		/* Or, take the window */
		*limit = needed;

	return skb;
}

static void defsched_init(struct sock *sk)
{
	struct defsched_priv *dsp = defsched_get_priv(tcp_sk(sk));

	dsp->last_rbuf_opti = tcp_time_stamp;
}

struct mptcp_sched_ops mptcp_only_fast = {
	.get_subflow = get_available_subflow,
	.next_segment = mptcp_next_segment,
	.init = defsched_init,
	.name = "only_fast",
	.owner = THIS_MODULE,
};

static int __init only_fast_register(void)
{
	BUILD_BUG_ON(sizeof(struct defsched_priv) > MPTCP_SCHED_SIZE);

	if (mptcp_register_scheduler(&mptcp_only_fast))
		return -1;

	return 0;
}

static void only_fast_unregister(void)
{
	mptcp_unregister_scheduler(&mptcp_only_fast);
}

module_init(only_fast_register);
module_exit(only_fast_unregister);