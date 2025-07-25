/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Copyright Authors of Cilium */

#pragma once

#include <bpf/ctx/ctx.h>
#include <bpf/api.h>

#include "bpf/compiler.h"
#include "tailcall.h"
#include "nat.h"
#include "edt.h"
#include "l3.h"
#include "lb.h"
#include "common.h"
#include "overloadable.h"
#include "egress_gateway.h"
#include "eps.h"
#include "conntrack.h"
#include "csum.h"
#include "encap.h"
#include "identity.h"
#include "trace.h"
#include "ghash.h"
#include "pcap.h"
#include "host_firewall.h"
#include "stubs.h"
#include "proxy_hairpin.h"
#include "fib.h"
#include "srv6.h"

DECLARE_CONFIG(__u16, device_mtu, "MTU of the device the bpf program is attached to (default: MTU set in node_config.h by agent)")
ASSIGN_CONFIG(__u16, device_mtu, MTU)
#define THIS_MTU CONFIG(device_mtu) /* Backwards compatibility */

/* Evaluate the input values for detecting compilation errors.
 * Just blindly substituting this macro with the CTX_ACT_OK
 * kills the C compile-time check against the input values
 * (e.g. it can pass type mismatching, it can pass undefined
 * variables, etc.). This whole block is evaluated to CTX_ACT_OK.
 */
#define nodeport_nat_egress_ipv4_hook(ctx, ip4, info, tuple, l4_off, ext_err) \
({ \
	struct __ctx_buff *_ctx __maybe_unused = ctx; \
	struct iphdr *_ip4 __maybe_unused = ip4; \
	__u32 _info __maybe_unused = info; \
	struct ipv4_ct_tuple *_tuple __maybe_unused = tuple; \
	int _l4_off __maybe_unused = l4_off; \
	__s8 *_ext_err __maybe_unused = ext_err; \
	CTX_ACT_OK; \
})

#define nodeport_rev_dnat_ipv4_hook(ctx, ip4, tuple, tunnel_endpoint, src_sec_identity, \
		dst_sec_identity) \
({ \
	struct __ctx_buff *_ctx __maybe_unused = ctx; \
	struct iphdr *_ip4 __maybe_unused = ip4; \
	struct ipv4_ct_tuple *_tuple __maybe_unused = tuple; \
	__u32 *_tunnel_endpoint __maybe_unused = tunnel_endpoint; \
	__u32 *_src_sec_identity __maybe_unused = src_sec_identity; \
	__u32 *_dst_sec_identity __maybe_unused = dst_sec_identity; \
	-1; \
})

#ifdef ENABLE_NODEPORT
/* The IPv6 extension should be 8-bytes aligned */
struct dsr_opt_v6 {
	struct ipv6_opt_hdr hdr;
	__u8 opt_type;
	__u8 opt_len;
	union v6addr addr;
	__be16 port;
	__u16 pad;
};

struct dsr_opt_v4 {
	__u8 type;
	__u8 len;
	__u16 port;
	__u32 addr;
};

static __always_inline bool nodeport_uses_dsr(bool flip __maybe_unused,
					      __u8 nexthdr __maybe_unused)
{
#ifdef ENABLE_DSR
# ifdef ENABLE_DSR_HYBRID
#  ifdef ENABLE_DSR_BYUSER
	return flip;
#  else
	if (nexthdr == IPPROTO_TCP)
		return true;
	return false;
#  endif
# else
	return true;
# endif
#else
	return false;
#endif
}

#ifdef HAVE_ENCAP
static __always_inline int
nodeport_add_tunnel_encap(struct __ctx_buff *ctx, __u32 src_ip, __be16 src_port,
			  const struct remote_endpoint_info *info,
			  __u32 src_sec_identity, enum trace_reason ct_reason,
			  __u32 monitor, int *ifindex, __be16 proto)
{
	/* Let kernel choose the outer source ip */
	if (ctx_is_skb())
		src_ip = 0;

	/* Append L2 hdr before redirecting to tunnel netdev.
	 * Otherwise, the kernel will drop such request in
	 * https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/net/core/filter.c?h=v6.7.4#n2147
	 */
	if (ETH_HLEN == 0) {
		int ret;

		ret = add_l2_hdr(ctx);
		if (ret != 0)
			return ret;
	}

	if (info->flag_ipv6_tunnel_ep)
		return __encap_with_nodeid6(ctx, &info->tunnel_endpoint.ip6,
					    src_sec_identity, info->sec_identity,
					    ct_reason, monitor, ifindex, proto);
	return __encap_with_nodeid4(ctx, src_ip, src_port, info->tunnel_endpoint.ip4,
				    src_sec_identity, info->sec_identity, NOT_VTEP_DST,
				    ct_reason, monitor, ifindex, proto);
}

# if defined(ENABLE_DSR) && DSR_ENCAP_MODE == DSR_ENCAP_GENEVE
static __always_inline int
nodeport_add_tunnel_encap_opt(struct __ctx_buff *ctx, __u32 src_ip, __be16 src_port,
			      const struct remote_endpoint_info *info,
			      __u32 src_sec_identity, void *opt, __u32 opt_len,
			      enum trace_reason ct_reason, __u32 monitor,
			      int *ifindex, __be16 proto)
{
	/* Let kernel choose the outer source ip */
	if (ctx_is_skb())
		src_ip = 0;

	/* Append L2 hdr before redirecting to tunnel netdev.
	 * Otherwise, the kernel will drop such request in
	 * https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/net/core/filter.c?h=v6.7.4#n2147
	 */
	if (ETH_HLEN == 0) {
		int ret;

		ret = add_l2_hdr(ctx);
		if (ret != 0)
			return ret;
	}

	if (info->flag_ipv6_tunnel_ep)
		return __encap_with_nodeid_opt6(ctx, &info->tunnel_endpoint.ip6,
						src_sec_identity, info->sec_identity,
						opt, opt_len, ct_reason, monitor,
						ifindex, proto);
	return __encap_with_nodeid_opt4(ctx, src_ip, src_port, info->tunnel_endpoint.ip4,
					src_sec_identity, info->sec_identity, NOT_VTEP_DST,
					opt, opt_len, ct_reason, monitor, ifindex, proto);
}
# endif
#endif /* HAVE_ENCAP */

static __always_inline bool dsr_fail_needs_reply(int code __maybe_unused)
{
#ifdef ENABLE_DSR_ICMP_ERRORS
	if (code == DROP_FRAG_NEEDED)
		return true;
#endif
	return false;
}

static __always_inline bool dsr_is_too_big(struct __ctx_buff *ctx __maybe_unused,
					   __u16 expanded_len __maybe_unused)
{
#ifdef ENABLE_DSR_ICMP_ERRORS
	if (expanded_len > THIS_MTU)
		return true;
#endif
	return false;
}

static __always_inline int
nodeport_fib_lookup_and_redirect(struct __ctx_buff *ctx,
				 struct bpf_fib_lookup_padded *fib_params,
				 __s8 *ext_err)
{
	__u32 oif;
	int ret;

	ret = (int)fib_lookup(ctx, &fib_params->l, sizeof(fib_params->l), 0);

	switch (ret) {
	case BPF_FIB_LKUP_RET_SUCCESS:
	case BPF_FIB_LKUP_RET_NO_NEIGH:
		oif = fib_params->l.ifindex;

		if (oif == THIS_INTERFACE_IFINDEX)
			return CTX_ACT_OK;

		return fib_do_redirect(ctx, true, fib_params, true, ret, oif, ext_err);
	default:
		*ext_err = (__s8)ret;
		return DROP_NO_FIB;
	}
}

#ifdef ENABLE_IPV6
static __always_inline bool nodeport_uses_dsr6(const struct lb6_service *svc,
					       const struct ipv6_ct_tuple *tuple)
{
	return nodeport_uses_dsr(svc->flags2 & SVC_FLAG_FWD_MODE_DSR,
				 tuple->nexthdr);
}

static __always_inline bool nodeport_xlate6(const struct lb6_service *svc,
					    const struct ipv6_ct_tuple *tuple)
{
	bool skip_xlate = DSR_ENCAP_MODE == DSR_ENCAP_IPIP;

	if (skip_xlate)
		skip_xlate = nodeport_uses_dsr6(svc, tuple);
	return skip_xlate;
}

#ifdef ENABLE_DSR
#if DSR_ENCAP_MODE == DSR_ENCAP_IPIP
static __always_inline void rss_gen_src6(union v6addr *src,
					 const union v6addr *client,
					 __be32 l4_hint)
{
	__u32 bits = 128 - IPV6_RSS_PREFIX_BITS;

	*src = (union v6addr)IPV6_RSS_PREFIX;
	if (bits) {
		__u32 todo;

		if (bits > 96) {
			todo = bits - 96;
			src->p1 |= bpf_htonl(hash_32(client->p1 ^ l4_hint, todo));
			bits -= todo;
		}
		if (bits > 64) {
			todo = bits - 64;
			src->p2 |= bpf_htonl(hash_32(client->p2 ^ l4_hint, todo));
			bits -= todo;
		}
		if (bits > 32) {
			todo = bits - 32;
			src->p3 |= bpf_htonl(hash_32(client->p3 ^ l4_hint, todo));
			bits -= todo;
		}
		src->p4 |= bpf_htonl(hash_32(client->p4 ^ l4_hint, bits));
	}
}

static __always_inline int dsr_set_ipip6(struct __ctx_buff *ctx,
					 const struct ipv6hdr *ip6,
					 const union v6addr *backend_addr,
					 __be32 l4_hint, int *ohead)
{
	__u16 payload_len = bpf_ntohs(ip6->payload_len) + sizeof(*ip6);
	const int l3_off = ETH_HLEN;
	union v6addr saddr;
	struct {
		__be16 payload_len;
		__u8 nexthdr;
		__u8 hop_limit;
	} tp_new = {
		.payload_len	= bpf_htons(payload_len),
		.nexthdr	= IPPROTO_IPV6,
		.hop_limit	= IPDEFTTL,
	};

	if (dsr_is_too_big(ctx, payload_len + sizeof(*ip6))) {
		*ohead = sizeof(*ip6);
		return DROP_FRAG_NEEDED;
	}

	rss_gen_src6(&saddr, (union v6addr *)&ip6->saddr, l4_hint);

	if (ctx_adjust_hroom(ctx, sizeof(*ip6), BPF_ADJ_ROOM_NET,
			     ctx_adjust_hroom_flags()))
		return DROP_INVALID;
	if (ctx_store_bytes(ctx, l3_off + offsetof(struct ipv6hdr, payload_len),
			    &tp_new.payload_len, 4, 0) < 0)
		return DROP_WRITE_ERROR;
	if (ctx_store_bytes(ctx, l3_off + offsetof(struct ipv6hdr, daddr),
			    backend_addr, sizeof(ip6->daddr), 0) < 0)
		return DROP_WRITE_ERROR;
	if (ctx_store_bytes(ctx, l3_off + offsetof(struct ipv6hdr, saddr),
			    &saddr, sizeof(ip6->saddr), 0) < 0)
		return DROP_WRITE_ERROR;
	return 0;
}
#elif DSR_ENCAP_MODE == DSR_ENCAP_NONE
static __always_inline int dsr_set_ext6(struct __ctx_buff *ctx,
					struct ipv6hdr *ip6,
					const union v6addr *svc_addr,
					__be16 svc_port, int *ohead)
{
	struct dsr_opt_v6 opt __align_stack_8 = {};
	__u16 payload_len = bpf_ntohs(ip6->payload_len) + sizeof(opt);
	__u16 total_len = bpf_ntohs(ip6->payload_len) + sizeof(struct ipv6hdr) + sizeof(opt);
	__u8 nexthdr = ip6->nexthdr;
	int hdrlen;

	/* The IPv6 extension should be 8-bytes aligned */
	build_bug_on((sizeof(struct dsr_opt_v6) % 8) != 0);

	hdrlen = ipv6_hdrlen(ctx, &nexthdr);
	if (hdrlen < 0)
		return hdrlen;

	/* See dsr_set_opt4(): */
	if (nexthdr == IPPROTO_TCP) {
		union tcp_flags tcp_flags = { .value = 0 };

		if (l4_load_tcp_flags(ctx, ETH_HLEN + hdrlen, &tcp_flags) < 0)
			return DROP_CT_INVALID_HDR;

		if (!(tcp_flags.value & (TCP_FLAG_SYN)))
			return 0;
	}

	if (dsr_is_too_big(ctx, total_len)) {
		*ohead = sizeof(opt);
		return DROP_FRAG_NEEDED;
	}

	opt.hdr.nexthdr = ip6->nexthdr;
	ip6->nexthdr = NEXTHDR_DEST;
	ip6->payload_len = bpf_htons(payload_len);

	opt.hdr.hdrlen = DSR_IPV6_EXT_LEN;
	opt.opt_type = DSR_IPV6_OPT_TYPE;
	opt.opt_len = DSR_IPV6_OPT_LEN;
	ipv6_addr_copy_unaligned(&opt.addr, svc_addr);
	opt.port = svc_port;

	if (ctx_adjust_hroom(ctx, sizeof(opt), BPF_ADJ_ROOM_NET,
			     ctx_adjust_hroom_flags()))
		return DROP_INVALID;
	if (ctx_store_bytes(ctx, ETH_HLEN + sizeof(*ip6), &opt,
			    sizeof(opt), 0) < 0)
		return DROP_INVALID;
	return 0;
}
#elif DSR_ENCAP_MODE == DSR_ENCAP_GENEVE
static __always_inline int encap_geneve_dsr_opt6(struct __ctx_buff *ctx,
						 struct ipv6hdr *ip6,
						 const union v6addr *svc_addr,
						 __be16 svc_port,
						 int *ifindex, int *ohead)
{
	struct remote_endpoint_info *info;
	struct ipv6_ct_tuple tuple __align_stack_8 = {};
	struct geneve_dsr_opt6 gopt;
	union v6addr *dst;
	bool need_opt = true;
	__u16 encap_len = sizeof(struct ipv6hdr) + sizeof(struct udphdr) +
		sizeof(struct genevehdr) + ETH_HLEN;
	__u16 payload_len = bpf_ntohs(ip6->payload_len) + sizeof(*ip6);
	fraginfo_t fraginfo;
	__u16 total_len = 0;
	__be16 src_port;
	int l4_off, ret;

	build_bug_on((sizeof(gopt) % 4) != 0);

	dst = (union v6addr *)&ip6->daddr;
	info = lookup_ip6_remote_endpoint(dst, 0);
	if (!info || !info->flag_has_tunnel_ep)
		return DROP_NO_TUNNEL_ENDPOINT;

	tuple.nexthdr = ip6->nexthdr;
	ret = ipv6_hdrlen_with_fraginfo(ctx, &tuple.nexthdr, &fraginfo);
	if (ret < 0)
		return ret;

	l4_off = ETH_HLEN + ret;

	ret = lb6_extract_tuple(ctx, ip6, fraginfo, l4_off, &tuple);
	if (IS_ERR(ret))
		return ret;

	src_port = tunnel_gen_src_port_v6(&tuple);

	/* See encap_geneve_dsr_opt4(): */
	if (tuple.nexthdr == IPPROTO_TCP) {
		union tcp_flags tcp_flags = { .value = 0 };

		if (l4_load_tcp_flags(ctx, l4_off, &tcp_flags) < 0)
			return DROP_CT_INVALID_HDR;

		if (!(tcp_flags.value & (TCP_FLAG_SYN)))
			need_opt = false;
	}

	if (need_opt) {
		encap_len += sizeof(struct geneve_dsr_opt6);
		set_geneve_dsr_opt6(svc_port, svc_addr, &gopt);
	}

	total_len = encap_len + payload_len;

	if (dsr_is_too_big(ctx, total_len)) {
		*ohead = encap_len;
		return DROP_FRAG_NEEDED;
	}

	if (need_opt)
		return nodeport_add_tunnel_encap_opt(ctx,
						     IPV4_DIRECT_ROUTING,
						     src_port,
						     info,
						     WORLD_IPV6_ID,
						     &gopt,
						     sizeof(gopt),
						     (enum trace_reason)CT_NEW,
						     TRACE_PAYLOAD_LEN,
						     ifindex,
						     bpf_htons(ETH_P_IPV6));

	return nodeport_add_tunnel_encap(ctx,
					 IPV4_DIRECT_ROUTING,
					 src_port,
					 info,
					 WORLD_IPV6_ID,
					 (enum trace_reason)CT_NEW,
					 TRACE_PAYLOAD_LEN,
					 ifindex,
					 bpf_htons(ETH_P_IPV6));
}
#endif /* DSR_ENCAP_MODE */

