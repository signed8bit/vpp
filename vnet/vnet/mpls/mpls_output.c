/*
 * mpls_output.c: MPLS Adj rewrite
 *
 * Copyright (c) 2012-2014 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vlib/vlib.h>
#include <vnet/pg/pg.h>
#include <vnet/mpls/mpls.h>

typedef struct {
  /* Adjacency taken. */
  u32 adj_index;
  u32 flow_hash;

  /* Packet data, possibly *after* rewrite. */
  u8 packet_data[64 - 1*sizeof(u32)];
} mpls_output_trace_t;

static u8 *
format_mpls_output_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  mpls_output_trace_t * t = va_arg (*args, mpls_output_trace_t *);
  vnet_main_t * vnm = vnet_get_main();
  uword indent = format_get_indent (s);

  s = format (s, "adj-idx %d : %U flow hash: 0x%08x",
              t->adj_index,
              format_ip_adjacency, t->adj_index, FORMAT_IP_ADJACENCY_NONE,
	      t->flow_hash);
  s = format (s, "\n%U%U",
              format_white_space, indent,
              format_ip_adjacency_packet_data,
              vnm, t->adj_index,
              t->packet_data, sizeof (t->packet_data));
  return s;
}

static inline uword
mpls_output_inline (vlib_main_t * vm,
                    vlib_node_runtime_t * node,
                    vlib_frame_t * from_frame,
		    int is_midchain)
{
  u32 n_left_from, next_index, * from, * to_next, cpu_index;
  vlib_node_runtime_t * error_node;

  cpu_index = os_get_cpu_number();
  error_node = vlib_node_get_runtime (vm, mpls_output_node.index);
  from = vlib_frame_vector_args (from_frame);
  n_left_from = from_frame->n_vectors;
  next_index = node->cached_next_index;

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index,
                           to_next, n_left_to_next);

      while (n_left_from > 0 && n_left_to_next > 0)
        {
	  ip_adjacency_t * adj0;
          mpls_unicast_header_t *hdr0;
	  vlib_buffer_t * p0;
	  u32 pi0, rw_len0, adj_index0, next0, error0;

	  pi0 = to_next[0] = from[0];

	  p0 = vlib_get_buffer (vm, pi0);

	  adj_index0 = vnet_buffer (p0)->ip.adj_index[VLIB_TX];

          /* We should never rewrite a pkt using the MISS adjacency */
          ASSERT(adj_index0);

	  adj0 = adj_get(adj_index0);
      	  hdr0 = vlib_buffer_get_current (p0);

	  /* Guess we are only writing on simple Ethernet header. */
          vnet_rewrite_one_header (adj0[0], hdr0, 
                                   sizeof (ethernet_header_t));
          
          /* Update packet buffer attributes/set output interface. */
          rw_len0 = adj0[0].rewrite_header.data_bytes;
          
          if (PREDICT_FALSE (rw_len0 > sizeof(ethernet_header_t)))
              vlib_increment_combined_counter 
                  (&adjacency_counters,
                   cpu_index, adj_index0, 
                   /* packet increment */ 0,
                   /* byte increment */ rw_len0-sizeof(ethernet_header_t));
          
          /* Check MTU of outgoing interface. */
          error0 = (vlib_buffer_length_in_chain (vm, p0) 
                    > adj0[0].rewrite_header.max_l3_packet_bytes
                    ? IP4_ERROR_MTU_EXCEEDED
                    : IP4_ERROR_NONE);

	  p0->error = error_node->errors[error0];

          /* Don't adjust the buffer for ttl issue; icmp-error node wants
           * to see the IP headerr */
          if (PREDICT_TRUE(error0 == IP4_ERROR_NONE))
            {
              p0->current_data -= rw_len0;
              p0->current_length += rw_len0;

              vnet_buffer (p0)->sw_if_index[VLIB_TX] =
                  adj0[0].rewrite_header.sw_if_index;
              next0 = adj0[0].rewrite_header.next_index;

	      if (is_midchain)
	        {
		  adj0->sub_type.midchain.fixup_func(vm, adj0, p0);
		}
            }
          else
            {
              next0 = MPLS_OUTPUT_NEXT_DROP;
            }

	  from += 1;
	  n_left_from -= 1;
	  to_next += 1;
	  n_left_to_next -= 1;
      
          if (PREDICT_FALSE(p0->flags & VLIB_BUFFER_IS_TRACED)) 
            {
              mpls_output_trace_t *tr = vlib_add_trace (vm, node, 
                                                        p0, sizeof (*tr));
              tr->adj_index = vnet_buffer(p0)->ip.adj_index[VLIB_TX];
              tr->flow_hash = vnet_buffer(p0)->ip.flow_hash;
            }

	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index,
					   to_next, n_left_to_next,
					   pi0, next0);
        }

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }
  vlib_node_increment_counter (vm, mpls_output_node.index,
                               MPLS_ERROR_PKTS_ENCAP,
                               from_frame->n_vectors);

  return from_frame->n_vectors;
}

static char * mpls_error_strings[] = {
#define mpls_error(n,s) s,
#include "error.def"
#undef mpls_error
};

static inline uword
mpls_output (vlib_main_t * vm,
             vlib_node_runtime_t * node,
             vlib_frame_t * from_frame)
{
    return (mpls_output_inline(vm, node, from_frame, /* is_midchain */ 0));
}

