/*
 * Copyright (c) 2015 Cisco and/or its affiliates.
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

#ifndef included_feature_registration_h
#define included_feature_registration_h

/** feature registration object */
typedef struct _vnet_feature_registration
{
  /** next registration in list of all registrations*/
  struct _vnet_feature_registration *next;
  /** Graph node name */
  char *node_name;
  /** Pointer to this feature index, filled in by vnet_feature_arc_init */
  u32 *feature_index;
  /** Constraints of the form "this feature runs before X" */
  char **runs_before;
  /** Constraints of the form "this feature runs after Y" */
  char **runs_after;
} vnet_feature_registration_t;

typedef struct ip_config_main_t_
{
  vnet_config_main_t config_main;
  u32 *config_index_by_sw_if_index;
} ip_config_main_t;

/** Syntactic sugar, the c-compiler won't initialize registrations without it */
#define ORDER_CONSTRAINTS (char*[])

clib_error_t *vnet_feature_arc_init (vlib_main_t * vm,
				     vnet_config_main_t * vcm,
				     char **feature_start_nodes,
				     int num_feature_start_nodes,
				     vnet_feature_registration_t *
				     first_reg, char ***feature_nodes);

void ip_interface_features_show (vlib_main_t * vm,
				 const char *pname,
				 ip_config_main_t * cm, u32 sw_if_index);

#endif /* included_feature_registration_h */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