static __always_inline int find_dsr_v6(struct __ctx_buff *ctx, __u8 nexthdr,
				       struct dsr_opt_v6 *dsr_opt, bool *found)
{
	int i, len = sizeof(struct ipv6hdr);
	__u8 nh = nexthdr;

#pragma unroll
	for (i = 0; i < IPV6_MAX_HEADERS; i++) {
		__u8 newnh = nh;
		int hdrlen = ipv6_skip_exthdr(ctx, &newnh, ETH_HLEN + len);

		if (hdrlen < 0)
			return hdrlen;

		if (!hdrlen)
			return 0;

		build_bug_on(sizeof(*dsr_opt) != 24);
		if (nh == NEXTHDR_DEST && hdrlen == sizeof(*dsr_opt)) {
			if (ctx_load_bytes(ctx, ETH_HLEN + len, dsr_opt, sizeof(*dsr_opt)) < 0)
				return DROP_INVALID;
			if (dsr_opt->opt_type == DSR_IPV6_OPT_TYPE &&
			    dsr_opt->opt_len == DSR_IPV6_OPT_LEN) {
				*found = true;
				return 0;
			}
		}

		len += hdrlen;
		nh = newnh;
	}

	/* Reached limit of supported extension headers */
	return DROP_INVALID_EXTHDR;
}

static __always_inline int
nodeport_extract_dsr_v6(struct __ctx_buff *ctx,
			struct ipv6hdr *ip6 __maybe_unused,
			const struct ipv6_ct_tuple *tuple, int l4_off,
			union v6addr *addr, __be16 *port, bool *dsr)
{
	struct ipv6_ct_tuple tmp = *tuple;

	if (tuple->nexthdr == IPPROTO_TCP) {
		union tcp_flags tcp_flags = {};

		if (l4_load_tcp_flags(ctx, l4_off, &tcp_flags) < 0)
			return DROP_CT_INVALID_HDR;

		ipv6_ct_tuple_reverse(&tmp);

		if (!(tcp_flags.value & TCP_FLAG_SYN)) {
			*dsr = ct_has_dsr_egress_entry6(get_ct_map6(&tmp), &tmp);
			*port = 0;
			return 0;
		}
	}

#if defined(IS_BPF_OVERLAY)
	{
		struct geneve_dsr_opt6 gopt;
		int ret = ctx_get_tunnel_opt(ctx, &gopt, sizeof(gopt));

		if (ret > 0) {
			if (gopt.hdr.opt_class == bpf_htons(DSR_GENEVE_OPT_CLASS) &&
			    gopt.hdr.type == DSR_GENEVE_OPT_TYPE) {
				*dsr = true;
				*port = gopt.port;
				ipv6_addr_copy_unaligned(addr,
							 (union v6addr *)&gopt.addr);
				return 0;
			}
		}
	}
#else
	{
		struct dsr_opt_v6 opt __align_stack_8 = {};
		int ret;

		ret = find_dsr_v6(ctx, ip6->nexthdr, &opt, dsr);
		if (ret != 0)
			return ret;

		if (*dsr) {
			*addr = opt.addr;
			*port = opt.port;
			return 0;
		}
	}
#endif

	/* SYN for a new connection that's not / no longer DSR.
	 * If it's reopened, avoid sending subsequent traffic down the DSR path.
	 */
	if (tuple->nexthdr == IPPROTO_TCP)
		ct_update_dsr(get_ct_map6(&tmp), &tmp, false);

	return 0;
}

static __always_inline struct ipv6_nat_entry *
nodeport_dsr_lookup_v6_nat_entry(const struct ipv6_ct_tuple *nat_tuple)
{
	return snat_v6_lookup(nat_tuple);
}

static __always_inline int dsr_reply_icmp6(struct __ctx_buff *ctx,
					   const struct ipv6hdr *ip6 __maybe_unused,
					   const union v6addr *svc_addr __maybe_unused,
					   __be16 dport __maybe_unused,
					   int code, int ohead __maybe_unused)
{
#ifdef ENABLE_DSR_ICMP_ERRORS
	const __s32 orig_dgram = 64, off = ETH_HLEN;
	__u8 orig_ipv6_hdr[orig_dgram];
	__be16 type = bpf_htons(ETH_P_IPV6);
	__u64 len_new = off + sizeof(*ip6) + orig_dgram;
	__u64 len_old = ctx_full_len(ctx);
	void *data_end = ctx_data_end(ctx);
	void *data = ctx_data(ctx);
	__u8 reason = (__u8)-code;
	__wsum wsum;
	union macaddr smac, dmac;
	struct icmp6hdr icmp __align_stack_8 = {
		.icmp6_type	= ICMPV6_PKT_TOOBIG,
		.icmp6_mtu	= bpf_htonl(THIS_MTU - ohead),
	};
	__u64 payload_len = sizeof(*ip6) + sizeof(icmp) + orig_dgram;
	struct ipv6hdr ip __align_stack_8 = {
		.version	= 6,
		.priority	= ip6->priority,
		.flow_lbl[0]	= ip6->flow_lbl[0],
		.flow_lbl[1]	= ip6->flow_lbl[1],
		.flow_lbl[2]	= ip6->flow_lbl[2],
		.nexthdr	= IPPROTO_ICMPV6,
		.hop_limit	= IPDEFTTL,
		.saddr		= ip6->daddr,
		.daddr		= ip6->saddr,
		.payload_len	= bpf_htons((__u16)payload_len),
	};
	struct ipv6hdr inner_ipv6_hdr __align_stack_8 = *ip6;
	__s32 l4_dport_offset;

	/* DSR changes the destination address from service ip to pod ip and
	 * destination port from service port to pod port. While resppnding
	 * back with ICMP error, it is necessary to set it to original ip and
	 * port.
	 */
	ipv6_addr_copy((union v6addr *)&inner_ipv6_hdr.daddr, svc_addr);

	if (inner_ipv6_hdr.nexthdr == IPPROTO_UDP)
		l4_dport_offset = UDP_DPORT_OFF;
	else if (inner_ipv6_hdr.nexthdr == IPPROTO_TCP)
		l4_dport_offset = TCP_DPORT_OFF;
	else
		goto drop_err;

	if (ctx_load_bytes(ctx, off + sizeof(inner_ipv6_hdr), orig_ipv6_hdr,
			   (__u32)sizeof(orig_ipv6_hdr)) < 0)
		goto drop_err;
	memcpy(orig_ipv6_hdr + l4_dport_offset, &dport, sizeof(dport));

	update_metrics(ctx_full_len(ctx), METRIC_EGRESS, reason);

	if (eth_load_saddr(ctx, smac.addr, 0) < 0)
		goto drop_err;
	if (eth_load_daddr(ctx, dmac.addr, 0) < 0)
		goto drop_err;
	if (unlikely(data + len_new > data_end))
		goto drop_err;

	wsum = ipv6_pseudohdr_checksum(&ip, IPPROTO_ICMPV6,
				       bpf_ntohs(ip.payload_len), 0);
	icmp.icmp6_cksum = csum_fold(csum_diff(NULL, 0, orig_ipv6_hdr, (__u32)sizeof(orig_ipv6_hdr),
					       csum_diff(NULL, 0, &inner_ipv6_hdr,
							 sizeof(inner_ipv6_hdr),
							 csum_diff(NULL, 0, &icmp,
								   sizeof(icmp), wsum))));

	if (ctx_adjust_troom(ctx, -(__s32)(len_old - len_new)) < 0)
		goto drop_err;
	if (ctx_adjust_hroom(ctx, sizeof(ip) + sizeof(icmp),
			     BPF_ADJ_ROOM_NET,
			     ctx_adjust_hroom_flags()) < 0)
		goto drop_err;

	if (eth_store_daddr(ctx, smac.addr, 0) < 0)
		goto drop_err;
	if (eth_store_saddr(ctx, dmac.addr, 0) < 0)
		goto drop_err;
	if (ctx_store_bytes(ctx, ETH_ALEN * 2, &type, sizeof(type), 0) < 0)
		goto drop_err;
	if (ctx_store_bytes(ctx, off, &ip, sizeof(ip), 0) < 0)
		goto drop_err;
	if (ctx_store_bytes(ctx, off + sizeof(ip), &icmp,
			    sizeof(icmp), 0) < 0)
		goto drop_err;
	if (ctx_store_bytes(ctx, off + sizeof(ip) + sizeof(icmp), &inner_ipv6_hdr,
			    sizeof(inner_ipv6_hdr), 0) < 0)
		goto drop_err;
	if (ctx_store_bytes(ctx, off + sizeof(ip) + sizeof(icmp) +
			    sizeof(inner_ipv6_hdr) + l4_dport_offset,
			    &dport, sizeof(dport), 0) < 0)
		goto drop_err;

	return ctx_redirect(ctx, ctx_get_ifindex(ctx), 0);
drop_err:
#endif
	return send_drop_notify_error(ctx, UNKNOWN_ID, code, METRIC_EGRESS);
}

__declare_tail(CILIUM_CALL_IPV6_NODEPORT_DSR)
int tail_nodeport_ipv6_dsr(struct __ctx_buff *ctx)
{
	struct bpf_fib_lookup_padded fib_params = {
		.l = {
			.family		= AF_INET6,
			.ifindex	= ctx_get_ifindex(ctx),
		},
	};
	int ret, oif = 0, ohead = 0;
	void *data, *data_end;
	struct ipv6hdr *ip6;
	union v6addr addr __align_stack_8 = {};
	__s8 ext_err = 0;
	__be16 port;

	if (!revalidate_data(ctx, &data, &data_end, &ip6)) {
		ret = DROP_INVALID;
		goto drop_err;
	}

	ctx_load_meta_ipv6(ctx, &addr, CB_ADDR_V6_1);

	port = (__be16)ctx_load_meta(ctx, CB_PORT);

#if DSR_ENCAP_MODE == DSR_ENCAP_IPIP
	ret = dsr_set_ipip6(ctx, ip6, &addr,
			    ctx_load_meta(ctx, CB_HINT), &ohead);
#elif DSR_ENCAP_MODE == DSR_ENCAP_NONE
	ret = dsr_set_ext6(ctx, ip6, &addr, port, &ohead);
#elif DSR_ENCAP_MODE == DSR_ENCAP_GENEVE
	ret = encap_geneve_dsr_opt6(ctx, ip6, &addr, port, &oif, &ohead);
	if (!IS_ERR(ret)) {
		if (ret == CTX_ACT_REDIRECT && oif) {
			cilium_capture_out(ctx);
			return ctx_redirect(ctx, oif, 0);
		}

		fib_params.l.family = AF_INET;
	}
#else
# error "Invalid load balancer DSR encapsulation mode!"
#endif
	if (IS_ERR(ret)) {
		if (dsr_fail_needs_reply(ret))
			return dsr_reply_icmp6(ctx, ip6, &addr, port, ret, ohead);
		goto drop_err;
	}

	if (fib_params.l.family == AF_INET) {
		struct iphdr *ip4;

		if (!revalidate_data(ctx, &data, &data_end, &ip4)) {
			ret = DROP_INVALID;
			goto drop_err;
		}

		fib_params.l.ipv4_src = ip4->saddr;
		fib_params.l.ipv4_dst = ip4->daddr;
	} else {
		if (!revalidate_data(ctx, &data, &data_end, &ip6)) {
			ret = DROP_INVALID;
			goto drop_err;
		}

		ipv6_addr_copy((union v6addr *)&fib_params.l.ipv6_src,
			       (union v6addr *)&ip6->saddr);
		ipv6_addr_copy((union v6addr *)&fib_params.l.ipv6_dst,
			       (union v6addr *)&ip6->daddr);
	}

	ret = fib_redirect(ctx, true, &fib_params, false, &ext_err, &oif);
	if (fib_ok(ret)) {
		cilium_capture_out(ctx);
		return ret;
	}
drop_err:
	return send_drop_notify_error_ext(ctx, UNKNOWN_ID, ret, ext_err,
					  METRIC_EGRESS);
}

