#include <linux/module.h>
#include <net/mptcp.h>

static unsigned char optim __read_mostly = 1;
module_param(optim, byte, 0644);
MODULE_PARM_DESC(optim, "1 = retransmit, 2 = retransmit+penalize, 0 = off");

static unsigned char lambda __read_mostly = 12;
module_param(lambda, byte, 0644);
MODULE_PARM_DESC(lambda, "Divided by 10 for scaling factor of fast flow rate estimation");

static unsigned char max_lambda __read_mostly = 13;
module_param(max_lambda, byte, 0644);
MODULE_PARM_DESC(max_lambda, "Divided by 10 for maximum scaling factor of fast flow rate estimation");

static unsigned char min_lambda __read_mostly = 10;
module_param(min_lambda, byte, 0644);
MODULE_PARM_DESC(min_lambda, "Divided by 10 for minimum scaling factor of fast flow rate estimation");

static unsigned char dyn_lambda_good = 10; // 1%
module_param(dyn_lambda_good, byte, 0644);
MODULE_PARM_DESC(dyn_lambda_good, "Decrease of lambda in positive case.");

static unsigned char dyn_lambda_bad = 40; // 4%
module_param(dyn_lambda_bad, byte, 0644);
MODULE_PARM_DESC(dyn_lambda_bad, "Increase of lambda in negative case.");

struct defsched_priv {
	u32 last_rbuf_opti;

	u32 min_srtt;
	u32 max_srtt;
};

struct blestsched_priv {
	int lambda_1000;

	u32 last_lambda_update;
	u32 retrans_count;
};

static struct defsched_priv *defsched_get_priv(const struct tcp_sock *tp)
{
	return (struct defsched_priv *)&tp->mptcp->mptcp_sched[0];
}

static struct blestsched_priv *blestsched_get_priv(const struct tcp_sock *meta_tp)
{
	struct mptcp_cb *mpcb = meta_tp->mpcb;
	struct tcp_sock *first_tp = mpcb->connection_list; // first connection
	return (struct defsched_priv *)&first_tp->mptcp->mptcp_sched[0] + 1;
}

static void modify_lambda(struct sock *meta_sk, int dir)
{
	struct tcp_sock *tp = tcp_sk(meta_sk);
	struct blestsched_priv *bsp = blestsched_get_priv(tp);
	int delta = 0;
	u32 old_lambda_1000;

	old_lambda_1000 = bsp->lambda_1000;

	if (dir == -1) {
		// use the slow flow more
		delta = dyn_lambda_good;
		delta = -delta;
	}
	else if (dir == 1) {
		// need to slow down on the slow flow
		delta = dyn_lambda_bad;
	}

	bsp->lambda_1000 += delta;
	if (bsp->lambda_1000 > max_lambda * 100)
		bsp->lambda_1000 = max_lambda * 100;
	else if (bsp->lambda_1000 < min_lambda * 100)
		bsp->lambda_1000 = min_lambda * 100;
}

// if there have been retransmissions of packets of the slow flow during the slow flows last RTT => increase lambda
// otherwise decrease
static void update_lambda(struct sock *slow_sk, struct sock *meta_sk)
{
	struct tcp_sock *slowtp = tcp_sk(slow_sk);
	struct defsched_priv *slowdsp = defsched_get_priv(slowtp);
	struct tcp_sock *tp = tcp_sk(meta_sk);
	struct blestsched_priv *bsp = blestsched_get_priv(tp);
	u32 slowrtt;

	if (!bsp->lambda_1000) {
		bsp->lambda_1000 = lambda * 100;
		bsp->last_lambda_update = tcp_time_stamp;
		return;
	}

	slowrtt = slowdsp->min_srtt;
	if (!slowrtt)
		slowrtt = slowtp->srtt_us;

	if (tcp_time_stamp - bsp->last_lambda_update < (slowrtt >> 3))
		return;

	if (bsp->retrans_count > 0) {
		modify_lambda(meta_sk, 1);
	}
	else {
		modify_lambda(meta_sk, -1);
	}
	bsp->retrans_count = 0;

	bsp->last_lambda_update = tcp_time_stamp;
}

static u32 get_dyn_lambda(struct sock *meta_sk)
{
	struct tcp_sock *tp = tcp_sk(meta_sk);
	struct blestsched_priv *bsp = blestsched_get_priv(tp);
	if (!bsp->lambda_1000)
		return lambda * 100;
	return bsp->lambda_1000;
}

static void mptcp_update_stats(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct defsched_priv *dsp = defsched_get_priv(tp);

	if (!dsp->min_srtt) {
		dsp->min_srtt = tp->srtt_us;
		dsp->max_srtt = tp->srtt_us;
	}
	else {
		if (tp->srtt_us < dsp->min_srtt)
			dsp->min_srtt = tp->srtt_us;
		if (tp->srtt_us > dsp->max_srtt)
			dsp->max_srtt = tp->srtt_us;
	}
}

