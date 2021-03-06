/*
 * Copyright (c) 2016 Cisco and/or its affiliates.
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

#include <vnet/lisp-gpe/lisp_gpe_fwd_entry.h>
#include <vnet/lisp-gpe/lisp_gpe_adjacency.h>
#include <vnet/lisp-gpe/lisp_gpe_tenant.h>
#include <vnet/lisp-cp/lisp_cp_dpo.h>
#include <vnet/fib/fib_table.h>
#include <vnet/fib/fib_entry.h>
#include <vnet/fib/fib_path_list.h>
#include <vnet/fib/ip6_fib.h>
#include <vnet/fib/ip4_fib.h>
#include <vnet/dpo/drop_dpo.h>
#include <vnet/dpo/lookup_dpo.h>
#include <vnet/dpo/load_balance.h>
#include <vnet/adj/adj_midchain.h>

/**
 * @brief Add route to IP4 or IP6 Destination FIB.
 *
 * Add a route to the destination FIB that results in the lookup
 * in the SRC FIB. The SRC FIB is created is it does not yet exist.
 *
 * @param[in]   dst_table_id    Destination FIB Table-ID
 * @param[in]   dst_prefix      Destination IP prefix.
 *
 * @return  src_fib_index   The index/ID of the SRC FIB created.
 */
static u32
ip_dst_fib_add_route (u32 dst_fib_index, const ip_prefix_t * dst_prefix)
{
  fib_node_index_t src_fib_index;
  fib_prefix_t dst_fib_prefix;
  fib_node_index_t dst_fei;

  ASSERT (NULL != dst_prefix);

  ip_prefix_to_fib_prefix (dst_prefix, &dst_fib_prefix);

  /*
   * lookup the destination prefix in the VRF table and retrieve the
   * LISP associated data
   */
  dst_fei = fib_table_lookup_exact_match (dst_fib_index, &dst_fib_prefix);

  /*
   * If the FIB entry is not present, or not LISP sourced, add it
   */
  if (dst_fei == FIB_NODE_INDEX_INVALID ||
      NULL == fib_entry_get_source_data (dst_fei, FIB_SOURCE_LISP))
    {
      dpo_id_t src_lkup_dpo = DPO_NULL;

      /* create a new src FIB.  */
      src_fib_index =
	fib_table_create_and_lock (dst_fib_prefix.fp_proto,
				   "LISP-src for [%d,%U]",
				   dst_fib_index,
				   format_fib_prefix, &dst_fib_prefix);

      /*
       * create a data-path object to perform the source address lookup
       * in the SRC FIB
       */
      lookup_dpo_add_or_lock_w_fib_index (src_fib_index,
					  (ip_prefix_version (dst_prefix) ==
					   IP6 ? DPO_PROTO_IP6 :
					   DPO_PROTO_IP4),
					  LOOKUP_INPUT_SRC_ADDR,
					  LOOKUP_TABLE_FROM_CONFIG,
					  &src_lkup_dpo);

      /*
       * add the entry to the destination FIB that uses the lookup DPO
       */
      dst_fei = fib_table_entry_special_dpo_add (dst_fib_index,
						 &dst_fib_prefix,
						 FIB_SOURCE_LISP,
						 FIB_ENTRY_FLAG_EXCLUSIVE,
						 &src_lkup_dpo);

      /*
       * the DPO is locked by the FIB entry, and we have no further
       * need for it.
       */
      dpo_unlock (&src_lkup_dpo);

      /*
       * save the SRC FIB index on the entry so we can retrieve it for
       * subsequent routes.
       */
      fib_entry_set_source_data (dst_fei, FIB_SOURCE_LISP, &src_fib_index);
    }
  else
    {
      /*
       * destination FIB entry already present
       */
      src_fib_index = *(u32 *) fib_entry_get_source_data (dst_fei,
							  FIB_SOURCE_LISP);
    }

  return (src_fib_index);
}

/**
 * @brief Del route to IP4 or IP6 SD FIB.
 *
 * Remove routes from both destination and source FIBs.
 *
 * @param[in]   src_fib_index   The index/ID of the SRC FIB
 * @param[in]   src_prefix      Source IP prefix.
 * @param[in]   dst_fib_index   The index/ID of the DST FIB
 * @param[in]   dst_prefix      Destination IP prefix.
 */