static __always_inline int
nodeport_dsr_ingress_ipv6(struct __ctx_buff *ctx, struct ipv6_ct_tuple *tuple, fraginfo_t fraginfo,
			  int l4_off, union v6addr *addr, __be16 port, __s8 *ext_err)
{
	struct ct_state ct_state_new = {};
	__u32 monitor = 0;
	int ret;

	/* look up with SCOPE_FORWARD: */
	__ipv6_ct_tuple_reverse(tuple);

	ret = ct_lazy_lookup6(get_ct_map6(tuple), tuple, ctx, fraginfo,
			      l4_off, CT_EGRESS, SCOPE_FORWARD,
			      CT_ENTRY_DSR, NULL, &monitor);
	if (ret < 0)
		return ret;

	switch (ret) {
	case CT_NEW:
create_ct:
		if (port == 0)
			return DROP_INVALID;

		ct_state_new.src_sec_id = WORLD_IPV6_ID;
		ct_state_new.dsr_internal = 1;

		ret = ct_create6(get_ct_map6(tuple), NULL, tuple, ctx,
				 CT_EGRESS, &ct_state_new, ext_err);
		if (!IS_ERR(ret))
			ret = snat_v6_create_dsr(tuple, addr, port, ext_err);

		if (IS_ERR(ret))
			return ret;
		break;
	case CT_ESTABLISHED:
		if (tuple->nexthdr == IPPROTO_TCP && port)
			goto create_ct;
		break;
	default:
		return DROP_UNKNOWN_CT;
	}

	return CTX_ACT_OK;
}
#endif /* ENABLE_DSR */

static __always_inline struct lb6_reverse_nat *
nodeport_rev_dnat_get_info_ipv6(struct __ctx_buff *ctx,
				struct ipv6_ct_tuple *tuple)
{
	struct ipv6_nat_entry *dsr_entry __maybe_unused;
	struct ipv6_ct_tuple dsr_tuple __maybe_unused;
	__u16 rev_nat_index = 0;

	if (!ct_has_nodeport_egress_entry6(get_ct_map6(tuple), tuple,
					   &rev_nat_index, is_defined(ENABLE_DSR)))
		return NULL;

	if (rev_nat_index)
		return lb6_lookup_rev_nat_entry(ctx, rev_nat_index);

#ifdef ENABLE_DSR
	dsr_tuple = *tuple;

	dsr_tuple.flags = NAT_DIR_EGRESS;
	dsr_tuple.sport = tuple->dport;
	dsr_tuple.dport = tuple->sport;

	dsr_entry = nodeport_dsr_lookup_v6_nat_entry(&dsr_tuple);
	if (dsr_entry)
		return &dsr_entry->nat_info;
#endif

	return NULL;
}

#ifdef ENABLE_NAT_46X64_GATEWAY
__declare_tail(CILIUM_CALL_IPV46_RFC6052)
int tail_nat_ipv46(struct __ctx_buff *ctx)
{
	int ret, oif = 0, l3_off = ETH_HLEN;
	void *data, *data_end;
	struct ipv6hdr *ip6;
	struct iphdr *ip4;
	__s8 ext_err = 0;

	if (!revalidate_data(ctx, &data, &data_end, &ip4)) {
		ret = DROP_INVALID;
		goto drop_err;
	}
	if (nat46_rfc6052(ctx, ip4, l3_off)) {
		ret = DROP_NAT46;
		goto drop_err;
	}
	if (!revalidate_data(ctx, &data, &data_end, &ip6)) {
		ret = DROP_INVALID;
		goto drop_err;
	}
	ret = fib_redirect_v6(ctx, l3_off, ip6, false, true, &ext_err, &oif);
	if (fib_ok(ret)) {
		cilium_capture_out(ctx);
		return ret;
	}
drop_err:
	return send_drop_notify_error_ext(ctx, UNKNOWN_ID, ret, ext_err,
					  METRIC_EGRESS);
}

__declare_tail(CILIUM_CALL_IPV64_RFC6052)
int tail_nat_ipv64(struct __ctx_buff *ctx)
{
	int ret, oif = 0, l3_off = ETH_HLEN;
	void *data, *data_end;
	struct ipv6hdr *ip6;
	struct iphdr *ip4;
	__s8 ext_err = 0;

	if (!revalidate_data(ctx, &data, &data_end, &ip6)) {
		ret = DROP_INVALID;
		goto drop_err;
	}
	if (nat64_rfc6052(ctx, ip6)) {
		ret = DROP_NAT64;
		goto drop_err;
	}
	if (!revalidate_data(ctx, &data, &data_end, &ip4)) {
		ret = DROP_INVALID;
		goto drop_err;
	}
	ret = fib_redirect_v4(ctx, l3_off, ip4, false, true, &ext_err, &oif);
	if (fib_ok(ret)) {
		cilium_capture_out(ctx);
		return ret;
	}
drop_err:
	return send_drop_notify_error_ext(ctx, UNKNOWN_ID, ret, ext_err,
					  METRIC_EGRESS);
}
#endif /* ENABLE_NAT_46X64_GATEWAY */

static __always_inline int
nodeport_rev_dnat_ipv6(struct __ctx_buff *ctx, enum ct_dir dir,
		       struct trace_ctx *trace, __s8 *ext_err)
{
	struct bpf_fib_lookup_padded fib_params = {
		.l = {
			.family		= AF_INET6,
			.ifindex	= ctx_get_ifindex(ctx),
		},
	};
	int ret, l4_off;
	struct remote_endpoint_info *info __maybe_unused;
	struct ipv6_ct_tuple tuple __align_stack_8 = {};
	struct ct_state ct_state = {};
	void *data, *data_end;
	struct ipv6hdr *ip6;
	__u32 src_sec_identity __maybe_unused = SECLABEL;
	__be16 src_port __maybe_unused = 0;
	bool allow_neigh_map = true;
	fraginfo_t fraginfo;
	int ifindex = 0;
	__u32 monitor = 0;

	if (!revalidate_data(ctx, &data, &data_end, &ip6))
		return DROP_INVALID;

#if !defined(IS_BPF_LXC) && defined(ENABLE_NAT_46X64_GATEWAY)
	if (nat46x64_cb_route(ctx))
		goto fib_lookup;
#endif

	tuple.nexthdr = ip6->nexthdr;
	ret = ipv6_hdrlen_with_fraginfo(ctx, &tuple.nexthdr, &fraginfo);
	if (ret < 0)
		return ret;

	l4_off = ETH_HLEN + ret;

	ret = lb6_extract_tuple(ctx, ip6, fraginfo, l4_off, &tuple);
	if (ret < 0) {
		if (ret == DROP_UNSUPP_SERVICE_PROTO || ret == DROP_UNKNOWN_L4)
			goto out;
		return ret;
	}

	ret = ct_lazy_lookup6(get_ct_map6(&tuple), &tuple, ctx, fraginfo,
			      l4_off, CT_INGRESS, SCOPE_REVERSE,
			      CT_ENTRY_NODEPORT, &ct_state, &monitor);
	if (ret == CT_REPLY) {
		trace->reason = TRACE_REASON_CT_REPLY;
		trace->monitor = monitor;
		ret = ipv6_l3(ctx, ETH_HLEN, NULL, NULL, METRIC_EGRESS);
		if (unlikely(ret != CTX_ACT_OK))
			return ret;

		ret = lb6_rev_nat(ctx, l4_off, ct_state.rev_nat_index,
				  &tuple, ipfrag_has_l4_header(fraginfo),
				  dir);
		if (IS_ERR(ret))
			return ret;
		if (!revalidate_data(ctx, &data, &data_end, &ip6))
			return DROP_INVALID;
		ctx_snat_done_set(ctx);

#ifdef TUNNEL_MODE
		info = lookup_ip6_remote_endpoint((union v6addr *)&ip6->daddr, 0);
		if (info && info->flag_has_tunnel_ep && !info->flag_skip_tunnel) {
			src_sec_identity = REMOTE_NODE_ID;
			goto encap_redirect;
		}
#endif

		goto fib_lookup;
	}
out:
#if defined(ENABLE_EGRESS_GATEWAY_COMMON) && (defined(IS_BPF_XDP) || defined(IS_BPF_HOST))
	/* The gateway node needs to manually steer any reply traffic
	 * for a remote pod into the tunnel (to avoid iptables potentially
	 * dropping or accidentally SNATing the packets).
	 */
	if (egress_gw_reply_needs_redirect_hook_v6(ip6, &info)) {
		trace->reason = TRACE_REASON_CT_REPLY;
		src_sec_identity = WORLD_ID;
		goto encap_redirect;
	}
#endif /* ENABLE_EGRESS_GATEWAY_COMMON */

	return CTX_ACT_OK;

#if (defined(ENABLE_EGRESS_GATEWAY_COMMON) && (defined(IS_BPF_XDP) || defined(IS_BPF_HOST))) ||	\
    defined(TUNNEL_MODE)
encap_redirect:
	src_port = tunnel_gen_src_port_v6(&tuple);

	ret = nodeport_add_tunnel_encap(ctx, IPV4_DIRECT_ROUTING, src_port,
					info, src_sec_identity, trace->reason,
					trace->monitor, &ifindex, bpf_htons(ETH_P_IPV6));
	if (IS_ERR(ret))
		return ret;

	if (ret == CTX_ACT_REDIRECT && ifindex)
		return ctx_redirect(ctx, ifindex, 0);

	fib_params.l.ipv4_src = IPV4_DIRECT_ROUTING;
	fib_params.l.ipv4_dst = info->tunnel_endpoint.ip4;
	fib_params.l.family = AF_INET;

	/* neigh map doesn't contain DMACs for other nodes */
	allow_neigh_map = false;
	goto fib_redirect;
#endif

fib_lookup:
	if (is_v4_in_v6((union v6addr *)&ip6->saddr)) {
		struct iphdr *ip4;

		ret = lb6_to_lb4(ctx, ip6);
		if (ret < 0)
			return ret;

		if (!revalidate_data(ctx, &data, &data_end, &ip4))
			return DROP_INVALID;

		fib_params.l.ipv4_src = ip4->saddr;
		fib_params.l.ipv4_dst = ip4->daddr;
		fib_params.l.family = AF_INET;
	} else {
		ipv6_addr_copy((union v6addr *)&fib_params.l.ipv6_src,
			       (union v6addr *)&ip6->saddr);
		ipv6_addr_copy((union v6addr *)&fib_params.l.ipv6_dst,
			       (union v6addr *)&ip6->daddr);
	}

#if (defined(ENABLE_EGRESS_GATEWAY_COMMON) && (defined(IS_BPF_XDP) || defined(IS_BPF_HOST))) ||	\
    defined(TUNNEL_MODE)
fib_redirect:
#endif
	return fib_redirect(ctx, true, &fib_params, allow_neigh_map, ext_err, &ifindex);
}

static __always_inline
int __nodeport_rev_dnat_ipv6(struct __ctx_buff *ctx, enum ct_dir dir)
{
	struct trace_ctx trace = {
		.reason = TRACE_REASON_CT_REPLY,
		.monitor = TRACE_PAYLOAD_LEN,
	};
	__s8 ext_err = 0;
	int ret = 0;

	ret = nodeport_rev_dnat_ipv6(ctx, dir, &trace, &ext_err);
	if (IS_ERR(ret))
		goto drop;

	if (ret == CTX_ACT_OK) {
		if (is_defined(IS_BPF_LXC)) {
			ret = DROP_NAT_NO_MAPPING;
			goto drop;
		}

		ctx_skip_nodeport_set(ctx);
		ret = tail_call_internal(ctx, CILIUM_CALL_IPV6_FROM_NETDEV, &ext_err);
		goto drop;
	}

#ifndef IS_BPF_LXC
	edt_set_aggregate(ctx, 0);
#endif
	cilium_capture_out(ctx);
	return ret;
drop:
	return send_drop_notify_error_ext(ctx, UNKNOWN_ID, ret, ext_err,
					  METRIC_EGRESS);
}

__declare_tail(CILIUM_CALL_IPV6_NODEPORT_REVNAT_INGRESS)
static __always_inline
int tail_nodeport_rev_dnat_ingress_ipv6(struct __ctx_buff *ctx)
{
	return __nodeport_rev_dnat_ipv6(ctx, CT_INGRESS);
}

static __always_inline
int nodeport_rev_dnat_ingress_ipv6(struct __ctx_buff *ctx,
				   struct trace_ctx *trace, __s8 *ext_err)
{
	return nodeport_rev_dnat_ipv6(ctx, CT_INGRESS, trace, ext_err);
}

__declare_tail(CILIUM_CALL_IPV6_NODEPORT_REVNAT_EGRESS)
static __always_inline
int tail_nodeport_rev_dnat_egress_ipv6(struct __ctx_buff *ctx)
{
	return __nodeport_rev_dnat_ipv6(ctx, CT_EGRESS);
}