VLIB_REGISTER_NODE (mpls_output_node) = {
  .function = mpls_output,
  .name = "mpls-output",
  /* Takes a vector of packets. */
  .vector_size = sizeof (u32),
  .n_errors = MPLS_N_ERROR,
  .error_strings = mpls_error_strings,

  .n_next_nodes = MPLS_OUTPUT_N_NEXT,
  .next_nodes = {
#define _(s,n) [MPLS_OUTPUT_NEXT_##s] = n,
    foreach_mpls_output_next
#undef _
  },

  .format_trace = format_mpls_output_trace,
};

VLIB_NODE_FUNCTION_MULTIARCH (mpls_output_node, mpls_output)

static inline uword
mpls_midchain (vlib_main_t * vm,
               vlib_node_runtime_t * node,
               vlib_frame_t * from_frame)
{
    return (mpls_output_inline(vm, node, from_frame, /* is_midchain */ 1));
}

VLIB_REGISTER_NODE (mpls_midchain_node) = {
  .function = mpls_midchain,
  .name = "mpls-midchain",
  .vector_size = sizeof (u32),

  .format_trace = format_mpls_output_trace,

  .sibling_of = "mpls-output",
};

VLIB_NODE_FUNCTION_MULTIARCH (mpls_midchain_node, mpls_midchain)

/**
 * @brief Next index values from the MPLS incomplete adj node
 */
#define foreach_mpls_adj_incomplete_next       	\
_(DROP, "error-drop")                   \
_(IP4,  "ip4-arp")                      \
_(IP6,  "ip6-discover-neighbor")

typedef enum {
#define _(s,n) MPLS_ADJ_INCOMPLETE_NEXT_##s,
  foreach_mpls_adj_incomplete_next
#undef _
  MPLS_ADJ_INCOMPLETE_N_NEXT,
} mpls_adj_incomplete_next_t;

/**
 * @brief A struct to hold tracing information for the MPLS label imposition
 * node.
 */
typedef struct mpls_adj_incomplete_trace_t_
{
    u32 next;
} mpls_adj_incomplete_trace_t;


/**
 * @brief Graph node for incomplete MPLS adjacency.
 * This node will push traffic to either the v4-arp or v6-nd node
 * based on the next-hop proto of the adj.
 * We pay a cost for this 'routing' node, but an incomplete adj is the
 * exception case.
 */
static inline uword
mpls_adj_incomplete (vlib_main_t * vm,
                     vlib_node_runtime_t * node,
                     vlib_frame_t * from_frame)
{
    u32 n_left_from, next_index, * from, * to_next;

  from = vlib_frame_vector_args (from_frame);
  n_left_from = from_frame->n_vectors;
  next_index = node->cached_next_index;

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index,
                           to_next, n_left_to_next);

      while (n_left_from > 0 && n_left_to_next > 0)
        {
          u32 pi0, next0, adj_index0;
	  ip_adjacency_t * adj0;
	  vlib_buffer_t * p0;

 	  pi0 = to_next[0] = from[0];
	  p0 = vlib_get_buffer (vm, pi0);
	  from += 1;
	  n_left_from -= 1;
	  to_next += 1;
	  n_left_to_next -= 1;

          adj_index0 = vnet_buffer (p0)->ip.adj_index[VLIB_TX];
          ASSERT(adj_index0);

	  adj0 = adj_get(adj_index0);

          if (PREDICT_TRUE(FIB_PROTOCOL_IP4 == adj0->ia_nh_proto))
          {
              next0 = MPLS_ADJ_INCOMPLETE_NEXT_IP4;
          }
          else
          {
              next0 = MPLS_ADJ_INCOMPLETE_NEXT_IP6;
          }              

	  if (PREDICT_FALSE(p0->flags & VLIB_BUFFER_IS_TRACED)) 
	  {
	      mpls_adj_incomplete_trace_t *tr =
		  vlib_add_trace (vm, node, p0, sizeof (*tr));
	      tr->next = next0;
	  }

	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index,
					   to_next, n_left_to_next,
					   pi0, next0);
        }

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  return from_frame->n_vectors;
}

static u8 *
format_mpls_adj_incomplete_trace (u8 * s, va_list * args)
{
    CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
    CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
    mpls_adj_incomplete_trace_t * t;
    uword indent;

    t = va_arg (*args, mpls_adj_incomplete_trace_t *);
    indent = format_get_indent (s);

    s = format (s, "%Unext:%d",
                format_white_space, indent,
                t->next);
    return (s);
}

VLIB_REGISTER_NODE (mpls_adj_incomplete_node) = {
  .function = mpls_adj_incomplete,
  .name = "mpls-adj-incomplete",
  .format_trace = format_mpls_adj_incomplete_trace,
  /* Takes a vector of packets. */
  .vector_size = sizeof (u32),
  .n_errors = MPLS_N_ERROR,
  .error_strings = mpls_error_strings,

  .n_next_nodes = MPLS_ADJ_INCOMPLETE_N_NEXT,
  .next_nodes = {
#define _(s,n) [MPLS_ADJ_INCOMPLETE_NEXT_##s] = n,
    foreach_mpls_adj_incomplete_next
#undef _
  },

  .format_trace = format_mpls_output_trace,
};

VLIB_NODE_FUNCTION_MULTIARCH (mpls_adj_incomplete_node,
                              mpls_adj_incomplete)