static void
ip_src_dst_fib_del_route (u32 src_fib_index,
			  const ip_prefix_t * src_prefix,
			  u32 dst_fib_index, const ip_prefix_t * dst_prefix)
{
  fib_prefix_t dst_fib_prefix, src_fib_prefix;

  ASSERT (NULL != dst_prefix);
  ASSERT (NULL != src_prefix);

  ip_prefix_to_fib_prefix (dst_prefix, &dst_fib_prefix);
  ip_prefix_to_fib_prefix (src_prefix, &src_fib_prefix);

  fib_table_entry_delete (src_fib_index, &src_fib_prefix, FIB_SOURCE_LISP);

  if (0 == fib_table_get_num_entries (src_fib_index,
				      src_fib_prefix.fp_proto,
				      FIB_SOURCE_LISP))
    {
      /*
       * there's nothing left, unlock the source FIB and the
       * destination route
       */
      fib_table_entry_special_remove (dst_fib_index,
				      &dst_fib_prefix, FIB_SOURCE_LISP);
      fib_table_unlock (src_fib_index, src_fib_prefix.fp_proto);
    }
}

/**
 * @brief Add route to IP4 or IP6 SRC FIB.
 *
 * Adds a route to in the LISP SRC FIB with the result of the route
 * being the DPO passed.
 *
 * @param[in]   src_fib_index   The index/ID of the SRC FIB
 * @param[in]   src_prefix      Source IP prefix.
 * @param[in]   src_dpo         The DPO the route will link to.
 */
static void
ip_src_fib_add_route_w_dpo (u32 src_fib_index,
			    const ip_prefix_t * src_prefix,
			    const dpo_id_t * src_dpo)
{
  fib_prefix_t src_fib_prefix;

  ip_prefix_to_fib_prefix (src_prefix, &src_fib_prefix);

  /*
   * add the entry into the source fib.
   */
  fib_node_index_t src_fei;

  src_fei = fib_table_lookup_exact_match (src_fib_index, &src_fib_prefix);

  if (FIB_NODE_INDEX_INVALID == src_fei ||
      !fib_entry_is_sourced (src_fei, FIB_SOURCE_LISP))
    {
      fib_table_entry_special_dpo_add (src_fib_index,
				       &src_fib_prefix,
				       FIB_SOURCE_LISP,
				       FIB_ENTRY_FLAG_EXCLUSIVE, src_dpo);
    }
}

static fib_route_path_t *
lisp_gpe_mk_fib_paths (const lisp_fwd_path_t * paths)
{
  const lisp_gpe_adjacency_t *ladj;
  fib_route_path_t *rpaths = NULL;
  u8 best_priority;
  u32 ii;

  vec_validate (rpaths, vec_len (paths) - 1);

  best_priority = paths[0].priority;

  vec_foreach_index (ii, paths)
  {
    if (paths[0].priority != best_priority)
      break;

    ladj = lisp_gpe_adjacency_get (paths[ii].lisp_adj);

    ip_address_to_46 (&ladj->remote_rloc,
		      &rpaths[ii].frp_addr, &rpaths[ii].frp_proto);

    rpaths[ii].frp_sw_if_index = ladj->sw_if_index;
    rpaths[ii].frp_weight = (paths[ii].weight ? paths[ii].weight : 1);
    rpaths[ii].frp_label = MPLS_LABEL_INVALID;
  }

  ASSERT (0 != vec_len (rpaths));

  return (rpaths);
}

/**
 * @brief Add route to IP4 or IP6 SRC FIB.
 *
 * Adds a route to in the LISP SRC FIB for the tunnel.
 *
 * @param[in]   src_fib_index   The index/ID of the SRC FIB
 * @param[in]   src_prefix      Source IP prefix.
 * @param[in]   paths           The paths from which to construct the
 *                              load balance
 */
static void
ip_src_fib_add_route (u32 src_fib_index,
		      const ip_prefix_t * src_prefix,
		      const lisp_fwd_path_t * paths)
{
  fib_prefix_t src_fib_prefix;
  fib_route_path_t *rpaths;

  ip_prefix_to_fib_prefix (src_prefix, &src_fib_prefix);

  rpaths = lisp_gpe_mk_fib_paths (paths);

  fib_table_entry_update (src_fib_index,
			  &src_fib_prefix,
			  FIB_SOURCE_LISP, FIB_ENTRY_FLAG_NONE, rpaths);
  vec_free (rpaths);
}