__declare_tail(CILIUM_CALL_IPV6_NODEPORT_NAT_INGRESS)
static __always_inline
int tail_nodeport_nat_ingress_ipv6(struct __ctx_buff *ctx)
{
	struct ipv6_nat_target target = {
		.min_port = NODEPORT_PORT_MIN_NAT,
		.max_port = NODEPORT_PORT_MAX_NAT,
	};
	struct trace_ctx trace = {
		.reason = TRACE_REASON_CT_REPLY,
		.monitor = TRACE_PAYLOAD_LEN,
	};
	__u32 src_id = 0;
	__s8 ext_err = 0;
	int ret;

	ret = snat_v6_rev_nat(ctx, &target, &trace, &ext_err);
	if (IS_ERR(ret)) {
		if (ret == NAT_PUNT_TO_STACK ||
		    /* DROP_NAT_NO_MAPPING is unwanted behavior in a
		     * rev-SNAT context. Let's continue to passing it
		     * up to the host and revisiting this later if
		     * needed.
		     */
		    ret == DROP_NAT_NO_MAPPING) {
			/* In case of no mapping, recircle back to
			 * main path. SNAT is very expensive in terms
			 * of instructions and
			 * complexity. Consequently, this is done
			 * inside a tail call here (because we don't
			 * have BPF to BPF calls).
			 */
			goto recircle;
		}
		goto drop_err;
	}

	ctx_snat_done_set(ctx);

#if !defined(ENABLE_DSR) || (defined(ENABLE_DSR) && defined(ENABLE_DSR_HYBRID)) ||	\
    (defined(ENABLE_EGRESS_GATEWAY_COMMON) && (defined(IS_BPF_XDP) || defined(IS_BPF_HOST)))

# if defined(ENABLE_HOST_FIREWALL) && defined(IS_BPF_HOST)
	ret = ipv6_host_policy_ingress(ctx, &src_id, &trace, &ext_err);
	if (IS_ERR(ret))
		goto drop_err;

	ctx_skip_host_fw_set(ctx);
# endif

	ret = invoke_traced_tailcall_if(__or(__and(is_defined(ENABLE_HOST_FIREWALL),
						   is_defined(IS_BPF_HOST)),
					     __and(is_defined(ENABLE_IPV6_FRAGMENTS),
						   is_defined(IS_BPF_XDP))),
					CILIUM_CALL_IPV6_NODEPORT_REVNAT_INGRESS,
					nodeport_rev_dnat_ingress_ipv6,
					&trace, &ext_err);
	if (IS_ERR(ret))
		goto drop_err;

	if (ret == CTX_ACT_OK)
		goto recircle;

	edt_set_aggregate(ctx, 0);
	cilium_capture_out(ctx);
	return ret;
#endif

recircle:
	ctx_skip_nodeport_set(ctx);
	ret = tail_call_internal(ctx, CILIUM_CALL_IPV6_FROM_NETDEV, &ext_err);

drop_err:
	return send_drop_notify_error_ext(ctx, src_id, ret, ext_err, METRIC_INGRESS);
}

__declare_tail(CILIUM_CALL_IPV6_NODEPORT_NAT_EGRESS)
static __always_inline
int tail_nodeport_nat_egress_ipv6(struct __ctx_buff *ctx)
{
	const bool nat_46x64 = nat46x64_cb_xlate(ctx);
	struct bpf_fib_lookup_padded fib_params = {
		.l = {
			.family		= AF_INET6,
			.ifindex	= ctx_get_ifindex(ctx),
		},
	};
	struct ipv6_nat_target target = {
		.min_port = NODEPORT_PORT_MIN_NAT,
		.max_port = NODEPORT_PORT_MAX_NAT,
		.addr = IPV6_DIRECT_ROUTING,
	};
	struct ipv6_ct_tuple tuple __align_stack_8 = {};
	struct trace_ctx trace = {
		.reason = (enum trace_reason)CT_NEW,
		.monitor = TRACE_PAYLOAD_LEN,
	};
	int ret, l4_off, oif = 0;
	void *data, *data_end;
	struct ipv6hdr *ip6;
	fraginfo_t fraginfo;
	__s8 ext_err = 0;
#ifdef TUNNEL_MODE
	struct remote_endpoint_info *info;
	union v6addr *dst;
#endif

	if (nat_46x64)
		build_v4_in_v6(&target.addr, IPV4_DIRECT_ROUTING);

	if (!revalidate_data(ctx, &data, &data_end, &ip6)) {
		ret = DROP_INVALID;
		goto drop_err;
	}

	tuple.nexthdr = ip6->nexthdr;
	ret = ipv6_hdrlen_with_fraginfo(ctx, &tuple.nexthdr, &fraginfo);
	if (ret < 0)
		return ret;

	l4_off = ETH_HLEN + ret;

#ifdef TUNNEL_MODE
	dst = (union v6addr *)&ip6->daddr;
	info = lookup_ip6_remote_endpoint(dst, 0);
	if (info && info->flag_has_tunnel_ep && !info->flag_skip_tunnel)
		target.addr = CONFIG(router_ipv6);
#endif

	ret = lb6_extract_tuple(ctx, ip6, fraginfo, l4_off, &tuple);
	if (IS_ERR(ret))
		goto drop_err;

	ipv6_ct_tuple_swap_ports(&tuple);
	tuple.flags = TUPLE_F_OUT;

	ret = ipv6_l3(ctx, ETH_HLEN, NULL, NULL, METRIC_EGRESS);
	if (unlikely(ret != CTX_ACT_OK))
		goto drop_err;

	ret = __snat_v6_nat(ctx, &tuple, fraginfo, l4_off, true,
			    &target, TCP_SPORT_OFF, &trace, &ext_err);
	if (IS_ERR(ret))
		goto drop_err;

	ctx_snat_done_set(ctx);

#ifdef TUNNEL_MODE
	if (info && info->flag_has_tunnel_ep && !info->flag_skip_tunnel) {
		__be16 src_port;

		src_port = tunnel_gen_src_port_v6(&tuple);

		ret = nodeport_add_tunnel_encap(ctx,
						IPV4_DIRECT_ROUTING,
						src_port,
						info,
						WORLD_IPV6_ID,
						trace.reason,
						trace.monitor,
						&oif,
						bpf_htons(ETH_P_IPV6));
		if (IS_ERR(ret))
			goto drop_err;

		if (ret == CTX_ACT_REDIRECT && oif) {
			cilium_capture_out(ctx);
			return ctx_redirect(ctx, oif, 0);
		}

		goto fib_ipv4;
	}
#endif
	if (!revalidate_data(ctx, &data, &data_end, &ip6)) {
		ret = DROP_INVALID;
		goto drop_err;
	}
	if (nat_46x64) {
		struct iphdr *ip4;

		ret = lb6_to_lb4(ctx, ip6);
		if (ret < 0)
			goto drop_err;

#ifdef TUNNEL_MODE
fib_ipv4:
#endif
		if (!revalidate_data(ctx, &data, &data_end, &ip4)) {
			ret = DROP_INVALID;
			goto drop_err;
		}
		fib_params.l.ipv4_src = ip4->saddr;
		fib_params.l.ipv4_dst = ip4->daddr;
		fib_params.l.family = AF_INET;
	} else {
		ipv6_addr_copy((union v6addr *)&fib_params.l.ipv6_src,
			       (union v6addr *)&ip6->saddr);
		ipv6_addr_copy((union v6addr *)&fib_params.l.ipv6_dst,
			       (union v6addr *)&ip6->daddr);
	}
	ret = fib_redirect(ctx, true, &fib_params, false, &ext_err, &oif);
	if (fib_ok(ret)) {
		cilium_capture_out(ctx);
		return ret;
	}
drop_err:
	return send_drop_notify_error_ext(ctx, UNKNOWN_ID, ret, ext_err,
					  METRIC_EGRESS);
}

static __always_inline int nodeport_svc_lb6(struct __ctx_buff *ctx,
					    struct ipv6_ct_tuple *tuple,
					    struct lb6_service *svc,
					    struct lb6_key *key,
					    struct ipv6hdr *ip6,
					    int l3_off,
					    fraginfo_t fraginfo,
					    int l4_off,
					    __u32 src_sec_identity __maybe_unused,
					    bool *punt_to_stack __maybe_unused,
					    __s8 *ext_err)
{
	struct ct_state ct_state_svc = {};
	bool backend_local;
	__u32 monitor = 0;
	int ret;

	if (!lb6_src_range_ok(svc, (union v6addr *)&ip6->saddr))
		return DROP_NOT_IN_SRC_RANGE;

	if (!lb6_svc_is_routable(svc))
		return DROP_IS_CLUSTER_IP;

#if defined(ENABLE_L7_LB)
	if (lb6_svc_is_l7_loadbalancer(svc)) {
# if !defined(IS_BPF_XDP)
		__be16 proxy_port = (__be16)svc->l7_lb_proxy_port;

		send_trace_notify(ctx, TRACE_TO_PROXY, src_sec_identity, UNKNOWN_ID,
				  bpf_ntohs((__u16)svc->l7_lb_proxy_port),
				  THIS_INTERFACE_IFINDEX, TRACE_REASON_POLICY, monitor,
				  bpf_htons(ETH_P_IPV6));

#  if defined(ENABLE_TPROXY)
		return ctx_redirect_to_proxy_hairpin_ipv6(ctx, proxy_port);
#  else
		cilium_dbg_capture(ctx, DBG_CAPTURE_PROXY_PRE, proxy_port);
		ctx->mark = MARK_MAGIC_TO_PROXY | (proxy_port << 16);
		cilium_dbg_capture(ctx, DBG_CAPTURE_PROXY_POST, proxy_port);

		*punt_to_stack = true;
#  endif /* ENABLE_TPROXY */
# endif /* IS_BPF_XDP */
		return CTX_ACT_OK;
	}
#endif
	ret = lb6_local(get_ct_map6(tuple), ctx, l3_off, fraginfo, l4_off,
			key, tuple, svc, &ct_state_svc,
			nodeport_xlate6(svc, tuple), ext_err, 0);
	if (IS_ERR(ret)) {
#ifdef SERVICE_NO_BACKEND_RESPONSE
		if (ret == DROP_NO_SERVICE) {
			edt_set_aggregate(ctx, 0);
			ret = tail_call_internal(ctx, CILIUM_CALL_IPV6_NO_SERVICE,
						 ext_err);
		}
#endif
		if (ret == LB_PUNT_TO_STACK) {
			*punt_to_stack = true;
			return CTX_ACT_OK;
		}
		return ret;
	}

	backend_local = __lookup_ip6_endpoint(&tuple->daddr);
	if (!backend_local && lb6_svc_is_hostport(svc))
		return DROP_INVALID;
	if (backend_local || !nodeport_uses_dsr6(svc, tuple)) {
		struct ct_state ct_state = {};

		/* lookup with SCOPE_FORWARD: */
		__ipv6_ct_tuple_reverse(tuple);

		/* only match CT entries that belong to the same service: */
		ct_state.rev_nat_index = ct_state_svc.rev_nat_index;

		ret = ct_lazy_lookup6(get_ct_map6(tuple), tuple, ctx, fraginfo,
				      l4_off, CT_EGRESS, SCOPE_FORWARD,
				      CT_ENTRY_NODEPORT, &ct_state, &monitor);
		if (ret < 0)
			return ret;

		switch (ret) {
		case CT_NEW:
			ct_state.src_sec_id = WORLD_IPV6_ID;
			ct_state.node_port = 1;

			ret = ct_create6(get_ct_map6(tuple), NULL, tuple, ctx,
					 CT_EGRESS, &ct_state, ext_err);
			if (IS_ERR(ret))
				return ret;
			break;
		case CT_ESTABLISHED:
			/* Note that we don't validate whether the matched CT entry
			 * has identical values (eg. .ifindex) as set above.
			 */
			break;
		default:
			return DROP_UNKNOWN_CT;
		}

		if (backend_local) {
			ctx_set_xfer(ctx, XFER_PKT_NO_SVC);
			return CTX_ACT_OK;
		}

		ret = neigh_record_ip6(ctx);
		if (ret < 0)
			return ret;
	}

	/* TX request to remote backend: */
	edt_set_aggregate(ctx, 0);
	if (nodeport_uses_dsr6(svc, tuple)) {
#if DSR_ENCAP_MODE == DSR_ENCAP_IPIP
		ctx_store_meta(ctx, CB_HINT,
			       ((__u32)tuple->sport << 16) | tuple->dport);
		ctx_store_meta_ipv6(ctx, CB_ADDR_V6_1, &tuple->daddr);
#elif DSR_ENCAP_MODE == DSR_ENCAP_GENEVE || DSR_ENCAP_MODE == DSR_ENCAP_NONE
		ctx_store_meta(ctx, CB_PORT, key->dport);
		ctx_store_meta_ipv6(ctx, CB_ADDR_V6_1, &key->address);
#endif /* DSR_ENCAP_MODE */
		return tail_call_internal(ctx, CILIUM_CALL_IPV6_NODEPORT_DSR, ext_err);
	} else {
		/* This code path is not only hit for NAT64, but also
		 * for NAT46. For the latter we initially hit the IPv4
		 * NodePort path, then migrate the request to IPv6 and
		 * recirculate into the regular IPv6 NodePort path. So
		 * we need to make sure to not NAT back to IPv4 for
		 * IPv4-in-IPv6 converted addresses.
		 */
		ctx_store_meta(ctx, CB_NAT_46X64,
			       !is_v4_in_v6(&key->address) &&
			       lb6_to_lb4_service(svc));
		return tail_call_internal(ctx, CILIUM_CALL_IPV6_NODEPORT_NAT_EGRESS,
					  ext_err);
	}
}

/* See nodeport_lb4(). */
static __always_inline int nodeport_lb6(struct __ctx_buff *ctx,
					struct ipv6hdr *ip6,
					__u32 src_sec_identity,
					bool *punt_to_stack,
					__s8 *ext_err,
					bool __maybe_unused *dsr)
{
	fraginfo_t fraginfo;
	bool is_svc_proto __maybe_unused = true;
	int ret, l3_off = ETH_HLEN, l4_off;
	struct ipv6_ct_tuple tuple __align_stack_8 = {};
	struct lb6_service *svc;
	struct lb6_key key = {};

	cilium_capture_in(ctx);

	tuple.nexthdr = ip6->nexthdr;
	ret = ipv6_hdrlen_with_fraginfo(ctx, &tuple.nexthdr, &fraginfo);
	if (ret < 0)
		return ret;

	l4_off = ETH_HLEN + ret;

	ret = lb6_extract_tuple(ctx, ip6, fraginfo, l4_off, &tuple);
	if (IS_ERR(ret)) {
		if (ret == DROP_UNSUPP_SERVICE_PROTO) {
			is_svc_proto = false;
			goto skip_service_lookup;
		}
		if (ret == DROP_UNKNOWN_L4) {
			ctx_set_xfer(ctx, XFER_PKT_NO_SVC);
			return CTX_ACT_OK;
		}
		return ret;
	}

	lb6_fill_key(&key, &tuple);

	svc = lb6_lookup_service(&key, false);
	if (svc) {
		return nodeport_svc_lb6(ctx, &tuple, svc, &key, ip6, l3_off,
					fraginfo, l4_off, src_sec_identity,
					punt_to_stack, ext_err);
	} else {
skip_service_lookup:
#ifdef ENABLE_NAT_46X64_GATEWAY
		if (is_v4_in_v6_rfc6052((union v6addr *)&ip6->daddr)) {
			ret = neigh_record_ip6(ctx);
			if (ret < 0)
				return ret;
			if (is_v4_in_v6_rfc6052((union v6addr *)&ip6->saddr))
				return tail_call_internal(ctx, CILIUM_CALL_IPV64_RFC6052,
							  ext_err);
			ctx_store_meta(ctx, CB_NAT_46X64, NAT46x64_MODE_XLATE);
			return tail_call_internal(ctx, CILIUM_CALL_IPV6_NODEPORT_NAT_EGRESS,
						  ext_err);
		}
#endif
		ctx_set_xfer(ctx, XFER_PKT_NO_SVC);

#ifdef ENABLE_DSR
#if (defined(IS_BPF_OVERLAY) && DSR_ENCAP_MODE == DSR_ENCAP_GENEVE) || \
    ((defined(IS_BPF_XDP) || defined(IS_BPF_HOST) || defined(IS_BPF_WIREGUARD)) && \
		DSR_ENCAP_MODE != DSR_ENCAP_GENEVE)
		if (is_svc_proto) {
			ret = nodeport_extract_dsr_v6(ctx, ip6, &tuple, l4_off,
						      &key.address,
						      &key.dport, dsr);
			if (IS_ERR(ret))
				return ret;
			if (*dsr)
				return nodeport_dsr_ingress_ipv6(ctx, &tuple, fraginfo, l4_off,
								 &key.address, key.dport,
								 ext_err);
		}
#endif
#endif /* ENABLE_DSR */

#ifndef ENABLE_MASQUERADE_IPV6
		if (!is_svc_proto)
			return CTX_ACT_OK;
#endif /* ENABLE_MASQUERADE_IPV6 */

		ctx_store_meta(ctx, CB_NAT_46X64, 0);
		ctx_store_meta(ctx, CB_SRC_LABEL, src_sec_identity);
		return tail_call_internal(ctx, CILIUM_CALL_IPV6_NODEPORT_NAT_INGRESS,
					  ext_err);
	}
}
#endif /* ENABLE_IPV6 */