// how many bytes will sk send during the rtt of another, slower flow?
static u32 estimate_bytes(struct sock* sk, u32 time_8)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct defsched_priv *dsp = defsched_get_priv(tp);
	u32 bytes, num_rtts, packets, lamb_1000;

	lamb_1000 = get_dyn_lambda(mptcp_meta_sk(sk));

	u32 avg_rtt = (dsp->min_srtt + dsp->max_srtt) / 2;
	if (avg_rtt == 0)
		num_rtts = 1; // sanity
	else
		num_rtts = (time_8 / avg_rtt) + 1; // round up

	// during num_rtts, how many bytes will be sent on the flow?
	if (tp->snd_ssthresh == TCP_INFINITE_SSTHRESH) {
		// we are in initial slow start
		if (num_rtts > 16)
			num_rtts = 16; // cap for sanity
		packets = tp->snd_cwnd * ((1 << num_rtts) - 1); // cwnd + 2*cwnd + 4*cwnd
	}
	else {
		u32 ca_cwnd = max(tp->snd_cwnd, tp->snd_ssthresh+1); // assume we jump to CA already
		packets = (ca_cwnd + (num_rtts - 1) / 2) * num_rtts;
	}
	bytes = div_u64( (((u64)packets) * tp->mss_cache * lamb_1000), 1000 );

	return bytes;
}

static u32 estimate_linger_time(struct sock* sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct defsched_priv *dsp = defsched_get_priv(tp);

	u32 old_estim = tp->srtt_us;

	u32 inflight = tcp_packets_in_flight(tp) + 1; // take into account the new one
	u32 cwnd = tp->snd_cwnd;

	u32 new_estim;
	if (inflight >= cwnd) {
		new_estim = dsp->max_srtt;
	}
	else {
		u32 slope = dsp->max_srtt - dsp->min_srtt;
		if (cwnd == 0)
			cwnd = 1; // sanity
		new_estim = dsp->min_srtt + (slope * inflight) / cwnd;
	}

	return (old_estim > new_estim) ? old_estim : new_estim;	
}

static bool mptcp_cwnd_test2(struct sock *sk, struct sk_buff *skb,
			       bool zero_wnd_test)
{
	struct tcp_sock *tp = tcp_sk(sk);
	unsigned int mss_now, space, in_flight;
	
	in_flight = tcp_packets_in_flight(tp);
	/* Not even a single spot in the cwnd */
	if (in_flight >= tp->snd_cwnd)
		return false;

	/* Now, check if what is queued in the subflow's send-queue
	 * already fills the cwnd.
	 */
	space = (tp->snd_cwnd - in_flight) * tp->mss_cache;

	if (tp->write_seq - tp->snd_nxt > space)
		return false;

	mss_now = tcp_current_mss(sk);

	/* Don't send on this subflow if we bypass the allowed send-window at
	 * the per-subflow level. Similar to tcp_snd_wnd_test, but manually
	 * calculated end_seq (because here at this point end_seq is still at
	 * the meta-level).
	 */
	if (skb && !zero_wnd_test &&
	    after(tp->write_seq + min(skb->len, mss_now), tcp_wnd_end(tp)))
		return false;
		
	return true;	
}

/* If the sub-socket sk available to send the skb? */
static bool mptcp_is_available(struct sock *sk, struct sk_buff *skb,
			       bool zero_wnd_test, bool cwnd_test)
{
	struct tcp_sock *tp = tcp_sk(sk);

	/* Set of states for which we are allowed to send data */
	if (!mptcp_sk_can_send(sk))
		return false;

	/* We do not send data on this subflow unless it is
	 * fully established, i.e. the 4th ack has been received.
	 */
	if (tp->mptcp->pre_established)
		return false;

	if (tp->pf)
		return false;

	if (inet_csk(sk)->icsk_ca_state == TCP_CA_Loss) {
		/* If SACK is disabled, and we got a loss, TCP does not exit
		 * the loss-state until something above high_seq has been acked.
		 * (see tcp_try_undo_recovery)
		 *
		 * high_seq is the snd_nxt at the moment of the RTO. As soon
		 * as we have an RTO, we won't push data on the subflow.
		 * Thus, snd_una can never go beyond high_seq.
		 */
		if (!tcp_is_reno(tp))
			return false;
		else if (tp->snd_una != tp->high_seq)
			return false;
	}

	if (!tp->mptcp->fully_established) {
		/* Make sure that we send in-order data */
		if (skb && tp->mptcp->second_packet &&
		    tp->mptcp->last_end_data_seq != TCP_SKB_CB(skb)->seq)
			return false;
	}

	/* If TSQ is already throttling us, do not send on this subflow. When
	 * TSQ gets cleared the subflow becomes eligible again.
	 */
	if (test_bit(TSQ_THROTTLED, &tp->tsq_flags))
		return false;

	if (cwnd_test && !mptcp_cwnd_test2(sk, skb, zero_wnd_test))
		return false;

	if (zero_wnd_test && !before(tp->write_seq, tcp_wnd_end(tp)))
		return false;

	return true;
}