static void
create_fib_entries (lisp_gpe_fwd_entry_t * lfe)
{
  dpo_proto_t dproto;

  dproto = (ip_prefix_version (&lfe->key->rmt.ippref) == IP4 ?
	    FIB_PROTOCOL_IP4 : FIB_PROTOCOL_IP6);

  lfe->src_fib_index = ip_dst_fib_add_route (lfe->eid_fib_index,
					     &lfe->key->rmt.ippref);

  if (LISP_GPE_FWD_ENTRY_TYPE_NEGATIVE == lfe->type)
    {
      dpo_id_t dpo = DPO_NULL;

      switch (lfe->action)
	{
	case LISP_NO_ACTION:
	  /* TODO update timers? */
	case LISP_FORWARD_NATIVE:
	  /* TODO check if route/next-hop for eid exists in fib and add
	   * more specific for the eid with the next-hop found */
	case LISP_SEND_MAP_REQUEST:
	  /* insert tunnel that always sends map-request */
	  dpo_copy (&dpo, lisp_cp_dpo_get (dproto));
	  break;
	case LISP_DROP:
	  /* for drop fwd entries, just add route, no need to add encap tunnel */
	  dpo_copy (&dpo, drop_dpo_get (dproto));
	  break;
	}
      ip_src_fib_add_route_w_dpo (lfe->src_fib_index,
				  &lfe->key->lcl.ippref, &dpo);
      dpo_reset (&dpo);
    }
  else
    {
      ip_src_fib_add_route (lfe->src_fib_index,
			    &lfe->key->lcl.ippref, lfe->paths);
    }
}

static void
delete_fib_entries (lisp_gpe_fwd_entry_t * lfe)
{
  ip_src_dst_fib_del_route (lfe->src_fib_index,
			    &lfe->key->lcl.ippref,
			    lfe->eid_fib_index, &lfe->key->rmt.ippref);
}

static void
gid_to_dp_address (gid_address_t * g, dp_address_t * d)
{
  switch (gid_address_type (g))
    {
    case GID_ADDR_IP_PREFIX:
    case GID_ADDR_SRC_DST:
      ip_prefix_copy (&d->ippref, &gid_address_ippref (g));
      d->type = FID_ADDR_IP_PREF;
      break;
    case GID_ADDR_MAC:
    default:
      mac_copy (&d->mac, &gid_address_mac (g));
      d->type = FID_ADDR_MAC;
      break;
    }
}

static lisp_gpe_fwd_entry_t *
find_fwd_entry (lisp_gpe_main_t * lgm,
		vnet_lisp_gpe_add_del_fwd_entry_args_t * a,
		lisp_gpe_fwd_entry_key_t * key)
{
  uword *p;

  memset (key, 0, sizeof (*key));

  if (GID_ADDR_IP_PREFIX == gid_address_type (&a->rmt_eid))
    {
      /*
       * the ip version of the source is not set to ip6 when the
       * source is all zeros. force it.
       */
      ip_prefix_version (&gid_address_ippref (&a->lcl_eid)) =
	ip_prefix_version (&gid_address_ippref (&a->rmt_eid));
    }

  gid_to_dp_address (&a->rmt_eid, &key->rmt);
  gid_to_dp_address (&a->lcl_eid, &key->lcl);
  key->vni = a->vni;

  p = hash_get_mem (lgm->lisp_gpe_fwd_entries, key);

  if (NULL != p)
    {
      return (pool_elt_at_index (lgm->lisp_fwd_entry_pool, p[0]));
    }
  return (NULL);
}

static int
lisp_gpe_fwd_entry_path_sort (void *a1, void *a2)
{
  lisp_fwd_path_t *p1 = a1, *p2 = a2;

  return (p1->priority - p2->priority);
}

static void
lisp_gpe_fwd_entry_mk_paths (lisp_gpe_fwd_entry_t * lfe,
			     vnet_lisp_gpe_add_del_fwd_entry_args_t * a)
{
  const lisp_gpe_tenant_t *lt;
  lisp_fwd_path_t *path;
  u32 index;

  lt = lisp_gpe_tenant_get (lfe->tenant);
  vec_validate (lfe->paths, vec_len (a->locator_pairs) - 1);

  vec_foreach_index (index, a->locator_pairs)
  {
    path = &lfe->paths[index];

    path->priority = a->locator_pairs[index].priority;
    path->weight = a->locator_pairs[index].weight;

    path->lisp_adj =
      lisp_gpe_adjacency_find_or_create_and_lock (&a->locator_pairs
						  [index],
						  lt->lt_table_id,
						  lfe->key->vni);
  }
  vec_sort_with_function (lfe->paths, lisp_gpe_fwd_entry_path_sort);
}

/**
 * @brief Add/Delete LISP IP forwarding entry.
 *
 * creation of forwarding entries for IP LISP overlay:
 *
 * @param[in]   lgm     Reference to @ref lisp_gpe_main_t.
 * @param[in]   a       Parameters for building the forwarding entry.
 *
 * @return 0 on success.
 */