#ifdef ENABLE_IPV4
static __always_inline bool nodeport_uses_dsr4(const struct lb4_service *svc,
					       const struct ipv4_ct_tuple *tuple)
{
	return nodeport_uses_dsr(svc->flags2 & SVC_FLAG_FWD_MODE_DSR,
				 tuple->nexthdr);
}

static __always_inline bool nodeport_xlate4(const struct lb4_service *svc,
					    const struct ipv4_ct_tuple *tuple)
{
	bool skip_xlate = DSR_ENCAP_MODE == DSR_ENCAP_IPIP;

	if (skip_xlate)
		skip_xlate = nodeport_uses_dsr4(svc, tuple);
	return skip_xlate;
}

#ifdef ENABLE_DSR
#if DSR_ENCAP_MODE == DSR_ENCAP_IPIP
static __always_inline __be32 rss_gen_src4(__be32 client, __be32 l4_hint)
{
	const __u32 bits = 32 - IPV4_RSS_PREFIX_BITS;
	__be32 src = IPV4_RSS_PREFIX;

	if (bits)
		src |= bpf_htonl(hash_32(client ^ l4_hint, bits));
	return src;
}

/*
 * Original packet: [clientIP:clientPort -> serviceIP:servicePort] } IP/L4
 *
 * After DSR IPIP:  [rssSrcIP -> backendIP]                        } IP
 *                  [clientIP:clientPort -> serviceIP:servicePort] } IP/L4
 */
static __always_inline int dsr_set_ipip4(struct __ctx_buff *ctx,
					 const struct iphdr *ip4,
					 __be32 backend_addr,
					 __be32 l4_hint, __be16 *ohead)
{
	__u16 tot_len = bpf_ntohs(ip4->tot_len) + sizeof(*ip4);
	const int l3_off = ETH_HLEN;
	__be32 sum;
	struct {
		__be16 tot_len;
		__be16 id;
		__be16 frag_off;
		__u8   ttl;
		__u8   protocol;
		__be32 saddr;
		__be32 daddr;
	} tp_old = {
		.tot_len	= ip4->tot_len,
		.ttl		= ip4->ttl,
		.protocol	= ip4->protocol,
		.saddr		= ip4->saddr,
		.daddr		= ip4->daddr,
	}, tp_new = {
		.tot_len	= bpf_htons(tot_len),
		.ttl		= IPDEFTTL,
		.protocol	= IPPROTO_IPIP,
		.saddr		= rss_gen_src4(ip4->saddr, l4_hint),
		.daddr		= backend_addr,
	};

	if (dsr_is_too_big(ctx, tot_len)) {
		*ohead = sizeof(*ip4);
		return DROP_FRAG_NEEDED;
	}

	if (ctx_adjust_hroom(ctx, sizeof(*ip4), BPF_ADJ_ROOM_NET,
			     ctx_adjust_hroom_flags()))
		return DROP_INVALID;
	sum = csum_diff(&tp_old, 16, &tp_new, 16, 0);
	if (ctx_store_bytes(ctx, l3_off + offsetof(struct iphdr, tot_len),
			    &tp_new.tot_len, 2, 0) < 0)
		return DROP_WRITE_ERROR;
	if (ctx_store_bytes(ctx, l3_off + offsetof(struct iphdr, ttl),
			    &tp_new.ttl, 2, 0) < 0)
		return DROP_WRITE_ERROR;
	if (ctx_store_bytes(ctx, l3_off + offsetof(struct iphdr, saddr),
			    &tp_new.saddr, 8, 0) < 0)
		return DROP_WRITE_ERROR;
	if (ipv4_csum_update_by_diff(ctx, l3_off, sum) < 0)
		return DROP_CSUM_L3;
	return 0;
}
#elif DSR_ENCAP_MODE == DSR_ENCAP_NONE
static __always_inline int dsr_set_opt4(struct __ctx_buff *ctx,
					struct iphdr *ip4, __be32 svc_addr,
					__be16 svc_port, __be16 *ohead)
{
	__u32 iph_old, iph_new;
	struct dsr_opt_v4 opt;
	__u16 tot_len = bpf_ntohs(ip4->tot_len) + sizeof(opt);
	__be32 sum;

	if (ip4->protocol == IPPROTO_TCP) {
		union tcp_flags tcp_flags = { .value = 0 };

		if (l4_load_tcp_flags(ctx, ETH_HLEN + ipv4_hdrlen(ip4), &tcp_flags) < 0)
			return DROP_CT_INVALID_HDR;

		/* Setting the option is required only for the first packet
		 * (SYN), in the case of TCP, as for further packets of the
		 * same connection a remote node will use a NAT entry to
		 * reverse xlate a reply.
		 */
		if (!(tcp_flags.value & (TCP_FLAG_SYN)))
			return 0;
	}

	if (ipv4_hdrlen(ip4) + sizeof(opt) > sizeof(struct iphdr) + MAX_IPOPTLEN)
		return DROP_CT_INVALID_HDR;

	if (dsr_is_too_big(ctx, tot_len)) {
		*ohead = sizeof(opt);
		return DROP_FRAG_NEEDED;
	}

	iph_old = *(__u32 *)ip4;
	ip4->ihl += sizeof(opt) >> 2;
	ip4->tot_len = bpf_htons(tot_len);
	iph_new = *(__u32 *)ip4;

	opt.type = DSR_IPV4_OPT_TYPE;
	opt.len = sizeof(opt);
	opt.port = bpf_htons(svc_port);
	opt.addr = bpf_htonl(svc_addr);

	sum = csum_diff(&iph_old, 4, &iph_new, 4, 0);
	sum = csum_diff(NULL, 0, &opt, sizeof(opt), sum);

	if (ctx_adjust_hroom(ctx, sizeof(opt), BPF_ADJ_ROOM_NET,
			     ctx_adjust_hroom_flags()))
		return DROP_INVALID;

	if (ctx_store_bytes(ctx, ETH_HLEN + sizeof(*ip4),
			    &opt, sizeof(opt), 0) < 0)
		return DROP_INVALID;
	if (ipv4_csum_update_by_diff(ctx, ETH_HLEN, sum) < 0)
		return DROP_CSUM_L3;

	return 0;
}
#elif DSR_ENCAP_MODE == DSR_ENCAP_GENEVE
static __always_inline int encap_geneve_dsr_opt4(struct __ctx_buff *ctx, int l3_off __maybe_unused,
						 struct iphdr *ip4, __be32 svc_addr,
						 __be16 svc_port, int *ifindex, __be16 *ohead)
{
	struct remote_endpoint_info *info __maybe_unused;
	struct geneve_dsr_opt4 gopt __align_stack_8;
	bool need_opt = true;
	__u16 encap_len = sizeof(struct iphdr) + sizeof(struct udphdr) +
		sizeof(struct genevehdr) + ETH_HLEN;
	__u16 total_len = bpf_ntohs(ip4->tot_len);
	__u32 src_sec_identity = WORLD_IPV4_ID;
	__be32 tunnel_endpoint __maybe_unused;
	__be16 src_port = 0;
#if __ctx_is == __ctx_xdp
	bool has_encap = l3_off > ETH_HLEN;
	struct iphdr *outer_ip4 = ip4;
	void *data, *data_end;

	build_bug_on((sizeof(gopt) % 4) != 0);

	if (has_encap) {
		/* point at the inner IPv4 header */
		if (!revalidate_data_l3_off(ctx, &data, &data_end, &ip4, encap_len + ETH_HLEN))
			return DROP_INVALID;

		encap_len = 0;
	} else {
		struct ipv4_ct_tuple tuple = {};
		fraginfo_t fraginfo;
		int l4_off, ret;

		fraginfo = ipfrag_encode_ipv4(ip4);
		l4_off = ETH_HLEN + ipv4_hdrlen(ip4);

		ret = lb4_extract_tuple(ctx, ip4, fraginfo, l4_off, &tuple);
		if (IS_ERR(ret))
			return ret;

		src_port = tunnel_gen_src_port_v4(&tuple);
	}
#endif

	info = lookup_ip4_remote_endpoint(ip4->daddr, 0);
	if (!info || !info->flag_has_tunnel_ep)
		return DROP_NO_TUNNEL_ENDPOINT;

	tunnel_endpoint = info->tunnel_endpoint.ip4;

	if (ip4->protocol == IPPROTO_TCP) {
		union tcp_flags tcp_flags = { .value = 0 };

		if (l4_load_tcp_flags(ctx, l3_off + ipv4_hdrlen(ip4), &tcp_flags) < 0)
			return DROP_CT_INVALID_HDR;

		/* The GENEVE option is required only for the first packet
		 * (SYN), in the case of TCP, as for further packets of the
		 * same connection a remote node will use a NAT entry to
		 * reverse xlate a reply.
		 */
		if (!(tcp_flags.value & (TCP_FLAG_SYN)))
			need_opt = false;
	}

	if (need_opt) {
		encap_len += sizeof(struct geneve_dsr_opt4);
		set_geneve_dsr_opt4(svc_port, svc_addr, &gopt);
	}

	if (dsr_is_too_big(ctx, total_len + encap_len)) {
		*ohead = encap_len;
		return DROP_FRAG_NEEDED;
	}

#if __ctx_is == __ctx_xdp
	if (has_encap) {
		int outer_l4_off = ETH_HLEN + ipv4_hdrlen(outer_ip4);
		__be32 lb_ip = IPV4_DIRECT_ROUTING;
		__wsum sum = 0;

		/* update outer_ip4 daddr and saddr: */
		sum = csum_diff(&outer_ip4->daddr, 4, &tunnel_endpoint, 4, 0);
		if (ctx_store_bytes(ctx, ETH_HLEN + offsetof(struct iphdr, daddr),
				    &tunnel_endpoint, 4, 0) < 0)
			return DROP_WRITE_ERROR;

		sum = csum_diff(&outer_ip4->saddr, 4, &lb_ip, 4, sum);
		if (ctx_store_bytes(ctx, ETH_HLEN + offsetof(struct iphdr, saddr),
				    &lb_ip, 4, 0) < 0)
			return DROP_WRITE_ERROR;

		/* adjust outer_ip4->csum: */
		if (ipv4_csum_update_by_diff(ctx, ETH_HLEN, sum) < 0)
			return DROP_CSUM_L3;

		/* insert the GENEVE-DSR option: */
		if (need_opt) {
			__be16 new_length;
			int ret;

			/* update udp->len */
			if (ctx_load_bytes(ctx, outer_l4_off + offsetof(struct udphdr, len),
					   &new_length, sizeof(new_length)) < 0)
				return DROP_INVALID;

			new_length = bpf_htons(bpf_ntohs(new_length) + sizeof(gopt));

			if (ctx_store_bytes(ctx, outer_l4_off + offsetof(struct udphdr, len),
					    &new_length, sizeof(new_length), 0) < 0)
				return DROP_WRITE_ERROR;

			/* update outer_ip4->tot_len */
			new_length = bpf_htons(total_len + sizeof(gopt));

			if (ipv4_csum_update_by_value(ctx, ETH_HLEN, outer_ip4->tot_len,
						      new_length, sizeof(new_length)) < 0)
				return DROP_CSUM_L3;

			if (ctx_store_bytes(ctx, ETH_HLEN + offsetof(struct iphdr, tot_len),
					    &new_length, sizeof(new_length), 0) < 0)
				return DROP_WRITE_ERROR;

			ret = ctx_set_tunnel_opt(ctx, (__u8 *)&gopt, sizeof(gopt));
			if (ret)
				return ret;
		}

		return CTX_ACT_REDIRECT;
	}
#endif

	if (need_opt)
		return nodeport_add_tunnel_encap_opt(ctx,
						     IPV4_DIRECT_ROUTING,
						     src_port,
						     info,
						     src_sec_identity,
						     &gopt,
						     sizeof(gopt),
						     (enum trace_reason)CT_NEW,
						     TRACE_PAYLOAD_LEN,
						     ifindex,
						     bpf_htons(ETH_P_IP));

	return nodeport_add_tunnel_encap(ctx,
					 IPV4_DIRECT_ROUTING,
					 src_port,
					 info,
					 src_sec_identity,
					 (enum trace_reason)CT_NEW,
					 TRACE_PAYLOAD_LEN,
					 ifindex,
					 bpf_htons(ETH_P_IP));
}
#endif /* DSR_ENCAP_MODE */