/* Are we not allowed to reinject this skb on tp? */
static int mptcp_dont_reinject_skb(struct tcp_sock *tp, struct sk_buff *skb)
{
	/* If the skb has already been enqueued in this sk, try to find
	 * another one.
	 */
	return skb &&
		/* Has the skb already been enqueued into this subsocket? */
		mptcp_pi_to_flag(tp->mptcp->path_index) & TCP_SKB_CB(skb)->path_mask;
}

/* This is the scheduler. This function decides on which flow to send
 * a given MSS. If all subflows are found to be busy, NULL is returned
 * The flow is selected based on the shortest RTT.
 * If all paths have full cong windows, we simply return NULL.
 */
static struct sock *get_available_subflow(struct sock *meta_sk,
					  struct sk_buff *skb,
					  bool zero_wnd_test)
{
	struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
	struct sock *sk, *bestsk = NULL, *backupsk = NULL;
	u32 best_time_to_peer = 0xffffffff;

	/* if there is only one subflow, bypass the scheduling function */
	if (mpcb->cnt_subflows == 1) {
		bestsk = (struct sock *)mpcb->connection_list;
		if (!mptcp_is_available(bestsk, skb, zero_wnd_test, true))
			bestsk = NULL;
		return bestsk;
	}

	/* Answer data_fin on same subflow!!! */
	if (meta_sk->sk_shutdown & RCV_SHUTDOWN &&
	    skb && mptcp_is_data_fin(skb)) {
		mptcp_for_each_sk(mpcb, sk) {
			if (tcp_sk(sk)->mptcp->path_index == mpcb->dfin_path_index &&
			    mptcp_is_available(sk, skb, zero_wnd_test, true))
				return sk;
		}
	}

	if (skb) { // BLEST: need an SKB
		struct sock *minsk = NULL;
		u32 min_time_to_peer = 0xffffffff;

		/* First, find the best subflow */
		mptcp_for_each_sk(mpcb, sk) {
			struct tcp_sock *tp = tcp_sk(sk);

			/* Set of states for which we are allowed to send data */
			if (!mptcp_sk_can_send(sk))
				continue;

			/* We do not send data on this subflow unless it is
			 * fully established, i.e. the 4th ack has been received.
			 */
			if (tp->mptcp->pre_established)
				continue;

			mptcp_update_stats(sk);

			// record minimal rtt
			if (tp->srtt_us < min_time_to_peer) {
				min_time_to_peer = tp->srtt_us;
				minsk = sk;
			}

			if (tp->srtt_us < best_time_to_peer) {
				if (!mptcp_is_available(sk, skb, zero_wnd_test, true))
					continue;

				if (mptcp_dont_reinject_skb(tp, skb)) {
					backupsk = sk;
					continue;
				}

				best_time_to_peer = tp->srtt_us;
				bestsk = sk;
			}
		}

		// if we decided to use a slower flow, we have the option of not using it at all
		if (bestsk && bestsk != minsk) {
			update_lambda(bestsk, meta_sk);

			struct tcp_sock *meta_tp = tcp_sk(meta_sk);

			struct tcp_sock *mintp  = tcp_sk(minsk);
			struct tcp_sock *besttp = tcp_sk(bestsk);

			// if we send this SKB now, it will be acked in besttp->srtt_us seconds
			// during this time: how many bytes will we send on the fast flow?
			u32 slow_linger_time = estimate_linger_time(bestsk);
			u32 fast_bytes = estimate_bytes(minsk, slow_linger_time);

			// is the required space available in the mptcp meta send window?
			// we assume that all bytes inflight on the slow path will be acked in besttp->srtt_us seconds
			// (just like the SKB if it was sent now) -> that means that those inflight bytes will
			// keep occupying space in the meta window until then
			u32 slow_inflight_bytes = besttp->write_seq - besttp->snd_una;
			u32 slow_bytes = skb->len + slow_inflight_bytes; // bytes of this SKB plus those in flight already

			u32 avail_space = (slow_bytes < meta_tp->snd_wnd) ? (meta_tp->snd_wnd - slow_bytes) : 0;

			tcp_log(mintp, "fast_bytes", fast_bytes);
			tcp_log(mintp, "avail_space", avail_space);
			if (fast_bytes > avail_space) {
				// sending this SKB on the slow flow means
				// we wouldn't be able to send all the data we'd like to send on the fast flow
				// so don't do that
				return NULL;
			}
		}
	}
	else {
		/* First, find the best subflow */
		mptcp_for_each_sk(mpcb, sk) {
			struct tcp_sock *tp = tcp_sk(sk);
	
			if (tp->srtt_us < best_time_to_peer) {
				if (!mptcp_is_available(sk, skb, zero_wnd_test, true))
					continue;
	
				if (mptcp_dont_reinject_skb(tp, skb)) {
					backupsk = sk;
					continue;
				}
	
				best_time_to_peer = tp->srtt_us;
				bestsk = sk;
			}
		}
	}

	if (bestsk) {
		sk = bestsk;
	} else if (backupsk) {
		/* It has been sent on all subflows once - let's give it a
		 * chance again by restarting its pathmask.
		 */
		if (skb)
			TCP_SKB_CB(skb)->path_mask = 0;
		sk = backupsk;
	}

	return sk;
}