static int
add_ip_fwd_entry (lisp_gpe_main_t * lgm,
		  vnet_lisp_gpe_add_del_fwd_entry_args_t * a)
{
  lisp_gpe_fwd_entry_key_t key;
  lisp_gpe_fwd_entry_t *lfe;
  fib_protocol_t fproto;

  lfe = find_fwd_entry (lgm, a, &key);

  if (NULL != lfe)
    /* don't support updates */
    return VNET_API_ERROR_INVALID_VALUE;

  pool_get (lgm->lisp_fwd_entry_pool, lfe);
  memset (lfe, 0, sizeof (*lfe));
  lfe->key = clib_mem_alloc (sizeof (key));
  memcpy (lfe->key, &key, sizeof (key));

  hash_set_mem (lgm->lisp_gpe_fwd_entries, lfe->key,
		lfe - lgm->lisp_fwd_entry_pool);

  fproto = (IP4 == ip_prefix_version (&fid_addr_ippref (&lfe->key->rmt)) ?
	    FIB_PROTOCOL_IP4 : FIB_PROTOCOL_IP6);

  lfe->type = (a->is_negative ?
	       LISP_GPE_FWD_ENTRY_TYPE_NEGATIVE :
	       LISP_GPE_FWD_ENTRY_TYPE_NORMAL);
  lfe->tenant = lisp_gpe_tenant_find_or_create (lfe->key->vni);
  lfe->eid_table_id = a->table_id;
  lfe->eid_fib_index = fib_table_find_or_create_and_lock (fproto,
							  lfe->eid_table_id);

  if (LISP_GPE_FWD_ENTRY_TYPE_NEGATIVE != lfe->type)
    {
      lisp_gpe_fwd_entry_mk_paths (lfe, a);
    }

  create_fib_entries (lfe);

  return (0);
}

static void
del_ip_fwd_entry_i (lisp_gpe_main_t * lgm, lisp_gpe_fwd_entry_t * lfe)
{
  lisp_fwd_path_t *path;
  fib_protocol_t fproto;

  vec_foreach (path, lfe->paths)
  {
    lisp_gpe_adjacency_unlock (path->lisp_adj);
  }

  delete_fib_entries (lfe);

  fproto = (IP4 == ip_prefix_version (&fid_addr_ippref (&lfe->key->rmt)) ?
	    FIB_PROTOCOL_IP4 : FIB_PROTOCOL_IP6);
  fib_table_unlock (lfe->eid_fib_index, fproto);

  hash_unset_mem (lgm->lisp_gpe_fwd_entries, lfe->key);
  clib_mem_free (lfe->key);
  pool_put (lgm->lisp_fwd_entry_pool, lfe);
}

/**
 * @brief Add/Delete LISP IP forwarding entry.
 *
 * removal of forwarding entries for IP LISP overlay:
 *
 * @param[in]   lgm     Reference to @ref lisp_gpe_main_t.
 * @param[in]   a       Parameters for building the forwarding entry.
 *
 * @return 0 on success.
 */
static int
del_ip_fwd_entry (lisp_gpe_main_t * lgm,
		  vnet_lisp_gpe_add_del_fwd_entry_args_t * a)
{
  lisp_gpe_fwd_entry_key_t key;
  lisp_gpe_fwd_entry_t *lfe;

  lfe = find_fwd_entry (lgm, a, &key);

  if (NULL == lfe)
    /* no such entry */
    return VNET_API_ERROR_INVALID_VALUE;

  del_ip_fwd_entry_i (lgm, lfe);

  return (0);
}

static void
make_mac_fib_key (BVT (clib_bihash_kv) * kv, u16 bd_index, u8 src_mac[6],
		  u8 dst_mac[6])
{
  kv->key[0] = (((u64) bd_index) << 48) | mac_to_u64 (dst_mac);
  kv->key[1] = mac_to_u64 (src_mac);
  kv->key[2] = 0;
}

/**
 * @brief Lookup L2 SD FIB entry
 *
 * Does a vni + dest + source lookup in the L2 LISP FIB. If the lookup fails
 * it tries a second time with source set to 0 (i.e., a simple dest lookup).
 *
 * @param[in]   lgm             Reference to @ref lisp_gpe_main_t.
 * @param[in]   bd_index        Bridge domain index.
 * @param[in]   src_mac         Source mac address.
 * @param[in]   dst_mac         Destination mac address.
 *
 * @return index of mapping matching the lookup key.
 */