static __always_inline int
nodeport_extract_dsr_v4(struct __ctx_buff *ctx,
			const struct iphdr *ip4 __maybe_unused,
			const struct ipv4_ct_tuple *tuple, int l4_off,
			__be32 *addr, __be16 *port, bool *dsr)
{
	struct ipv4_ct_tuple tmp = *tuple;

	/* Parse DSR info from the packet, to get the addr/port of the
	 * addressed service. We need this for RevDNATing the backend's replies.
	 *
	 * TCP connections have the DSR Option only in their SYN packet.
	 * To identify that a non-SYN packet belongs to a DSR connection,
	 * we need to check whether a corresponding CT entry with .dsr flag exists.
	 */
	if (tuple->nexthdr == IPPROTO_TCP) {
		union tcp_flags tcp_flags = {};

		if (l4_load_tcp_flags(ctx, l4_off, &tcp_flags) < 0)
			return DROP_CT_INVALID_HDR;

		ipv4_ct_tuple_reverse(&tmp);

		if (!(tcp_flags.value & TCP_FLAG_SYN)) {
			/* If the packet belongs to a tracked DSR connection,
			 * trigger a CT update.
			 * We don't have any DSR info to report back, and that's ok.
			 */
			*dsr = ct_has_dsr_egress_entry4(get_ct_map4(&tmp), &tmp);
			*port = 0;
			return 0;
		}
	}

#if defined(IS_BPF_OVERLAY)
	{
		struct geneve_dsr_opt4 gopt;
		int ret = 0;

		ret = ctx_get_tunnel_opt(ctx, &gopt, sizeof(gopt));

		if (ret > 0) {
			if (gopt.hdr.opt_class == bpf_htons(DSR_GENEVE_OPT_CLASS) &&
			    gopt.hdr.type == DSR_GENEVE_OPT_TYPE) {
				*dsr = true;
				*port = gopt.port;
				*addr = gopt.addr;
				return 0;
			}
		}
	}
#else
	/* Check whether IPv4 header contains a 64-bit option (IPv4 header
	 * w/o option (5 x 32-bit words) + the DSR option (2 x 32-bit words)).
	 */
	if (ip4->ihl >= 0x7) {
		struct dsr_opt_v4 opt;

		if (ctx_load_bytes(ctx, ETH_HLEN + sizeof(struct iphdr),
				   &opt, sizeof(opt)) < 0)
			return DROP_INVALID;

		if (opt.type == DSR_IPV4_OPT_TYPE && opt.len == sizeof(opt)) {
			*dsr = true;
			*addr = bpf_ntohl(opt.addr);
			*port = bpf_ntohs(opt.port);
			return 0;
		}
	}
#endif

	/* SYN for a new connection that's not / no longer DSR.
	 * If it's reopened, avoid sending subsequent traffic down the DSR path.
	 */
	if (tuple->nexthdr == IPPROTO_TCP)
		ct_update_dsr(get_ct_map4(&tmp), &tmp, false);

	return 0;
}

static __always_inline struct ipv4_nat_entry *
nodeport_dsr_lookup_v4_nat_entry(const struct ipv4_ct_tuple *nat_tuple)
{
	return snat_v4_lookup(nat_tuple);
}

static __always_inline int dsr_reply_icmp4(struct __ctx_buff *ctx,
					   struct iphdr *ip4 __maybe_unused,
					   __be32 svc_addr __maybe_unused,
					   __be16 dport __maybe_unused,
					   int code, __be16 ohead __maybe_unused)
{
#ifdef ENABLE_DSR_ICMP_ERRORS
	const __s32 orig_dgram = 8, off = ETH_HLEN;
	const __u32 l3_max = MAX_IPOPTLEN + sizeof(*ip4) + orig_dgram;
	__be16 type = bpf_htons(ETH_P_IP);
	__s32 len_new = off + ipv4_hdrlen(ip4) + orig_dgram;
	__s32 len_old = (__s32)ctx_full_len(ctx);
	__u8 reason = (__u8)-code;
	__u8 tmp[l3_max];
	union macaddr smac, dmac;
	struct icmphdr icmp __align_stack_8 = {
		.type		= ICMP_DEST_UNREACH,
		.code		= ICMP_FRAG_NEEDED,
		.un = {
			.frag = {
				.mtu = bpf_htons(THIS_MTU - ohead),
			},
		},
	};
	__u64 tot_len = sizeof(struct iphdr) + ipv4_hdrlen(ip4) + sizeof(icmp) + orig_dgram;
	struct iphdr ip __align_stack_8 = {
		.ihl		= sizeof(ip) >> 2,
		.version	= IPVERSION,
		.ttl		= IPDEFTTL,
		.tos		= ip4->tos,
		.id		= ip4->id,
		.protocol	= IPPROTO_ICMP,
		.saddr		= ip4->daddr,
		.daddr		= ip4->saddr,
		.frag_off	= bpf_htons(IP_DF),
		.tot_len	= bpf_htons((__u16)tot_len),
	};

	struct iphdr inner_ip_hdr __align_stack_8 = *ip4;
	__s32 l4_dport_offset;

	/* DSR changes the destination address from service ip to pod ip and
	 * destination port from service port to pod port. While resppnding
	 * back with ICMP error, it is necessary to set it to original ip and
	 * port.
	 * We do recompute the whole checksum here. Another way would be to
	 * unfold checksum and then do the math adding the diff.
	 */
	inner_ip_hdr.daddr = svc_addr;
	inner_ip_hdr.check = 0;
	inner_ip_hdr.check = csum_fold(csum_diff(NULL, 0, &inner_ip_hdr,
						 sizeof(inner_ip_hdr), 0));

	if (inner_ip_hdr.protocol == IPPROTO_UDP)
		l4_dport_offset = UDP_DPORT_OFF;
	else if (inner_ip_hdr.protocol == IPPROTO_TCP)
		l4_dport_offset = TCP_DPORT_OFF;

	update_metrics(ctx_full_len(ctx), METRIC_EGRESS, reason);

	if (eth_load_saddr(ctx, smac.addr, 0) < 0)
		goto drop_err;
	if (eth_load_daddr(ctx, dmac.addr, 0) < 0)
		goto drop_err;

	ip.check = csum_fold(csum_diff(NULL, 0, &ip, sizeof(ip), 0));

	/* We use a workaround here in that we push zero-bytes into the
	 * payload in order to support dynamic IPv4 header size. This
	 * works given one's complement sum does not change.
	 */
	memset(tmp, 0, MAX_IPOPTLEN);
	if (ctx_store_bytes(ctx, len_new, tmp, MAX_IPOPTLEN, 0) < 0)
		goto drop_err;
	if (ctx_load_bytes(ctx, off, tmp, (__u32)sizeof(tmp)) < 0)
		goto drop_err;

	memcpy(tmp, &inner_ip_hdr, sizeof(inner_ip_hdr));
	memcpy(tmp + sizeof(inner_ip_hdr) + l4_dport_offset, &dport, sizeof(dport));

	icmp.checksum = csum_fold(csum_diff(NULL, 0, tmp, (__u32)sizeof(tmp),
					    csum_diff(NULL, 0, &icmp,
						      sizeof(icmp), 0)));

	if (ctx_adjust_troom(ctx, -(len_old - len_new)) < 0)
		goto drop_err;
	if (ctx_adjust_hroom(ctx, sizeof(ip) + sizeof(icmp),
			     BPF_ADJ_ROOM_NET,
			     ctx_adjust_hroom_flags()) < 0)
		goto drop_err;

	if (eth_store_daddr(ctx, smac.addr, 0) < 0)
		goto drop_err;
	if (eth_store_saddr(ctx, dmac.addr, 0) < 0)
		goto drop_err;
	if (ctx_store_bytes(ctx, ETH_ALEN * 2, &type, sizeof(type), 0) < 0)
		goto drop_err;
	if (ctx_store_bytes(ctx, off, &ip, sizeof(ip), 0) < 0)
		goto drop_err;
	if (ctx_store_bytes(ctx, off + sizeof(ip), &icmp,
			    sizeof(icmp), 0) < 0)
		goto drop_err;
	if (ctx_store_bytes(ctx, off + sizeof(ip) + sizeof(icmp),
			    &inner_ip_hdr, sizeof(inner_ip_hdr), 0) < 0)
		goto drop_err;
	if (ctx_store_bytes(ctx, off + sizeof(ip) + sizeof(icmp)
			    + sizeof(inner_ip_hdr) + l4_dport_offset,
			    &dport, sizeof(dport), 0) < 0)
		goto drop_err;

	return ctx_redirect(ctx, ctx_get_ifindex(ctx), 0);
drop_err:
#endif
	return send_drop_notify_error(ctx, UNKNOWN_ID, code, METRIC_EGRESS);
}

__declare_tail(CILIUM_CALL_IPV4_NODEPORT_DSR)
int tail_nodeport_ipv4_dsr(struct __ctx_buff *ctx)
{
	void *data, *data_end;
	struct iphdr *ip4;
	int ret, oif = 0;
	__be16 ohead = 0;
	__s8 ext_err = 0;
	__be32 addr;
	__be16 port;

	if (!revalidate_data(ctx, &data, &data_end, &ip4)) {
		ret = DROP_INVALID;
		goto drop_err;
	}
	addr = ctx_load_meta(ctx, CB_ADDR_V4);
	port = (__be16)ctx_load_meta(ctx, CB_PORT);

#if DSR_ENCAP_MODE == DSR_ENCAP_IPIP
	ret = dsr_set_ipip4(ctx, ip4,
			    addr,
			    ctx_load_meta(ctx, CB_HINT), &ohead);
#elif DSR_ENCAP_MODE == DSR_ENCAP_NONE
	ret = dsr_set_opt4(ctx, ip4,
			   addr,
			   port, &ohead);
#elif DSR_ENCAP_MODE == DSR_ENCAP_GENEVE
	ret = encap_geneve_dsr_opt4(ctx, ctx_load_meta(ctx, CB_DSR_L3_OFF),
				    ip4, addr, port, &oif, &ohead);
	if (!IS_ERR(ret)) {
		if (ret == CTX_ACT_REDIRECT && oif) {
			cilium_capture_out(ctx);
			return ctx_redirect(ctx, oif, 0);
		}
	}
#else
# error "Invalid load balancer DSR encapsulation mode!"
#endif
	if (IS_ERR(ret)) {
		if (dsr_fail_needs_reply(ret))
			return dsr_reply_icmp4(ctx, ip4, addr, port, ret, ohead);
		goto drop_err;
	}
	if (!revalidate_data(ctx, &data, &data_end, &ip4)) {
		ret = DROP_INVALID;
		goto drop_err;
	}
	ret = fib_redirect_v4(ctx, ETH_HLEN, ip4, true, false, &ext_err, &oif);
	if (fib_ok(ret)) {
		cilium_capture_out(ctx);
		return ret;
	}
drop_err:
	return send_drop_notify_error_ext(ctx, UNKNOWN_ID, ret, ext_err,
					  METRIC_EGRESS);
}

static __always_inline int
nodeport_dsr_ingress_ipv4(struct __ctx_buff *ctx, struct ipv4_ct_tuple *tuple, fraginfo_t fraginfo,
			  int l4_off, __be32 addr, __be16 port, __s8 *ext_err)
{
	struct ct_state ct_state_new = {};
	__u32 monitor = 0;
	int ret;

	/* lookup with SCOPE_FORWARD: */
	__ipv4_ct_tuple_reverse(tuple);

	ret = ct_lazy_lookup4(get_ct_map4(tuple), tuple, ctx, fraginfo,
			      l4_off, CT_EGRESS, SCOPE_FORWARD,
			      CT_ENTRY_DSR, NULL, &monitor);
	if (ret < 0)
		return ret;

	switch (ret) {
	case CT_NEW:
create_ct:
		if (port == 0)
			/* Not expected at all - nodeport_extract_dsr_v4() said
			 * there would be a CT entry! Without DSR info we can't
			 * do anything smart here.
			 */
			return DROP_INVALID;

		ct_state_new.src_sec_id = WORLD_IPV4_ID;
		ct_state_new.dsr_internal = 1;

		ret = ct_create4(get_ct_map4(tuple), NULL, tuple, ctx,
				 CT_EGRESS, &ct_state_new, ext_err);
		if (!IS_ERR(ret))
			ret = snat_v4_create_dsr(tuple, addr, port, ext_err);

		if (IS_ERR(ret))
			return ret;
		break;
	case CT_ESTABLISHED:
		/* For TCP we only expect DSR info on the SYN, so CT_ESTABLISHED
		 * is unexpected and we need to refresh the CT entry.
		 *
		 * Otherwise we tolerate DSR info on an established connection.
		 * TODO: how do we know if we need to refresh the SNAT entry?
		 */
		if (tuple->nexthdr == IPPROTO_TCP && port)
			goto create_ct;
		break;
	default:
		return DROP_UNKNOWN_CT;
	}

	return CTX_ACT_OK;
}
#endif /* ENABLE_DSR */

static __always_inline struct lb4_reverse_nat *
nodeport_rev_dnat_get_info_ipv4(struct __ctx_buff *ctx,
				struct ipv4_ct_tuple *tuple)
{
	struct ipv4_nat_entry *dsr_entry __maybe_unused;
	struct ipv4_ct_tuple dsr_tuple __maybe_unused;
	__u16 rev_nat_index = 0;

	if (!ct_has_nodeport_egress_entry4(get_ct_map4(tuple), tuple,
					   &rev_nat_index, is_defined(ENABLE_DSR)))
		return NULL;

	if (rev_nat_index)
		return lb4_lookup_rev_nat_entry(ctx, rev_nat_index);

#ifdef ENABLE_DSR
	dsr_tuple = *tuple;

	dsr_tuple.flags = NAT_DIR_EGRESS;
	dsr_tuple.sport = tuple->dport;
	dsr_tuple.dport = tuple->sport;

	dsr_entry = nodeport_dsr_lookup_v4_nat_entry(&dsr_tuple);
	if (dsr_entry)
		return &dsr_entry->nat_info;
#endif

	return NULL;
}

/* Reverse NAT handling of node-port traffic for the case where the
 * backend i) was a local EP and bpf_lxc redirected to us, ii) was
 * a remote backend and we got here after reverse SNAT from the
 * tail_nodeport_nat_ingress_ipv4().
 *
 * Also, reverse NAT handling return path egress-gw traffic.
 *
 * CILIUM_CALL_IPV{4,6}_NODEPORT_REVNAT is plugged into CILIUM_MAP_CALLS
 * of the bpf_host, bpf_overlay and of the bpf_lxc.
 */