static struct sk_buff *mptcp_rcv_buf_optimization(struct sock *sk, int penal)
{
	struct sock *meta_sk;
	struct tcp_sock *tp = tcp_sk(sk), *tp_it;
	struct sk_buff *skb_head;
	struct defsched_priv *dsp = defsched_get_priv(tp);

	if (optim != 1 && optim != 2) // no retrans and no retrans+penal?
		return NULL;

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

	struct tcp_sock *metatp = tcp_sk(meta_sk);
	struct blestsched_priv *bsp = blestsched_get_priv(metatp);
	bsp->retrans_count += 1;

	if (optim != 2) // no penal?
		goto retrans;

	/* Only penalize again after an RTT has elapsed */
	if (tcp_time_stamp - dsp->last_rbuf_opti < tp->srtt_us >> 3)
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

		if (do_retrans && mptcp_is_available(sk, skb_head, false, true)) {
			return skb_head;
		}
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

	*reinject = 0;

	/* If we are in fallback-mode, just take from the meta-send-queue */
	if (mpcb->infinite_mapping_snd || mpcb->send_infinite_mapping)
		return tcp_send_head(meta_sk);

	skb = skb_peek(&mpcb->reinject_queue);

	if (skb) {
		*reinject = 1;
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

// copy from tcp_output.c: tcp_snd_wnd_test
/* Does at least the first segment of SKB fit into the send window? */
static bool mptcp_snd_wnd_test(const struct tcp_sock *tp,
                               const struct sk_buff *skb,
                               unsigned int cur_mss)
{
  u32 end_seq = TCP_SKB_CB(skb)->end_seq;

  if (skb->len > cur_mss)
          end_seq = TCP_SKB_CB(skb)->seq + cur_mss;

  return !after(end_seq, tcp_wnd_end(tp));
}

// copy from tcp_output.c: tcp_cwnd_test
/* Can at least one segment of SKB be sent right now, according to the
 * congestion window rules?  If so, return how many segments are allowed.
 */
static inline unsigned int mptcp_cwnd_test(const struct tcp_sock *tp,
                                           const struct sk_buff *skb)
{
  u32 in_flight, cwnd;

  /* Don't be strict about the congestion window for the final FIN.  */
  if ((TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN) &&
      tcp_skb_pcount(skb) == 1)
    return 1;

  in_flight = tcp_packets_in_flight(tp);
  cwnd = tp->snd_cwnd;
  if (in_flight < cwnd)
    return (cwnd - in_flight);

  return 0;
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

	if (!*reinject && unlikely(!mptcp_snd_wnd_test(tcp_sk(meta_sk), skb, mss_now))) {
		skb = mptcp_rcv_buf_optimization(*subsk, 1);
		if (skb)
			*reinject = -1;
		else
			return NULL;
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
	max_segs = min_t(unsigned int, mptcp_cwnd_test(subtp, skb), gso_max_segs);
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


	struct blestsched_priv *bsp = dsp + 1;
	bsp->lambda_1000 = lambda * 100;
}

static struct mptcp_sched_ops mptcp_blest = {
	.get_subflow = get_available_subflow,
	.next_segment = mptcp_next_segment,
	.init = defsched_init,
	.name = "blest",
	.owner = THIS_MODULE,
};

static int __init blest_register(void)
{
	BUILD_BUG_ON(sizeof(struct defsched_priv) + sizeof(struct blestsched_priv) > MPTCP_SCHED_SIZE);

	if (mptcp_register_scheduler(&mptcp_blest))
		return -1;

	return 0;
}

static void blest_unregister(void)
{
	mptcp_unregister_scheduler(&mptcp_blest);
}

module_init(blest_register);
module_exit(blest_unregister);

MODULE_AUTHOR("Simone Ferlin, Christoph Paasch");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BLEST scheduler for MPTCP, based on default minimum RTT scheduler");
MODULE_VERSION("0.89");