index_t
lisp_l2_fib_lookup (lisp_gpe_main_t * lgm, u16 bd_index, u8 src_mac[6],
		    u8 dst_mac[6])
{
  int rv;
  BVT (clib_bihash_kv) kv, value;

  make_mac_fib_key (&kv, bd_index, src_mac, dst_mac);
  rv = BV (clib_bihash_search_inline_2) (&lgm->l2_fib, &kv, &value);

  /* no match, try with src 0, catch all for dst */
  if (rv != 0)
    {
      kv.key[1] = 0;
      rv = BV (clib_bihash_search_inline_2) (&lgm->l2_fib, &kv, &value);
      if (rv == 0)
	return value.value;
    }

  return lisp_gpe_main.l2_lb_cp_lkup.dpoi_index;
}

/**
 * @brief Add/del L2 SD FIB entry
 *
 * Inserts value in L2 FIB keyed by vni + dest + source. If entry is
 * overwritten the associated value is returned.
 *
 * @param[in]   lgm             Reference to @ref lisp_gpe_main_t.
 * @param[in]   bd_index        Bridge domain index.
 * @param[in]   src_mac         Source mac address.
 * @param[in]   dst_mac         Destination mac address.
 * @param[in]   val             Value to add.
 * @param[in]   is_add          Add/del flag.
 *
 * @return ~0 or value of overwritten entry.
 */
static u32
lisp_l2_fib_add_del_entry (u16 bd_index, u8 src_mac[6],
			   u8 dst_mac[6], const dpo_id_t * dpo, u8 is_add)
{
  lisp_gpe_main_t *lgm = &lisp_gpe_main;
  BVT (clib_bihash_kv) kv, value;
  u32 old_val = ~0;

  make_mac_fib_key (&kv, bd_index, src_mac, dst_mac);

  if (BV (clib_bihash_search) (&lgm->l2_fib, &kv, &value) == 0)
    old_val = value.value;

  if (!is_add)
    BV (clib_bihash_add_del) (&lgm->l2_fib, &kv, 0 /* is_add */ );
  else
    {
      kv.value = dpo->dpoi_index;
      BV (clib_bihash_add_del) (&lgm->l2_fib, &kv, 1 /* is_add */ );
    }
  return old_val;
}

#define L2_FIB_DEFAULT_HASH_NUM_BUCKETS (64 * 1024)
#define L2_FIB_DEFAULT_HASH_MEMORY_SIZE (32<<20)

static void
l2_fib_init (lisp_gpe_main_t * lgm)
{
  index_t lbi;

  BV (clib_bihash_init) (&lgm->l2_fib, "l2 fib",
			 1 << max_log2 (L2_FIB_DEFAULT_HASH_NUM_BUCKETS),
			 L2_FIB_DEFAULT_HASH_MEMORY_SIZE);

  /*
   * the result from a 'miss' in a L2 Table
   */
  lbi = load_balance_create (1, DPO_PROTO_ETHERNET, 0);
  load_balance_set_bucket (lbi, 0, lisp_cp_dpo_get (DPO_PROTO_ETHERNET));

  dpo_set (&lgm->l2_lb_cp_lkup, DPO_LOAD_BALANCE, DPO_PROTO_ETHERNET, lbi);
}

static void
del_l2_fwd_entry_i (lisp_gpe_main_t * lgm, lisp_gpe_fwd_entry_t * lfe)
{
  lisp_fwd_path_t *path;

  if (LISP_GPE_FWD_ENTRY_TYPE_NEGATIVE != lfe->type)
    {
      vec_foreach (path, lfe->paths)
      {
	lisp_gpe_adjacency_unlock (path->lisp_adj);
      }
      fib_path_list_child_remove (lfe->l2.path_list_index,
				  lfe->l2.child_index);
    }

  lisp_l2_fib_add_del_entry (lfe->l2.eid_bd_index,
			     fid_addr_mac (&lfe->key->lcl),
			     fid_addr_mac (&lfe->key->rmt), NULL, 0);

  hash_unset_mem (lgm->lisp_gpe_fwd_entries, lfe->key);
  clib_mem_free (lfe->key);
  pool_put (lgm->lisp_fwd_entry_pool, lfe);
}

/**
 * @brief Delete LISP L2 forwarding entry.
 *
 * Coordinates the removal of forwarding entries for L2 LISP overlay:
 *
 * @param[in]   lgm     Reference to @ref lisp_gpe_main_t.
 * @param[in]   a       Parameters for building the forwarding entry.
 *
 * @return 0 on success.
 */
