/* MPTCP Scheduler module selector. Highly inspired by tcp_cong.c */

#include <linux/module.h>
#include <net/mptcp.h>
#include <trace/events/tcp.h>

static DEFINE_SPINLOCK(mptcp_sched_list_lock);
static LIST_HEAD(mptcp_sched_list);

struct defsched_priv {
	u32	last_rbuf_opti;
};

static struct defsched_priv *defsched_get_priv(const struct tcp_sock *tp)
{
	return (struct defsched_priv *)&tp->mptcp->mptcp_sched[0];
}

bool mptcp_is_def_unavailable(struct sock *sk)
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
EXPORT_SYMBOL_GPL(mptcp_is_def_unavailable);

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

	in_flight = tcp_packets_in_flight(tp);
	/* Not even a single spot in the cwnd */
	if (in_flight >= tp->snd_cwnd)
		return true;

	mss_now = tcp_current_mss(sk);

	/* Now, check if what is queued in the subflow's send-queue
	 * already fills the cwnd.
	 */
	space = (tp->snd_cwnd - in_flight) * mss_now;

	if (tp->write_seq - tp->snd_nxt >= space)
		return true;

	if (zero_wnd_test && !before(tp->write_seq, tcp_wnd_end(tp)))
		return true;

	/* Don't send on this subflow if we bypass the allowed send-window at
	 * the per-subflow level. Similar to tcp_snd_wnd_test, but manually
	 * calculated end_seq (because here at this point end_seq is still at
	 * the meta-level).
	 */
	if (skb && zero_wnd_test &&
	    after(tp->write_seq + min(skb->len, mss_now), tcp_wnd_end(tp)))
		return true;

	return false;
}

/* Is the sub-socket sk available to send the skb? */
bool mptcp_is_available(struct sock *sk, const struct sk_buff *skb,
			bool zero_wnd_test)
{
	return !mptcp_is_def_unavailable(sk) &&
	       !mptcp_is_temp_unavailable(sk, skb, zero_wnd_test);
}
EXPORT_SYMBOL_GPL(mptcp_is_available);

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

bool subflow_is_backup(const struct tcp_sock *tp)
{
	return tp->mptcp->rcv_low_prio || tp->mptcp->low_prio;
}
EXPORT_SYMBOL_GPL(subflow_is_backup);