static __always_inline int
nodeport_rev_dnat_ipv4(struct __ctx_buff *ctx, struct trace_ctx *trace,
		       __s8 *ext_err)
{
	struct bpf_fib_lookup_padded fib_params = {
		.l = {
			.family		= AF_INET,
			.ifindex	= ctx_get_ifindex(ctx),
		},
	};
	struct remote_endpoint_info *info __maybe_unused = NULL;
	int ifindex = 0, ret, l3_off = ETH_HLEN, l4_off;
	struct ipv4_ct_tuple tuple = {};
	struct ct_state ct_state = {};
	void *data, *data_end;
	struct iphdr *ip4;
	__u32 tunnel_endpoint __maybe_unused = 0;
	__u32 dst_sec_identity __maybe_unused = 0;
	__u32 src_sec_identity __maybe_unused = SECLABEL;
	bool allow_neigh_map = true;
	fraginfo_t fraginfo;
	__u32 *vrf_id __maybe_unused = NULL;
	__u32 monitor = 0;

	if (!revalidate_data(ctx, &data, &data_end, &ip4))
		return DROP_INVALID;

	fraginfo = ipfrag_encode_ipv4(ip4);
	l4_off = ETH_HLEN + ipv4_hdrlen(ip4);

	ret = lb4_extract_tuple(ctx, ip4, fraginfo, l4_off, &tuple);
	if (ret < 0) {
		/* If it's not a SVC protocol, we don't need to check for RevDNAT: */
		if (ret == DROP_UNSUPP_SERVICE_PROTO || ret == DROP_UNKNOWN_L4)
			goto skip_revdnat;

		return ret;
	}

#if defined(ENABLE_SRV6) && defined(IS_BPF_LXC)
	/* Determine if packet belongs to a VRF before we do NAT.
	 * This is needed because we determine the VRF membership
	 * based on the source address of the packet. This should
	 * be fixed in the future by determining the VRF membership
	 * based on the IP address-agnostic way such as ingress
	 * interface index.
	 */
	vrf_id = srv6_lookup_vrf4(ip4->saddr, ip4->daddr);
#endif

	ret = nodeport_rev_dnat_ipv4_hook(ctx, ip4, &tuple, &tunnel_endpoint,
					  &src_sec_identity, &dst_sec_identity);
	if (ret == CTX_ACT_OK)
		return ret;
	else if (ret == CTX_ACT_REDIRECT)
		goto redirect;

	ret = ct_lazy_lookup4(get_ct_map4(&tuple), &tuple, ctx, fraginfo,
			      l4_off, CT_INGRESS, SCOPE_REVERSE,
			      CT_ENTRY_NODEPORT, &ct_state, &monitor);
	if (ret == CT_REPLY) {
		trace->reason = TRACE_REASON_CT_REPLY;
		trace->monitor = monitor;
		ret = lb4_rev_nat(ctx, l3_off, l4_off, ct_state.rev_nat_index, false,
				  &tuple, ipfrag_has_l4_header(fraginfo));
		if (IS_ERR(ret))
			return ret;
		if (!revalidate_data(ctx, &data, &data_end, &ip4))
			return DROP_INVALID;
		ctx_snat_done_set(ctx);

#if defined(TUNNEL_MODE)
		info = lookup_ip4_remote_endpoint(ip4->daddr, 0);
		if (info && info->flag_has_tunnel_ep && !info->flag_skip_tunnel) {
			tunnel_endpoint = info->tunnel_endpoint.ip4;
			src_sec_identity = REMOTE_NODE_ID;
			dst_sec_identity = info->sec_identity;
		}
#endif

		goto redirect;
	}

skip_revdnat:
#if defined(ENABLE_EGRESS_GATEWAY_COMMON) && \
	(defined(IS_BPF_XDP) || defined(IS_BPF_HOST) || defined(IS_BPF_WIREGUARD))
	/* The gateway node needs to manually steer any reply traffic
	 * for a remote pod into the tunnel (to avoid iptables potentially
	 * dropping or accidentally SNATing the packets).
	 */
	if (egress_gw_reply_needs_redirect_hook(ip4, &tunnel_endpoint, &dst_sec_identity)) {
		trace->reason = TRACE_REASON_CT_REPLY;
		src_sec_identity = WORLD_ID;
		goto redirect;
	}
#endif /* ENABLE_EGRESS_GATEWAY_COMMON */

	return CTX_ACT_OK;

redirect:
#if defined(ENABLE_SRV6) && defined(IS_BPF_LXC)
	if (vrf_id) {
		union v6addr *sid;
		/* Do policy lookup if it belongs to a VRF */
		sid = srv6_lookup_policy4(*vrf_id, ip4->daddr);
		if (sid) {
			/* If there's a policy, tailcall to the H.Encaps logic */
			srv6_store_meta_sid(ctx, sid);
			return tail_call_internal(ctx, CILIUM_CALL_SRV6_ENCAP,
						  ext_err);
		}
	}
#endif /* ENABLE_SRV6 */

	fib_params.l.ipv4_src = ip4->saddr;
	fib_params.l.ipv4_dst = ip4->daddr;

	ret = ipv4_l3(ctx, l3_off, NULL, NULL, ip4);
	if (unlikely(ret != CTX_ACT_OK))
		return ret;

#if (defined(ENABLE_EGRESS_GATEWAY_COMMON) && \
		(defined(IS_BPF_XDP) || defined(IS_BPF_HOST) || defined(IS_BPF_WIREGUARD))) ||	\
    defined(TUNNEL_MODE)
	if (tunnel_endpoint) {
		__be16 src_port = tunnel_gen_src_port_v4(&tuple);
		struct remote_endpoint_info fake_info = {0};

		/* Needed because info might be null while tunnel_endpoint isn't. */
		fake_info.tunnel_endpoint.ip4 = tunnel_endpoint;
		fake_info.flag_has_tunnel_ep = true;
		fake_info.sec_identity = dst_sec_identity;
		ret = nodeport_add_tunnel_encap(ctx, IPV4_DIRECT_ROUTING, src_port,
						&fake_info, src_sec_identity,
						trace->reason, trace->monitor, &ifindex,
						bpf_htons(ETH_P_IP));
		if (IS_ERR(ret))
			return ret;

		if (ret == CTX_ACT_REDIRECT && ifindex)
			return ctx_redirect(ctx, ifindex, 0);

		fib_params.l.ipv4_src = IPV4_DIRECT_ROUTING;
		fib_params.l.ipv4_dst = tunnel_endpoint;

		/* neigh map doesn't contain DMACs for other nodes */
		allow_neigh_map = false;
	}
#endif

	return fib_redirect(ctx, true, &fib_params, allow_neigh_map, ext_err, &ifindex);
}

__declare_tail(CILIUM_CALL_IPV4_NODEPORT_REVNAT)
static __always_inline
int tail_nodeport_rev_dnat_ipv4(struct __ctx_buff *ctx)
{
	struct trace_ctx trace = {
		.reason = TRACE_REASON_UNKNOWN,
		.monitor = TRACE_PAYLOAD_LEN,
	};
	__s8 ext_err = 0;
	int ret = 0;

	ret = nodeport_rev_dnat_ipv4(ctx, &trace, &ext_err);
	if (IS_ERR(ret))
		goto drop_err;

	if (ret == CTX_ACT_OK) {
		/* When called by bpf_lxc to handle a reply by local backend,
		 * the packet *must* be redirected.
		 */
		if (is_defined(IS_BPF_LXC)) {
			ret = DROP_NAT_NO_MAPPING;
			goto drop_err;
		}

		ctx_skip_nodeport_set(ctx);
		ret = tail_call_internal(ctx, CILIUM_CALL_IPV4_FROM_NETDEV, &ext_err);
		goto drop_err;
	}

#ifndef IS_BPF_LXC
	edt_set_aggregate(ctx, 0);
#endif
	cilium_capture_out(ctx);
	return ret;

drop_err:
	return send_drop_notify_error_ext(ctx, UNKNOWN_ID, ret, ext_err,
					  METRIC_EGRESS);
}

__declare_tail(CILIUM_CALL_IPV4_NODEPORT_NAT_INGRESS)
static __always_inline
int tail_nodeport_nat_ingress_ipv4(struct __ctx_buff *ctx)
{
	struct ipv4_nat_target target = {
		.min_port = NODEPORT_PORT_MIN_NAT,
		.max_port = NODEPORT_PORT_MAX_NAT,
	};
	struct trace_ctx trace = {
		.reason = TRACE_REASON_UNKNOWN,
		.monitor = TRACE_PAYLOAD_LEN,
	};
	__u32 src_id = 0;
	__s8 ext_err = 0;
	int ret;

	ret = snat_v4_rev_nat(ctx, &target, &trace, &ext_err);
	if (IS_ERR(ret)) {
		if (ret == NAT_PUNT_TO_STACK ||
		    /* DROP_NAT_NO_MAPPING is unwanted behavior in a
		     * rev-SNAT context. Let's continue to passing it up
		     * to the host and revisiting this later if
		     * needed.
		     */
		    ret == DROP_NAT_NO_MAPPING) {
			/* In case of no mapping, recircle back to
			 * main path. SNAT is very expensive in terms
			 * of instructions and
			 * complexity. Consequently, this is done
			 * inside a tail call here (because we don't
			 * have BPF to BPF calls).
			 */
			goto recircle;
		}
		goto drop_err;
	}

	ctx_snat_done_set(ctx);

	/* At this point we know that a reverse SNAT mapping exists.
	 * Otherwise, we would have tail-called back to
	 * CALL_IPV4_FROM_NETDEV in the code above.
	 */
#if !defined(ENABLE_DSR) || (defined(ENABLE_DSR) && defined(ENABLE_DSR_HYBRID)) ||	\
    (defined(ENABLE_EGRESS_GATEWAY_COMMON) && \
		(defined(IS_BPF_XDP) || defined(IS_BPF_HOST) || defined(IS_BPF_WIREGUARD)))

# if defined(ENABLE_HOST_FIREWALL) && defined(IS_BPF_HOST)
	ret = ipv4_host_policy_ingress(ctx, &src_id, &trace, &ext_err);
	if (IS_ERR(ret))
		goto drop_err;

	/* We don't want to enforce host policies a second time,
	 * on recircle / after RevDNAT.
	 */
	ctx_skip_host_fw_set(ctx);
# endif

	/* If we're not in full DSR mode, reply traffic from remote backends
	 * might pass back through the LB node and requires revDNAT.
	 *
	 * Also let nodeport_rev_dnat_ipv4() redirect EgressGW
	 * reply traffic into tunnel (see there for details).
	 */
	ret = invoke_traced_tailcall_if(__and(is_defined(ENABLE_HOST_FIREWALL),
					      is_defined(IS_BPF_HOST)),
					CILIUM_CALL_IPV4_NODEPORT_REVNAT,
					nodeport_rev_dnat_ipv4,
					&trace, &ext_err);
	if (IS_ERR(ret))
		goto drop_err;

	/* No redirect needed: */
	if (ret == CTX_ACT_OK)
		goto recircle;

	/* Redirected to egress interface: */
	edt_set_aggregate(ctx, 0);
	cilium_capture_out(ctx);
	return ret;
#endif

recircle:
	ctx_skip_nodeport_set(ctx);
	ret = tail_call_internal(ctx, CILIUM_CALL_IPV4_FROM_NETDEV, &ext_err);

drop_err:
	return send_drop_notify_error_ext(ctx, src_id, ret, ext_err,
					  METRIC_INGRESS);
}

__declare_tail(CILIUM_CALL_IPV4_NODEPORT_NAT_EGRESS)
static __always_inline
int tail_nodeport_nat_egress_ipv4(struct __ctx_buff *ctx)
{
	struct bpf_fib_lookup_padded fib_params = {
		.l = {
			.family		= AF_INET,
			.ifindex	= ctx_get_ifindex(ctx),
		},
	};
	struct ipv4_nat_target target = {
		.min_port = NODEPORT_PORT_MIN_NAT,
		.max_port = NODEPORT_PORT_MAX_NAT,
		/* Unfortunately, the bpf_fib_lookup() is not able to set src IP addr.
		 * So we need to assume that the direct routing device is going to be
		 * used to fwd the NodePort request, thus SNAT-ing to its IP addr.
		 * This will change once we have resolved GH#17158.
		 */
		.addr = IPV4_DIRECT_ROUTING,
	};
	struct ipv4_ct_tuple tuple = {};
	struct trace_ctx trace = {
		.reason = (enum trace_reason)CT_NEW,
		.monitor = TRACE_PAYLOAD_LEN,
	};
	int ret, l4_off, oif = 0;
	void *data, *data_end;
	struct iphdr *ip4;
	fraginfo_t fraginfo;
	__s8 ext_err = 0;
	__u32 dst_sec_identity __maybe_unused = 0;
#ifdef TUNNEL_MODE
	__u32 src_sec_identity = ctx_load_meta(ctx, CB_SRC_LABEL);
	__u8 cluster_id __maybe_unused = (__u8)ctx_load_meta(ctx, CB_CLUSTER_ID_EGRESS);
	struct remote_endpoint_info *info;
	__be32 tunnel_endpoint = 0;
#endif

	if (!revalidate_data(ctx, &data, &data_end, &ip4)) {
		ret = DROP_INVALID;
		goto drop_err;
	}

	fraginfo = ipfrag_encode_ipv4(ip4);
	l4_off = ETH_HLEN + ipv4_hdrlen(ip4);

#ifdef TUNNEL_MODE
	info = lookup_ip4_remote_endpoint(ip4->daddr, cluster_id);
	if (info && info->flag_has_tunnel_ep && !info->flag_skip_tunnel) {
		tunnel_endpoint = info->tunnel_endpoint.ip4;
		dst_sec_identity = info->sec_identity;

		target.addr = IPV4_GATEWAY;
#if defined(ENABLE_CLUSTER_AWARE_ADDRESSING) && defined(ENABLE_INTER_CLUSTER_SNAT)
		if (cluster_id && cluster_id != CLUSTER_ID)
			target.addr = IPV4_INTER_CLUSTER_SNAT;
#endif
	}
#endif

	ret = lb4_extract_tuple(ctx, ip4, fraginfo, l4_off, &tuple);
	if (IS_ERR(ret))
		goto drop_err;

	/* Extracted ports are in flipped order, but SNAT wants them to
	 * match the packet header:
	 */
	ipv4_ct_tuple_swap_ports(&tuple);
	tuple.flags = TUPLE_F_OUT;

	ret = nodeport_nat_egress_ipv4_hook(ctx, ip4, dst_sec_identity, &tuple, l4_off, &ext_err);
	if (ret != CTX_ACT_OK)
		return ret;

	ret = ipv4_l3(ctx, ETH_HLEN, NULL, NULL, ip4);
	if (unlikely(ret != CTX_ACT_OK))
		goto drop_err;

	ret = __snat_v4_nat(ctx, &tuple, fraginfo, l4_off, true,
			    &target, TCP_SPORT_OFF, &trace, &ext_err);
	if (IS_ERR(ret))
		goto drop_err;

	/* This is also needed for from-overlay, to avoid a second SNAT by
	 * to-overlay or to-netdev.
	 */
	ctx_snat_done_set(ctx);

#ifdef TUNNEL_MODE
	if (info && tunnel_endpoint) {
		__be16 src_port;

		src_port = tunnel_gen_src_port_v4(&tuple);

		/* The request came from outside, so we need to
		 * set the security id in the tunnel header to WORLD_ID.
		 * Otherwise, the remote node will assume, that the
		 * request originated from a cluster node which will
		 * bypass any netpol which disallows LB requests from
		 * outside.
		 */
		ret = nodeport_add_tunnel_encap(ctx,
						IPV4_DIRECT_ROUTING,
						src_port,
						info,
						src_sec_identity,
						trace.reason,
						trace.monitor,
						&oif,
						bpf_htons(ETH_P_IP));
		if (IS_ERR(ret))
			goto drop_err;

		if (ret == CTX_ACT_REDIRECT && oif) {
			cilium_capture_out(ctx);
			return ctx_redirect(ctx, oif, 0);
		}
	}
#endif
	if (!revalidate_data(ctx, &data, &data_end, &ip4)) {
		ret = DROP_INVALID;
		goto drop_err;
	}

	fib_params.l.ipv4_src = ip4->saddr;
	fib_params.l.ipv4_dst = ip4->daddr;

	ret = fib_redirect(ctx, true, &fib_params, false, &ext_err, &oif);
	if (fib_ok(ret)) {
		cilium_capture_out(ctx);
		return ret;
	}
drop_err:
	return send_drop_notify_error_ext(ctx, UNKNOWN_ID, ret, ext_err,
					  METRIC_EGRESS);
}