static int
del_l2_fwd_entry (lisp_gpe_main_t * lgm,
		  vnet_lisp_gpe_add_del_fwd_entry_args_t * a)
{
  lisp_gpe_fwd_entry_key_t key;
  lisp_gpe_fwd_entry_t *lfe;

  lfe = find_fwd_entry (lgm, a, &key);

  if (NULL == lfe)
    return VNET_API_ERROR_INVALID_VALUE;

  del_l2_fwd_entry_i (lgm, lfe);

  return (0);
}

/**
 * @brief Construct and insert the forwarding information used by a L2 entry
 */
static void
lisp_gpe_l2_update_fwding (lisp_gpe_fwd_entry_t * lfe)
{
  lisp_gpe_main_t *lgm = &lisp_gpe_main;
  dpo_id_t dpo = DPO_NULL;

  if (LISP_GPE_FWD_ENTRY_TYPE_NEGATIVE != lfe->type)
    {
      fib_path_list_contribute_forwarding (lfe->l2.path_list_index,
					   FIB_FORW_CHAIN_TYPE_ETHERNET,
					   &lfe->l2.dpo);
      dpo_copy (&dpo, &lfe->l2.dpo);
    }
  else
    {
      dpo_copy (&dpo, &lgm->l2_lb_cp_lkup);
    }

  /* add entry to l2 lisp fib */
  lisp_l2_fib_add_del_entry (lfe->l2.eid_bd_index,
			     fid_addr_mac (&lfe->key->lcl),
			     fid_addr_mac (&lfe->key->rmt), &dpo, 1);

  dpo_reset (&dpo);
}

/**
 * @brief Add LISP L2 forwarding entry.
 *
 * Coordinates the creation of forwarding entries for L2 LISP overlay:
 * creates lisp-gpe tunnel and injects new entry in Source/Dest L2 FIB.
 *
 * @param[in]   lgm     Reference to @ref lisp_gpe_main_t.
 * @param[in]   a       Parameters for building the forwarding entry.
 *
 * @return 0 on success.
 */
static int
add_l2_fwd_entry (lisp_gpe_main_t * lgm,
		  vnet_lisp_gpe_add_del_fwd_entry_args_t * a)
{
  lisp_gpe_fwd_entry_key_t key;
  bd_main_t *bdm = &bd_main;
  lisp_gpe_fwd_entry_t *lfe;
  uword *bd_indexp;

  bd_indexp = hash_get (bdm->bd_index_by_bd_id, a->bd_id);
  if (!bd_indexp)
    {
      clib_warning ("bridge domain %d doesn't exist", a->bd_id);
      return -1;
    }

  lfe = find_fwd_entry (lgm, a, &key);

  if (NULL != lfe)
    /* don't support updates */
    return VNET_API_ERROR_INVALID_VALUE;

  pool_get (lgm->lisp_fwd_entry_pool, lfe);
  memset (lfe, 0, sizeof (*lfe));
  lfe->key = clib_mem_alloc (sizeof (key));
  memcpy (lfe->key, &key, sizeof (key));

  hash_set_mem (lgm->lisp_gpe_fwd_entries, lfe->key,
		lfe - lgm->lisp_fwd_entry_pool);

  lfe->type = (a->is_negative ?
	       LISP_GPE_FWD_ENTRY_TYPE_NEGATIVE :
	       LISP_GPE_FWD_ENTRY_TYPE_NORMAL);
  lfe->l2.eid_bd_id = a->bd_id;
  lfe->l2.eid_bd_index = bd_indexp[0];
  lfe->tenant = lisp_gpe_tenant_find_or_create (lfe->key->vni);

  if (LISP_GPE_FWD_ENTRY_TYPE_NEGATIVE != lfe->type)
    {
      fib_route_path_t *rpaths;

      /*
       * Make the sorted array of LISP paths with their resp. adjacency
       */
      lisp_gpe_fwd_entry_mk_paths (lfe, a);

      /*
       * From the LISP paths, construct a FIB path list that will
       * contribute a load-balance.
       */
      rpaths = lisp_gpe_mk_fib_paths (lfe->paths);

      lfe->l2.path_list_index =
	fib_path_list_create (FIB_PATH_LIST_FLAG_NONE, rpaths);

      /*
       * become a child of the path-list so we receive updates when
       * its forwarding state changes. this includes an implicit lock.
       */
      lfe->l2.child_index =
	fib_path_list_child_add (lfe->l2.path_list_index,
				 FIB_NODE_TYPE_LISP_GPE_FWD_ENTRY,
				 lfe - lgm->lisp_fwd_entry_pool);
    }
  else
    {
      lfe->action = a->action;
    }

  lisp_gpe_l2_update_fwding (lfe);

  return 0;
}