bool subflow_is_active(const struct tcp_sock *tp)
{
	return !tp->mptcp->rcv_low_prio && !tp->mptcp->low_prio;
}
EXPORT_SYMBOL_GPL(subflow_is_active);

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
	struct mptcp_tcp_sock *mptcp;

	mptcp_for_each_sub(mpcb, mptcp) {
		struct sock *sk = mptcp_to_sock(mptcp);
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

		if (tp->srtt_us < min_srtt) {
			min_srtt = tp->srtt_us;
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

	return bestsk;
}

/* This is the scheduler. This function decides on which flow to send
 * a given MSS. If all subflows are found to be busy, NULL is returned
 * The flow is selected based on the shortest RTT.
 * If all paths have full cong windows, we simply return NULL.
 *
 * Additionally, this function is aware of the backup-subflows.
 */
struct sock *get_available_subflow(struct sock *meta_sk, struct sk_buff *skb,
				   bool zero_wnd_test)
{
	struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
	struct sock *sk;
	bool looping = false, force;

	/* Answer data_fin on same subflow!!! */
	if (meta_sk->sk_shutdown & RCV_SHUTDOWN &&
	    skb && mptcp_is_data_fin(skb)) {
		struct mptcp_tcp_sock *mptcp;

		mptcp_for_each_sub(mpcb, mptcp) {
			sk = mptcp_to_sock(mptcp);

			if (tcp_sk(sk)->mptcp->path_index == mpcb->dfin_path_index &&
			    mptcp_is_available(sk, skb, zero_wnd_test))
				return sk;
		}
	}

	/* Find the best subflow */
restart:
	sk = get_subflow_from_selectors(mpcb, skb, &subflow_is_active,
					zero_wnd_test, &force);
	if (force)
		/* one unused active sk or one NULL sk when there is at least
		 * one temporally unavailable unused active sk
		 */
		return sk;

	sk = get_subflow_from_selectors(mpcb, skb, &subflow_is_backup,
					zero_wnd_test, &force);
	if (!force && skb) {
		/* one used backup sk or one NULL sk where there is no one
		 * temporally unavailable unused backup sk
		 *
		 * the skb passed through all the available active and backups
		 * sks, so clean the path mask
		 */
		TCP_SKB_CB(skb)->path_mask = 0;

		if (!looping) {
			looping = true;
			goto restart;
		}
	}
	return sk;
}
EXPORT_SYMBOL_GPL(get_available_subflow);

static struct sk_buff *mptcp_rcv_buf_optimization(struct sock *sk, int penal)
{
	struct sock *meta_sk;
	const struct tcp_sock *tp = tcp_sk(sk);
	struct mptcp_tcp_sock *mptcp;
	struct sk_buff *skb_head;
	struct defsched_priv *def_p = defsched_get_priv(tp);

	meta_sk = mptcp_meta_sk(sk);
	skb_head = tcp_rtx_queue_head(meta_sk);

	if (!skb_head)
		return NULL;

	/* If penalization is optional (coming from mptcp_next_segment() and
	 * We are not send-buffer-limited we do not penalize. The retransmission
	 * is just an optimization to fix the idle-time due to the delay before
	 * we wake up the application.
	 */
	if (!penal && sk_stream_memory_free(meta_sk))
		goto retrans;

	/* Only penalize again after an RTT has elapsed */
	if (tcp_jiffies32 - def_p->last_rbuf_opti < usecs_to_jiffies(tp->srtt_us >> 3))
		goto retrans;

	/* Half the cwnd of the slow flows */
	mptcp_for_each_sub(tp->mpcb, mptcp) {
		struct tcp_sock *tp_it = mptcp->tp;

		if (tp_it != tp &&
		    TCP_SKB_CB(skb_head)->path_mask & mptcp_pi_to_flag(tp_it->mptcp->path_index)) {
			if (tp->srtt_us < tp_it->srtt_us && inet_csk((struct sock *)tp_it)->icsk_ca_state == TCP_CA_Open) {
				u32 prior_cwnd = tp_it->snd_cwnd;

				tp_it->snd_cwnd = max(tp_it->snd_cwnd >> 1U, 1U);

				/* If in slow start, do not reduce the ssthresh */
				if (prior_cwnd >= tp_it->snd_ssthresh)
					tp_it->snd_ssthresh = max(tp_it->snd_ssthresh >> 1U, 2U);

				def_p->last_rbuf_opti = tcp_jiffies32;
			}
		}
	}

retrans:

	/* Segment not yet injected into this path? Take it!!! */
	if (!(TCP_SKB_CB(skb_head)->path_mask & mptcp_pi_to_flag(tp->mptcp->path_index))) {
		bool do_retrans = false;
		mptcp_for_each_sub(tp->mpcb, mptcp) {
			struct tcp_sock *tp_it = mptcp->tp;

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

		if (do_retrans && mptcp_is_available(sk, skb_head, false)) {
			trace_mptcp_retransmit(sk, skb_head);
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
	const struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
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
			struct sock *subsk;

			/* meta is send buffer limited */
			tcp_chrono_start(meta_sk, TCP_CHRONO_SNDBUF_LIMITED);

			subsk = mpcb->sched_ops->get_subflow(meta_sk,
							     NULL, false);
			if (!subsk)
				return NULL;

			skb = mptcp_rcv_buf_optimization(subsk, 0);
			if (skb)
				*reinject = -1;
			else
				tcp_chrono_start(subsk,
						 TCP_CHRONO_SNDBUF_LIMITED);
		}
	}
	return skb;
}

struct sk_buff *mptcp_next_segment(struct sock *meta_sk,
					  int *reinject,
					  struct sock **subsk,
					  unsigned int *limit)
{
	struct sk_buff *skb = __mptcp_next_segment(meta_sk, reinject);
	unsigned int mss_now, in_flight_space;
	int remaining_in_flight_space;
	u32 max_len, max_segs, window;
	struct tcp_sock *subtp;
	u16 gso_max_segs;

	/* As we set it, we have to reset it as well. */
	*limit = 0;

	if (!skb)
		return NULL;

	*subsk = tcp_sk(meta_sk)->mpcb->sched_ops->get_subflow(meta_sk, skb, false);
	if (!*subsk)
		return NULL;

	subtp = tcp_sk(*subsk);
	mss_now = tcp_current_mss(*subsk);

	if (!*reinject && unlikely(!tcp_snd_wnd_test(tcp_sk(meta_sk), skb, mss_now))) {
		/* an active flow is selected, but segment will not be sent due
		 * to no more space in send window
		 * this means the meta is receive window limited
		 * the subflow might also be, if we have nothing to reinject
		 */
		tcp_chrono_start(meta_sk, TCP_CHRONO_RWND_LIMITED);
		skb = mptcp_rcv_buf_optimization(*subsk, 1);
		if (skb)
			*reinject = -1;
		else
			return NULL;
	}

	if (!*reinject) {
		/* this will stop any other chronos on the meta */
		tcp_chrono_start(meta_sk, TCP_CHRONO_BUSY);
	}

	/* No splitting required, as we will only send one single segment */
	if (skb->len <= mss_now)
		return skb;

	/* The following is similar to tcp_mss_split_point, but
	 * we do not care about nagle, because we will anyways
	 * use TCP_NAGLE_PUSH, which overrides this.
	 */

	gso_max_segs = (*subsk)->sk_gso_max_segs;
	if (!gso_max_segs) /* No gso supported on the subflow's NIC */
		gso_max_segs = 1;
	max_segs = min_t(unsigned int, tcp_cwnd_test(subtp, skb), gso_max_segs);
	if (!max_segs)
		return NULL;

	/* max_len is what would fit in the cwnd (respecting the 2GSO-limit of
	 * tcp_cwnd_test), but ignoring whatever was already queued.
	 */
	max_len = min(mss_now * max_segs, skb->len);

	in_flight_space = (subtp->snd_cwnd - tcp_packets_in_flight(subtp)) * mss_now;
	remaining_in_flight_space = (int)in_flight_space - (subtp->write_seq - subtp->snd_nxt);

	if (remaining_in_flight_space <= 0)
		WARN_ONCE(1, "in_flight %u cwnd %u wseq %u snxt %u mss_now %u cache %u",
			  tcp_packets_in_flight(subtp), subtp->snd_cwnd,
			  subtp->write_seq, subtp->snd_nxt, mss_now, subtp->mss_cache);
	else
		/* max_len now fits exactly in the write-queue, taking into
		 * account what was already queued.
		 */
		max_len = min_t(u32, max_len, remaining_in_flight_space);

	window = tcp_wnd_end(subtp) - subtp->write_seq;

	/* max_len now also respects the announced receive-window */
	max_len = min(max_len, window);

	*limit = max_len;

	return skb;
}
EXPORT_SYMBOL_GPL(mptcp_next_segment);

static void defsched_init(struct sock *sk)
{
	struct defsched_priv *def_p = defsched_get_priv(tcp_sk(sk));

	def_p->last_rbuf_opti = tcp_jiffies32;
}

struct mptcp_sched_ops mptcp_sched_default = {
	.get_subflow = get_available_subflow,
	.next_segment = mptcp_next_segment,
	.init = defsched_init,
	.name = "default",
	.owner = THIS_MODULE,
};

static struct mptcp_sched_ops *mptcp_sched_find(const char *name)
{
	struct mptcp_sched_ops *e;

	list_for_each_entry_rcu(e, &mptcp_sched_list, list) {
		if (strcmp(e->name, name) == 0)
			return e;
	}

	return NULL;
}

int mptcp_register_scheduler(struct mptcp_sched_ops *sched)
{
	int ret = 0;

	if (!sched->get_subflow || !sched->next_segment)
		return -EINVAL;

	spin_lock(&mptcp_sched_list_lock);
	if (mptcp_sched_find(sched->name)) {
		pr_notice("%s already registered\n", sched->name);
		ret = -EEXIST;
	} else {
		list_add_tail_rcu(&sched->list, &mptcp_sched_list);
		pr_info("%s registered\n", sched->name);
	}
	spin_unlock(&mptcp_sched_list_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mptcp_register_scheduler);

void mptcp_unregister_scheduler(struct mptcp_sched_ops *sched)
{
	spin_lock(&mptcp_sched_list_lock);
	list_del_rcu(&sched->list);
	spin_unlock(&mptcp_sched_list_lock);

	/* Wait for outstanding readers to complete before the
	 * module gets removed entirely.
	 *
	 * A try_module_get() should fail by now as our module is
	 * in "going" state since no refs are held anymore and
	 * module_exit() handler being called.
	 */
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(mptcp_unregister_scheduler);

void mptcp_get_default_scheduler(char *name)
{
	struct mptcp_sched_ops *sched;

	BUG_ON(list_empty(&mptcp_sched_list));

	rcu_read_lock();
	sched = list_entry(mptcp_sched_list.next, struct mptcp_sched_ops, list);
	strncpy(name, sched->name, MPTCP_SCHED_NAME_MAX);
	rcu_read_unlock();
}

int mptcp_set_default_scheduler(const char *name)
{
	struct mptcp_sched_ops *sched;
	int ret = -ENOENT;

	spin_lock(&mptcp_sched_list_lock);
	sched = mptcp_sched_find(name);
#ifdef CONFIG_MODULES
	if (!sched && capable(CAP_NET_ADMIN)) {
		spin_unlock(&mptcp_sched_list_lock);

		request_module("mptcp_%s", name);
		spin_lock(&mptcp_sched_list_lock);
		sched = mptcp_sched_find(name);
	}
#endif

	if (sched) {
		list_move(&sched->list, &mptcp_sched_list);
		ret = 0;
	} else {
		pr_info("%s is not available\n", name);
	}
	spin_unlock(&mptcp_sched_list_lock);

	return ret;
}

/* Must be called with rcu lock held */
static struct mptcp_sched_ops *__mptcp_sched_find_autoload(const char *name)
{
	struct mptcp_sched_ops *sched = mptcp_sched_find(name);
#ifdef CONFIG_MODULES
	if (!sched && capable(CAP_NET_ADMIN)) {
		rcu_read_unlock();
		request_module("mptcp_%s", name);
		rcu_read_lock();
		sched = mptcp_sched_find(name);
	}
#endif
	return sched;
}

void mptcp_init_scheduler(struct mptcp_cb *mpcb)
{
	struct mptcp_sched_ops *sched;
	struct sock *meta_sk = mpcb->meta_sk;
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);

	rcu_read_lock();
	/* if scheduler was set using socket option */
	if (meta_tp->mptcp_sched_setsockopt) {
		sched = __mptcp_sched_find_autoload(meta_tp->mptcp_sched_name);
		if (sched && try_module_get(sched->owner)) {
			mpcb->sched_ops = sched;
			goto out;
		}
	}

	list_for_each_entry_rcu(sched, &mptcp_sched_list, list) {
		if (try_module_get(sched->owner)) {
			mpcb->sched_ops = sched;
			break;
		}
	}
out:
	rcu_read_unlock();
}

/* Change scheduler for socket */
int mptcp_set_scheduler(struct sock *sk, const char *name)
{
	struct mptcp_sched_ops *sched;
	int err = 0;

	rcu_read_lock();
	sched = __mptcp_sched_find_autoload(name);

	if (!sched) {
		err = -ENOENT;
	} else if (!ns_capable(sock_net(sk)->user_ns, CAP_NET_ADMIN)) {
		err = -EPERM;
	} else {
		strcpy(tcp_sk(sk)->mptcp_sched_name, name);
		tcp_sk(sk)->mptcp_sched_setsockopt = 1;
	}
	rcu_read_unlock();

	return err;
}

/* Manage refcounts on socket close. */
void mptcp_cleanup_scheduler(struct mptcp_cb *mpcb)
{
	module_put(mpcb->sched_ops->owner);
}

/* Set default value from kernel configuration at bootup */
static int __init mptcp_scheduler_default(void)
{
	BUILD_BUG_ON(sizeof(struct defsched_priv) > MPTCP_SCHED_SIZE);

	return mptcp_set_default_scheduler(CONFIG_DEFAULT_MPTCP_SCHED);
}
late_initcall(mptcp_scheduler_default);