static __always_inline int nodeport_svc_lb4(struct __ctx_buff *ctx,
					    struct ipv4_ct_tuple *tuple,
					    struct lb4_service *svc,
					    struct lb4_key *key,
					    struct iphdr *ip4,
					    int l3_off,
					    fraginfo_t fraginfo,
					    int l4_off,
					    __u32 src_sec_identity,
					    bool *punt_to_stack __maybe_unused,
					    __s8 *ext_err)
{
	struct ct_state ct_state_svc = {};
	__u32 cluster_id = 0;
	bool backend_local;
	__u32 monitor = 0;
	int ret;

	if (!lb4_src_range_ok(svc, ip4->saddr))
		return DROP_NOT_IN_SRC_RANGE;

	if (!lb4_svc_is_routable(svc))
		return DROP_IS_CLUSTER_IP;

#if defined(ENABLE_L7_LB)
	if (lb4_svc_is_l7_loadbalancer(svc)) {
		/* We cannot redirect from the XDP layer to cilium_host.
		 * Therefore, let the bpf_host to handle the L7 ingress
		 * request.
		 */
# if !defined(IS_BPF_XDP)
		__be16 proxy_port = (__be16)svc->l7_lb_proxy_port;

		send_trace_notify(ctx, TRACE_TO_PROXY, src_sec_identity, UNKNOWN_ID,
				  bpf_ntohs(proxy_port),
				  THIS_INTERFACE_IFINDEX, TRACE_REASON_POLICY, monitor,
				  bpf_htons(ETH_P_IP));

#  if defined(ENABLE_TPROXY)
		return ctx_redirect_to_proxy_hairpin_ipv4(ctx, ip4, proxy_port);
#  else
		/* Pass the packet straight to the proxy, without redirecting via
		 * cilium_host.
		 */
		cilium_dbg_capture(ctx, DBG_CAPTURE_PROXY_PRE, proxy_port);
		ctx->mark = MARK_MAGIC_TO_PROXY | (proxy_port << 16);
		cilium_dbg_capture(ctx, DBG_CAPTURE_PROXY_POST, proxy_port);

		*punt_to_stack = true;
#  endif /* ENABLE_TPROXY */
# endif /* IS_BPF_XDP */
		return CTX_ACT_OK;
	}
#endif
	if (lb4_to_lb6_service(svc)) {
		ret = lb4_to_lb6(ctx, ip4, l3_off);
		if (!ret)
			return NAT_46X64_RECIRC;
	} else {
		ret = lb4_local(get_ct_map4(tuple), ctx, l3_off, fraginfo, l4_off,
				key, tuple, svc, &ct_state_svc,
				nodeport_xlate4(svc, tuple), &cluster_id, ext_err, 0);
	}
	if (IS_ERR(ret)) {
#ifdef SERVICE_NO_BACKEND_RESPONSE
		if (ret == DROP_NO_SERVICE) {
			/* Packet is TX'ed back out, avoid EDT false-positives: */
			edt_set_aggregate(ctx, 0);
			ret = tail_call_internal(ctx, CILIUM_CALL_IPV4_NO_SERVICE,
						 ext_err);
		}
#endif
		if (ret == LB_PUNT_TO_STACK) {
			*punt_to_stack = true;
			return CTX_ACT_OK;
		}
		return ret;
	}

	backend_local = __lookup_ip4_endpoint(tuple->daddr);
	if (!backend_local && lb4_svc_is_hostport(svc))
		return DROP_INVALID;
	/* Reply from DSR packet is never seen on this node again
	 * hence no need to track in here.
	 */
	if (backend_local || !nodeport_uses_dsr4(svc, tuple)) {
		struct ct_state ct_state = {};

#if (defined(ENABLE_CLUSTER_AWARE_ADDRESSING) && defined(ENABLE_INTER_CLUSTER_SNAT))
		if (src_sec_identity == UNKNOWN_ID)
			src_sec_identity = WORLD_IPV4_ID;

		 /* Before forwarding the identity, make sure it's not local,
		  * as in that case the next hop would't understand it.
		  */
		if (identity_is_local(src_sec_identity))
			return DROP_INVALID_IDENTITY;

		if (identity_is_host(src_sec_identity))
			return DROP_INVALID_IDENTITY;
#else
		src_sec_identity = WORLD_IPV4_ID;
#endif

		/* lookup with SCOPE_FORWARD: */
		__ipv4_ct_tuple_reverse(tuple);

		/* only match CT entries that belong to the same service: */
		ct_state.rev_nat_index = ct_state_svc.rev_nat_index;

		/* Cache is_fragment in advance, lb4_local may invalidate ip4. */
		ret = ct_lazy_lookup4(get_ct_map4(tuple), tuple, ctx, fraginfo,
				      l4_off, CT_EGRESS, SCOPE_FORWARD,
				      CT_ENTRY_NODEPORT, &ct_state, &monitor);
		if (ret < 0)
			return ret;

		switch (ret) {
		case CT_NEW:
			ct_state.src_sec_id = src_sec_identity;
			ct_state.node_port = 1;

			ret = ct_create4(get_ct_map4(tuple), NULL, tuple, ctx,
					 CT_EGRESS, &ct_state, ext_err);
			if (IS_ERR(ret))
				return ret;
			break;
		case CT_ESTABLISHED:
			/* Note that we don't validate whether the matched CT entry
			 * has identical values (eg. .ifindex) as set above.
			 */
			break;
		default:
			return DROP_UNKNOWN_CT;
		}

		if (backend_local) {
			ctx_set_xfer(ctx, XFER_PKT_NO_SVC);
			return CTX_ACT_OK;
		}

		ret = neigh_record_ip4(ctx);
		if (ret < 0)
			return ret;
	}

	/* TX request to remote backend: */
	edt_set_aggregate(ctx, 0);
	if (nodeport_uses_dsr4(svc, tuple)) {
#if DSR_ENCAP_MODE == DSR_ENCAP_IPIP
		ctx_store_meta(ctx, CB_HINT,
			       ((__u32)tuple->sport << 16) | tuple->dport);
		ctx_store_meta(ctx, CB_ADDR_V4, tuple->daddr);
#elif DSR_ENCAP_MODE == DSR_ENCAP_GENEVE || DSR_ENCAP_MODE == DSR_ENCAP_NONE
		ctx_store_meta(ctx, CB_PORT, key->dport);
		ctx_store_meta(ctx, CB_ADDR_V4, key->address);
		ctx_store_meta(ctx, CB_DSR_L3_OFF, l3_off);
#endif /* DSR_ENCAP_MODE */
		return tail_call_internal(ctx, CILIUM_CALL_IPV4_NODEPORT_DSR, ext_err);
	}

	ctx_store_meta(ctx, CB_SRC_LABEL, src_sec_identity);
	ctx_store_meta(ctx, CB_CLUSTER_ID_EGRESS, cluster_id);
	return tail_call_internal(ctx, CILIUM_CALL_IPV4_NODEPORT_NAT_EGRESS,
				  ext_err);
}

/* Main node-port entry point for host-external ingressing node-port traffic
 * which handles the case of: i) backend is local EP, ii) backend is remote EP,
 * iii) reply from remote backend EP.
 */
static __always_inline int nodeport_lb4(struct __ctx_buff *ctx,
					struct iphdr *ip4,
					int l3_off,
					__u32 src_sec_identity,
					bool *punt_to_stack,
					__s8 *ext_err,
					bool __maybe_unused *dsr)
{
	fraginfo_t fraginfo;
	struct ipv4_ct_tuple tuple = {};
	bool is_svc_proto = true;
	struct lb4_service *svc;
	struct lb4_key key = {};
	int ret, l4_off;

	cilium_capture_in(ctx);

	fraginfo = ipfrag_encode_ipv4(ip4);
	l4_off = ETH_HLEN + ipv4_hdrlen(ip4);

	ret = lb4_extract_tuple(ctx, ip4, fraginfo, l4_off, &tuple);
	if (IS_ERR(ret)) {
		if (ret == DROP_UNSUPP_SERVICE_PROTO) {
			is_svc_proto = false;
			goto skip_service_lookup;
		}
		if (ret == DROP_UNKNOWN_L4) {
			ctx_set_xfer(ctx, XFER_PKT_NO_SVC);
			return CTX_ACT_OK;
		}
		return ret;
	}

	lb4_fill_key(&key, &tuple);

	svc = lb4_lookup_service(&key, false);
	if (svc) {
		return nodeport_svc_lb4(ctx, &tuple, svc, &key, ip4, l3_off,
					fraginfo, l4_off, src_sec_identity,
					punt_to_stack, ext_err);
	} else {
skip_service_lookup:
#ifdef ENABLE_NAT_46X64_GATEWAY
		if (ip4->daddr != IPV4_DIRECT_ROUTING)
			return tail_call_internal(ctx, CILIUM_CALL_IPV46_RFC6052, ext_err);
#endif
		/* The packet is not destined to a service but it can be a reply
		 * packet from a remote backend, in which case we need to perform
		 * the reverse NAT.
		 */
		ctx_set_xfer(ctx, XFER_PKT_NO_SVC);

#ifdef ENABLE_DSR
#if (defined(IS_BPF_OVERLAY) && DSR_ENCAP_MODE == DSR_ENCAP_GENEVE) || \
    ((defined(IS_BPF_XDP) || defined(IS_BPF_HOST) || defined(IS_BPF_WIREGUARD)) && \
		DSR_ENCAP_MODE != DSR_ENCAP_GENEVE)
		if (is_svc_proto) {
			/* Check if packet has embedded DSR info, or belongs to
			 * an established DSR connection:
			 */
			ret = nodeport_extract_dsr_v4(ctx, ip4, &tuple,
						      l4_off, &key.address,
						      &key.dport, dsr);
			if (IS_ERR(ret))
				return ret;
			if (*dsr)
				/* Packet continues on its way to local backend: */
				return nodeport_dsr_ingress_ipv4(ctx, &tuple, fraginfo, l4_off,
								 key.address, key.dport,
								 ext_err);
		}
#endif
#endif /* ENABLE_DSR */

#ifndef ENABLE_MASQUERADE_IPV4
		/* When BPF-Masquerading is off, we can skip the revSNAT path via
		 * CILIUM_CALL_IPV4_NODEPORT_NAT_INGRESS if the packet is ICMP.
		 */
		if (!is_svc_proto)
			return CTX_ACT_OK;
#endif /* ENABLE_MASQUERADE_IPV4 */

		ctx_store_meta(ctx, CB_SRC_LABEL, src_sec_identity);
		/* For NAT64 we might see an IPv4 reply from the backend to
		 * the LB entering this path. Thus, transform back to IPv6.
		 */
		if (is_svc_proto && snat_v6_has_v4_match(&tuple)) {
			ret = lb4_to_lb6(ctx, ip4, l3_off);
			if (ret)
				return ret;
			ctx_store_meta(ctx, CB_NAT_46X64, 0);
			return tail_call_internal(ctx, CILIUM_CALL_IPV6_NODEPORT_NAT_INGRESS,
						  ext_err);
#ifdef ENABLE_NAT_46X64_GATEWAY
		} else if (is_svc_proto &&
			   snat_v6_has_v4_match_rfc6052(&tuple)) {
			ret = snat_remap_rfc6052(ctx, ip4, l3_off);
			if (ret)
				return ret;
			ctx_store_meta(ctx, CB_NAT_46X64, NAT46x64_MODE_ROUTE);
			return tail_call_internal(ctx, CILIUM_CALL_IPV6_NODEPORT_NAT_INGRESS,
						  ext_err);
#endif
		}

		return tail_call_internal(ctx, CILIUM_CALL_IPV4_NODEPORT_NAT_INGRESS, ext_err);
	}
}
#endif /* ENABLE_IPV4 */

#endif /* ENABLE_NODEPORT */