/**
 * @brief conver from the embedded fib_node_t struct to the LSIP entry
 */
static lisp_gpe_fwd_entry_t *
lisp_gpe_fwd_entry_from_fib_node (fib_node_t * node)
{
  return ((lisp_gpe_fwd_entry_t *) (((char *) node) -
				    STRUCT_OFFSET_OF (lisp_gpe_fwd_entry_t,
						      node)));
}

/**
 * @brief Function invoked during a backwalk of the FIB graph
 */
static fib_node_back_walk_rc_t
lisp_gpe_fib_node_back_walk (fib_node_t * node,
			     fib_node_back_walk_ctx_t * ctx)
{
  lisp_gpe_l2_update_fwding (lisp_gpe_fwd_entry_from_fib_node (node));

  return (FIB_NODE_BACK_WALK_CONTINUE);
}

/**
 * @brief Get a fib_node_t struct from the index of a LISP fwd entry
 */
static fib_node_t *
lisp_gpe_fwd_entry_get_fib_node (fib_node_index_t index)
{
  lisp_gpe_main_t *lgm = &lisp_gpe_main;
  lisp_gpe_fwd_entry_t *lfe;

  lfe = pool_elt_at_index (lgm->lisp_fwd_entry_pool, index);

  return (&(lfe->node));
}

/**
 * @brief An indication from the graph that the last lock has gone
 */
static void
lisp_gpe_fwd_entry_fib_node_last_lock_gone (fib_node_t * node)
{
  /* We don't manage the locks of the LISP objects via the graph, since
   * this object has no children. so this is a no-op. */
}

/**
 * @brief Virtual function table to register with FIB for the LISP type
 */
const static fib_node_vft_t lisp_fwd_vft = {
  .fnv_get = lisp_gpe_fwd_entry_get_fib_node,
  .fnv_last_lock = lisp_gpe_fwd_entry_fib_node_last_lock_gone,
  .fnv_back_walk = lisp_gpe_fib_node_back_walk,
};

/**
 * @brief Forwarding entry create/remove dispatcher.
 *
 * Calls l2 or l3 forwarding entry add/del function based on input data.
 *
 * @param[in]   a       Forwarding entry parameters.
 * @param[out]  hw_if_indexp    NOT USED
 *
 * @return 0 on success.
 */
int
vnet_lisp_gpe_add_del_fwd_entry (vnet_lisp_gpe_add_del_fwd_entry_args_t * a,
				 u32 * hw_if_indexp)
{
  lisp_gpe_main_t *lgm = &lisp_gpe_main;
  u8 type;

  if (vnet_lisp_gpe_enable_disable_status () == 0)
    {
      clib_warning ("LISP is disabled!");
      return VNET_API_ERROR_LISP_DISABLED;
    }

  type = gid_address_type (&a->rmt_eid);
  switch (type)
    {
    case GID_ADDR_IP_PREFIX:
      if (a->is_add)
	return add_ip_fwd_entry (lgm, a);
      else
	return del_ip_fwd_entry (lgm, a);
      break;
    case GID_ADDR_MAC:
      if (a->is_add)
	return add_l2_fwd_entry (lgm, a);
      else
	return del_l2_fwd_entry (lgm, a);
    default:
      clib_warning ("Forwarding entries for type %d not supported!", type);
      return -1;
    }
}

/**
 * @brief Flush all the forwrding entries
 */
void
vnet_lisp_gpe_fwd_entry_flush (void)
{
  lisp_gpe_main_t *lgm = &lisp_gpe_main;
  lisp_gpe_fwd_entry_t *lfe;

  /* *INDENT-OFF* */
  pool_foreach (lfe, lgm->lisp_fwd_entry_pool,
  ({
    switch (fid_addr_type(&lfe->key->rmt))
      {
      case FID_ADDR_MAC:
	del_l2_fwd_entry_i (lgm, lfe);
	break;
      case FID_ADDR_IP_PREF:
	del_ip_fwd_entry_i (lgm, lfe);
	break;
      }
  }));
  /* *INDENT-ON* */
}

static u8 *
format_lisp_fwd_path (u8 * s, va_list ap)
{
  lisp_fwd_path_t *lfp = va_arg (ap, lisp_fwd_path_t *);

  s = format (s, "pirority:%d weight:%d ", lfp->priority, lfp->weight);
  s = format (s, "adj:[%U]\n",
	      format_lisp_gpe_adjacency,
	      lisp_gpe_adjacency_get (lfp->lisp_adj),
	      LISP_GPE_ADJ_FORMAT_FLAG_NONE);

  return (s);
}

typedef enum lisp_gpe_fwd_entry_format_flag_t_
{
  LISP_GPE_FWD_ENTRY_FORMAT_NONE = (0 << 0),
  LISP_GPE_FWD_ENTRY_FORMAT_DETAIL = (1 << 1),
} lisp_gpe_fwd_entry_format_flag_t;


static u8 *
format_lisp_gpe_fwd_entry (u8 * s, va_list ap)
{
  lisp_gpe_main_t *lgm = &lisp_gpe_main;
  lisp_gpe_fwd_entry_t *lfe = va_arg (ap, lisp_gpe_fwd_entry_t *);
  lisp_gpe_fwd_entry_format_flag_t flags =
    va_arg (ap, lisp_gpe_fwd_entry_format_flag_t);

  s = format (s, "VNI:%d VRF:%d EID: %U -> %U  [index:%d]",
	      lfe->key->vni, lfe->eid_table_id,
	      format_fid_address, &lfe->key->lcl,
	      format_fid_address, &lfe->key->rmt,
	      lfe - lgm->lisp_fwd_entry_pool);

  if (LISP_GPE_FWD_ENTRY_TYPE_NEGATIVE == lfe->type)
    {
      s = format (s, "\n Negative - action:%U",
		  format_negative_mapping_action, lfe->action);
    }
  else
    {
      lisp_fwd_path_t *path;

      s = format (s, "\n via:");
      vec_foreach (path, lfe->paths)
      {
	s = format (s, "\n  %U", format_lisp_fwd_path, path);
      }
    }

  if (flags & LISP_GPE_FWD_ENTRY_FORMAT_DETAIL)
    {
      switch (fid_addr_type (&lfe->key->rmt))
	{
	case FID_ADDR_MAC:
	  s = format (s, " fib-path-list:%d\n", lfe->l2.path_list_index);
	  s = format (s, " dpo:%U\n", format_dpo_id, &lfe->l2.dpo, 0);
	  break;
	case FID_ADDR_IP_PREF:
	  break;
	}
    }

  return (s);
}

static clib_error_t *
lisp_gpe_fwd_entry_show (vlib_main_t * vm,
			 unformat_input_t * input, vlib_cli_command_t * cmd)
{
  lisp_gpe_main_t *lgm = &lisp_gpe_main;
  lisp_gpe_fwd_entry_t *lfe;
  index_t index;
  u32 vni = ~0;

  if (unformat (input, "vni %d", &vni))
    ;
  else if (unformat (input, "%d", &index))
    {
      if (!pool_is_free_index (lgm->lisp_fwd_entry_pool, index))
	{
	  lfe = pool_elt_at_index (lgm->lisp_fwd_entry_pool, index);

	  vlib_cli_output (vm, "[%d@] %U",
			   index,
			   format_lisp_gpe_fwd_entry, lfe,
			   LISP_GPE_FWD_ENTRY_FORMAT_DETAIL);
	}
      else
	{
	  vlib_cli_output (vm, "entry %d invalid", index);
	}

      return (NULL);
    }

  /* *INDENT-OFF* */
  pool_foreach (lfe, lgm->lisp_fwd_entry_pool,
  ({
    if ((vni == ~0) ||
	(lfe->key->vni == vni))
      vlib_cli_output (vm, "%U", format_lisp_gpe_fwd_entry, lfe,
		       LISP_GPE_FWD_ENTRY_FORMAT_NONE);
  }));
  /* *INDENT-ON* */

  return (NULL);
}

/* *INDENT-OFF* */
VLIB_CLI_COMMAND (lisp_gpe_fwd_entry_show_command, static) = {
  .path = "show lisp gpe entry",
  .short_help = "show lisp gpe entry vni <vni> vrf <vrf> [leid <leid>] reid <reid>",
  .function = lisp_gpe_fwd_entry_show,
};
/* *INDENT-ON* */

clib_error_t *
lisp_gpe_fwd_entry_init (vlib_main_t * vm)
{
  lisp_gpe_main_t *lgm = &lisp_gpe_main;
  clib_error_t *error = NULL;

  if ((error = vlib_call_init_function (vm, lisp_cp_dpo_module_init)))
    return (error);

  l2_fib_init (lgm);

  fib_node_register_type (FIB_NODE_TYPE_LISP_GPE_FWD_ENTRY, &lisp_fwd_vft);

  return (error);
}

VLIB_INIT_FUNCTION (lisp_gpe_fwd_entry_init);

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
