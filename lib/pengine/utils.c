/*
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include <crm_internal.h>
#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/util.h>

#include <glib.h>

#include <crm/pengine/rules.h>
#include <crm/pengine/internal.h>

pe_working_set_t *pe_dataset = NULL;

extern xmlNode *get_object_root(const char *object_type, xmlNode * the_root);
void print_str_str(gpointer key, gpointer value, gpointer user_data);
gboolean ghash_free_str_str(gpointer key, gpointer value, gpointer user_data);
void unpack_operation(action_t * action, xmlNode * xml_obj, resource_t * container,
                      pe_working_set_t * data_set);
static xmlNode *find_rsc_op_entry_helper(resource_t * rsc, const char *key,
                                         gboolean include_disabled);

/*!
 * \internal
 * \brief Check whether we can fence a particular node
 *
 * \param[in] data_set  Working set for cluster
 * \param[in] node      Name of node to check
 *
 * \return TRUE if node can be fenced, FALSE otherwise
 *
 * \note This function should only be called for cluster nodes and baremetal
 *       remote nodes; guest nodes are fenced by stopping their container
 *       resource, so fence execution requirements do not apply to them.
 */
bool pe_can_fence(pe_working_set_t * data_set, node_t *node)
{
    if(is_not_set(data_set->flags, pe_flag_stonith_enabled)) {
        return FALSE; /* Turned off */

    } else if (is_not_set(data_set->flags, pe_flag_have_stonith_resource)) {
        return FALSE; /* No devices */

    } else if (is_set(data_set->flags, pe_flag_have_quorum)) {
        return TRUE;

    } else if (data_set->no_quorum_policy == no_quorum_ignore) {
        return TRUE;

    } else if(node == NULL) {
        return FALSE;

    } else if(node->details->online) {
        crm_notice("We can fence %s without quorum because they're in our membership", node->details->uname);
        return TRUE;
    }

    crm_trace("Cannot fence %s", node->details->uname);
    return FALSE;
}

node_t *
node_copy(const node_t *this_node)
{
    node_t *new_node = NULL;

    CRM_CHECK(this_node != NULL, return NULL);

    new_node = calloc(1, sizeof(node_t));
    CRM_ASSERT(new_node != NULL);

    crm_trace("Copying %p (%s) to %p", this_node, this_node->details->uname, new_node);

    new_node->rsc_discover_mode = this_node->rsc_discover_mode;
    new_node->weight = this_node->weight;
    new_node->fixed = this_node->fixed;
    new_node->details = this_node->details;

    return new_node;
}

/* any node in list1 or list2 and not in the other gets a score of -INFINITY */
void
node_list_exclude(GHashTable * hash, GListPtr list, gboolean merge_scores)
{
    GHashTable *result = hash;
    node_t *other_node = NULL;
    GListPtr gIter = list;

    GHashTableIter iter;
    node_t *node = NULL;

    g_hash_table_iter_init(&iter, hash);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&node)) {

        other_node = pe_find_node_id(list, node->details->id);
        if (other_node == NULL) {
            node->weight = -INFINITY;
        } else if (merge_scores) {
            node->weight = merge_weights(node->weight, other_node->weight);
        }
    }

    for (; gIter != NULL; gIter = gIter->next) {
        node_t *node = (node_t *) gIter->data;

        other_node = pe_hash_table_lookup(result, node->details->id);

        if (other_node == NULL) {
            node_t *new_node = node_copy(node);

            new_node->weight = -INFINITY;
            g_hash_table_insert(result, (gpointer) new_node->details->id, new_node);
        }
    }
}

GHashTable *
node_hash_from_list(GListPtr list)
{
    GListPtr gIter = list;
    GHashTable *result = g_hash_table_new_full(crm_str_hash, g_str_equal, NULL, g_hash_destroy_str);

    for (; gIter != NULL; gIter = gIter->next) {
        node_t *node = (node_t *) gIter->data;
        node_t *n = node_copy(node);

        g_hash_table_insert(result, (gpointer) n->details->id, n);
    }

    return result;
}

GListPtr
node_list_dup(GListPtr list1, gboolean reset, gboolean filter)
{
    GListPtr result = NULL;
    GListPtr gIter = list1;

    for (; gIter != NULL; gIter = gIter->next) {
        node_t *new_node = NULL;
        node_t *this_node = (node_t *) gIter->data;

        if (filter && this_node->weight < 0) {
            continue;
        }

        new_node = node_copy(this_node);
        if (reset) {
            new_node->weight = 0;
        }
        if (new_node != NULL) {
            result = g_list_prepend(result, new_node);
        }
    }

    return result;
}

gint
sort_node_uname(gconstpointer a, gconstpointer b)
{
    const node_t *node_a = a;
    const node_t *node_b = b;

    return strcmp(node_a->details->uname, node_b->details->uname);
}

void
dump_node_scores_worker(int level, const char *file, const char *function, int line,
                        resource_t * rsc, const char *comment, GHashTable * nodes)
{
    GHashTable *hash = nodes;
    GHashTableIter iter;
    node_t *node = NULL;

    if (rsc) {
        hash = rsc->allowed_nodes;
    }

    if (rsc && is_set(rsc->flags, pe_rsc_orphan)) {
        /* Don't show the allocation scores for orphans */
        return;
    }

    if (level == 0) {
        char score[128];
        int len = sizeof(score);
        /* For now we want this in sorted order to keep the regression tests happy */
        GListPtr gIter = NULL;
        GListPtr list = g_hash_table_get_values(hash);

        list = g_list_sort(list, sort_node_uname);

        gIter = list;
        for (; gIter != NULL; gIter = gIter->next) {
            node_t *node = (node_t *) gIter->data;
            /* This function is called a whole lot, use stack allocated score */
            score2char_stack(node->weight, score, len);

            if (rsc) {
                printf("%s: %s allocation score on %s: %s\n",
                       comment, rsc->id, node->details->uname, score);
            } else {
                printf("%s: %s = %s\n", comment, node->details->uname, score);
            }
        }

        g_list_free(list);

    } else if (hash) {
        char score[128];
        int len = sizeof(score);
        g_hash_table_iter_init(&iter, hash);
        while (g_hash_table_iter_next(&iter, NULL, (void **)&node)) {
            /* This function is called a whole lot, use stack allocated score */
            score2char_stack(node->weight, score, len);

            if (rsc) {
                do_crm_log_alias(LOG_TRACE, file, function, line,
                                 "%s: %s allocation score on %s: %s", comment, rsc->id,
                                 node->details->uname, score);
            } else {
                do_crm_log_alias(LOG_TRACE, file, function, line + 1, "%s: %s = %s", comment,
                                 node->details->uname, score);
            }
        }
    }

    if (rsc && rsc->children) {
        GListPtr gIter = NULL;

        gIter = rsc->children;
        for (; gIter != NULL; gIter = gIter->next) {
            resource_t *child = (resource_t *) gIter->data;

            dump_node_scores_worker(level, file, function, line, child, comment, nodes);
        }
    }
}

static void
append_dump_text(gpointer key, gpointer value, gpointer user_data)
{
    char **dump_text = user_data;
    int len = 0;
    char *new_text = NULL;

    len = strlen(*dump_text) + strlen(" ") + strlen(key) + strlen("=") + strlen(value) + 1;
    new_text = calloc(1, len);
    sprintf(new_text, "%s %s=%s", *dump_text, (char *)key, (char *)value);

    free(*dump_text);
    *dump_text = new_text;
}

void
dump_node_capacity(int level, const char *comment, node_t * node)
{
    int len = 0;
    char *dump_text = NULL;

    len = strlen(comment) + strlen(": ") + strlen(node->details->uname) + strlen(" capacity:") + 1;
    dump_text = calloc(1, len);
    sprintf(dump_text, "%s: %s capacity:", comment, node->details->uname);

    g_hash_table_foreach(node->details->utilization, append_dump_text, &dump_text);

    if (level == 0) {
        fprintf(stdout, "%s\n", dump_text);
    } else {
        crm_trace("%s", dump_text);
    }

    free(dump_text);
}

void
dump_rsc_utilization(int level, const char *comment, resource_t * rsc, node_t * node)
{
    int len = 0;
    char *dump_text = NULL;

    len = strlen(comment) + strlen(": ") + strlen(rsc->id) + strlen(" utilization on ")
        + strlen(node->details->uname) + strlen(":") + 1;
    dump_text = calloc(1, len);
    sprintf(dump_text, "%s: %s utilization on %s:", comment, rsc->id, node->details->uname);

    g_hash_table_foreach(rsc->utilization, append_dump_text, &dump_text);

    if (level == 0) {
        fprintf(stdout, "%s\n", dump_text);
    } else {
        crm_trace("%s", dump_text);
    }

    free(dump_text);
}

gint
sort_rsc_index(gconstpointer a, gconstpointer b)
{
    const resource_t *resource1 = (const resource_t *)a;
    const resource_t *resource2 = (const resource_t *)b;

    if (a == NULL && b == NULL) {
        return 0;
    }
    if (a == NULL) {
        return 1;
    }
    if (b == NULL) {
        return -1;
    }

    if (resource1->sort_index > resource2->sort_index) {
        return -1;
    }

    if (resource1->sort_index < resource2->sort_index) {
        return 1;
    }

    return 0;
}

gint
sort_rsc_priority(gconstpointer a, gconstpointer b)
{
    const resource_t *resource1 = (const resource_t *)a;
    const resource_t *resource2 = (const resource_t *)b;

    if (a == NULL && b == NULL) {
        return 0;
    }
    if (a == NULL) {
        return 1;
    }
    if (b == NULL) {
        return -1;
    }

    if (resource1->priority > resource2->priority) {
        return -1;
    }

    if (resource1->priority < resource2->priority) {
        return 1;
    }

    return 0;
}

action_t *
custom_action(resource_t * rsc, char *key, const char *task,
              node_t * on_node, gboolean optional, gboolean save_action,
              pe_working_set_t * data_set)
{
    action_t *action = NULL;
    GListPtr possible_matches = NULL;

    CRM_CHECK(key != NULL, return NULL);
    CRM_CHECK(task != NULL, free(key); return NULL);

    if (save_action && rsc != NULL) {
        possible_matches = find_actions(rsc->actions, key, on_node);
    } else if(save_action) {
#if 0
        action = g_hash_table_lookup(data_set->singletons, key);
#else
        /* More expensive but takes 'node' into account */
        possible_matches = find_actions(data_set->actions, key, on_node);
#endif
    }

    if(data_set->singletons == NULL) {
        data_set->singletons = g_hash_table_new_full(crm_str_hash, g_str_equal, NULL, NULL);
    }

    if (possible_matches != NULL) {
        if (g_list_length(possible_matches) > 1) {
            pe_warn("Action %s for %s on %s exists %d times",
                    task, rsc ? rsc->id : "<NULL>",
                    on_node ? on_node->details->uname : "<NULL>", g_list_length(possible_matches));
        }

        action = g_list_nth_data(possible_matches, 0);
        pe_rsc_trace(rsc, "Found existing action (%d) %s for %s on %s",
                     action->id, task, rsc ? rsc->id : "<NULL>",
                     on_node ? on_node->details->uname : "<NULL>");
        g_list_free(possible_matches);
    }

    if (action == NULL) {
        if (save_action) {
            pe_rsc_trace(rsc, "Creating%s action %d: %s for %s on %s %d",
                         optional ? "" : " mandatory", data_set->action_id, key,
                         rsc ? rsc->id : "<NULL>", on_node ? on_node->details->uname : "<NULL>", optional);
        }

        action = calloc(1, sizeof(action_t));
        if (save_action) {
            action->id = data_set->action_id++;
        } else {
            action->id = 0;
        }
        action->rsc = rsc;
        CRM_ASSERT(task != NULL);
        action->task = strdup(task);
        if (on_node) {
            action->node = node_copy(on_node);
        }
        action->uuid = strdup(key);

        pe_set_action_bit(action, pe_action_runnable);
        if (optional) {
            pe_rsc_trace(rsc, "Set optional on %s", action->uuid);
            pe_set_action_bit(action, pe_action_optional);
        } else {
            pe_clear_action_bit(action, pe_action_optional);
            pe_rsc_trace(rsc, "Unset optional on %s", action->uuid);
        }

/*
  Implied by calloc()...
  action->actions_before   = NULL;
  action->actions_after    = NULL;

  action->pseudo     = FALSE;
  action->dumped     = FALSE;
  action->processed  = FALSE;
  action->seen_count = 0;
*/

        action->extra = g_hash_table_new_full(crm_str_hash, g_str_equal, free, free);

        action->meta = g_hash_table_new_full(crm_str_hash, g_str_equal, free, free);

        action->versioned_parameters = create_xml_node(NULL, XML_TAG_OP_VER_ATTRS);

        action->versioned_meta = create_xml_node(NULL, XML_TAG_OP_VER_META);

        if (save_action) {
            data_set->actions = g_list_prepend(data_set->actions, action);
            if(rsc == NULL) {
                g_hash_table_insert(data_set->singletons, action->uuid, action);
            }
        }

        if (rsc != NULL) {
            action->op_entry = find_rsc_op_entry_helper(rsc, key, TRUE);

            unpack_operation(action, action->op_entry, rsc->container, data_set);

            if (save_action) {
                rsc->actions = g_list_prepend(rsc->actions, action);
            }
        }

        if (save_action) {
            pe_rsc_trace(rsc, "Action %d created", action->id);
        }
    }

    if (optional == FALSE) {
        pe_rsc_trace(rsc, "Unset optional on %s", action->uuid);
        pe_clear_action_bit(action, pe_action_optional);
    }

    if (rsc != NULL) {
        enum action_tasks a_task = text2task(action->task);
        int warn_level = LOG_TRACE;

        if (save_action) {
            warn_level = LOG_WARNING;
        }

        if (is_set(action->flags, pe_action_have_node_attrs) == FALSE
            && action->node != NULL && action->op_entry != NULL) {
            pe_set_action_bit(action, pe_action_have_node_attrs);
            unpack_instance_attributes(data_set->input, action->op_entry, XML_TAG_ATTR_SETS,
                                       action->node->details->attrs,
                                       action->extra, NULL, FALSE, data_set->now);
        }

        if (is_set(action->flags, pe_action_pseudo)) {
            /* leave untouched */

        } else if (action->node == NULL) {
            pe_rsc_trace(rsc, "Unset runnable on %s", action->uuid);
            pe_clear_action_bit(action, pe_action_runnable);

        } else if (is_not_set(rsc->flags, pe_rsc_managed)
                   && g_hash_table_lookup(action->meta, XML_LRM_ATTR_INTERVAL) == NULL) {
            crm_debug("Action %s (unmanaged)", action->uuid);
            pe_rsc_trace(rsc, "Set optional on %s", action->uuid);
            pe_set_action_bit(action, pe_action_optional);
/*   			action->runnable = FALSE; */

        } else if (action->node->details->online == FALSE
                   && (!is_container_remote_node(action->node) || action->node->details->remote_requires_reset)) {
            pe_clear_action_bit(action, pe_action_runnable);
            do_crm_log(warn_level, "Action %s on %s is unrunnable (offline)",
                       action->uuid, action->node->details->uname);
            if (is_set(action->rsc->flags, pe_rsc_managed)
                && save_action && a_task == stop_rsc
                && action->node->details->unclean == FALSE) {
                pe_fence_node(data_set, action->node, "resource actions are unrunnable");
            }

        } else if (action->node->details->pending) {
            pe_clear_action_bit(action, pe_action_runnable);
            do_crm_log(warn_level, "Action %s on %s is unrunnable (pending)",
                       action->uuid, action->node->details->uname);

        } else if (action->needs == rsc_req_nothing) {
            pe_rsc_trace(rsc, "Action %s does not require anything", action->uuid);
            pe_set_action_bit(action, pe_action_runnable);
#if 0
            /*
             * No point checking this
             * - if we don't have quorum we can't stonith anyway
             */
        } else if (action->needs == rsc_req_stonith) {
            crm_trace("Action %s requires only stonith", action->uuid);
            action->runnable = TRUE;
#endif
        } else if (is_set(data_set->flags, pe_flag_have_quorum) == FALSE
                   && data_set->no_quorum_policy == no_quorum_stop) {
            pe_clear_action_bit(action, pe_action_runnable);
            crm_debug("%s\t%s (cancelled : quorum)", action->node->details->uname, action->uuid);

        } else if (is_set(data_set->flags, pe_flag_have_quorum) == FALSE
                   && data_set->no_quorum_policy == no_quorum_freeze) {
            pe_rsc_trace(rsc, "Check resource is already active: %s %s %s %s", rsc->id, action->uuid, role2text(rsc->next_role), role2text(rsc->role));
            if (rsc->fns->active(rsc, TRUE) == FALSE || rsc->next_role > rsc->role) {
                pe_clear_action_bit(action, pe_action_runnable);
                pe_rsc_debug(rsc, "%s\t%s (cancelled : quorum freeze)",
                             action->node->details->uname, action->uuid);
            }

        } else {
            pe_rsc_trace(rsc, "Action %s is runnable", action->uuid);
            pe_set_action_bit(action, pe_action_runnable);
        }

        if (save_action) {
            switch (a_task) {
                case stop_rsc:
                    set_bit(rsc->flags, pe_rsc_stopping);
                    break;
                case start_rsc:
                    clear_bit(rsc->flags, pe_rsc_starting);
                    if (is_set(action->flags, pe_action_runnable)) {
                        set_bit(rsc->flags, pe_rsc_starting);
                    }
                    break;
                default:
                    break;
            }
        }
    }

    free(key);
    return action;
}

static const char *
unpack_operation_on_fail(action_t * action)
{

    const char *value = g_hash_table_lookup(action->meta, XML_OP_ATTR_ON_FAIL);

    if (safe_str_eq(action->task, CRMD_ACTION_STOP) && safe_str_eq(value, "standby")) {
        crm_config_err("on-fail=standby is not allowed for stop actions: %s", action->rsc->id);
        return NULL;
    } else if (safe_str_eq(action->task, CRMD_ACTION_DEMOTE) && !value) {
        /* demote on_fail defaults to master monitor value if present */
        xmlNode *operation = NULL;
        const char *name = NULL;
        const char *role = NULL;
        const char *on_fail = NULL;
        const char *interval = NULL;
        const char *enabled = NULL;

        CRM_CHECK(action->rsc != NULL, return NULL);

        for (operation = __xml_first_child(action->rsc->ops_xml);
             operation && !value; operation = __xml_next_element(operation)) {

            if (!crm_str_eq((const char *)operation->name, "op", TRUE)) {
                continue;
            }
            name = crm_element_value(operation, "name");
            role = crm_element_value(operation, "role");
            on_fail = crm_element_value(operation, XML_OP_ATTR_ON_FAIL);
            enabled = crm_element_value(operation, "enabled");
            interval = crm_element_value(operation, XML_LRM_ATTR_INTERVAL);
            if (!on_fail) {
                continue;
            } else if (enabled && !crm_is_true(enabled)) {
                continue;
            } else if (safe_str_neq(name, "monitor") || safe_str_neq(role, "Master")) {
                continue;
            } else if (crm_get_interval(interval) <= 0) {
                continue;
            }

            value = on_fail;
        }
    }

    return value;
}

static xmlNode *
find_min_interval_mon(resource_t * rsc, gboolean include_disabled)
{
    int number = 0;
    int min_interval = -1;
    const char *name = NULL;
    const char *value = NULL;
    const char *interval = NULL;
    xmlNode *op = NULL;
    xmlNode *operation = NULL;

    for (operation = __xml_first_child(rsc->ops_xml); operation != NULL;
         operation = __xml_next_element(operation)) {

        if (crm_str_eq((const char *)operation->name, "op", TRUE)) {
            name = crm_element_value(operation, "name");
            interval = crm_element_value(operation, XML_LRM_ATTR_INTERVAL);
            value = crm_element_value(operation, "enabled");
            if (!include_disabled && value && crm_is_true(value) == FALSE) {
                continue;
            }

            if (safe_str_neq(name, RSC_STATUS)) {
                continue;
            }

            number = crm_get_interval(interval);
            if (number < 0) {
                continue;
            }

            if (min_interval < 0 || number < min_interval) {
                min_interval = number;
                op = operation;
            }
        }
    }

    return op;
}

static int
unpack_start_delay(const char *value, GHashTable *meta)
{
    int start_delay = 0;

    if (value != NULL) {
        start_delay = crm_get_msec(value);

        if (start_delay < 0) {
            start_delay = 0;
        }

        if (meta) {
            g_hash_table_replace(meta, strdup(XML_OP_ATTR_START_DELAY), crm_itoa(start_delay));
        }
    }

    return start_delay;
}

static int
unpack_interval_origin(const char *value, GHashTable *meta, xmlNode *xml_obj,
                       unsigned long long interval, crm_time_t *now)
{
    int start_delay = 0;

    if (interval > 0 && value) {
        crm_time_t *origin = crm_time_new(value);

        if (origin && now) {
            crm_time_t *delay = NULL;
            int rc = crm_time_compare(origin, now);
            long long delay_s = 0;
            int interval_s = (interval / 1000);

            crm_trace("Origin: %s, interval: %d", value, interval_s);

            /* If 'origin' is in the future, find the most recent "multiple" that occurred in the past */
            while(rc > 0) {
                crm_time_add_seconds(origin, -interval_s);
                rc = crm_time_compare(origin, now);
            }

            /* Now find the first "multiple" that occurs after 'now' */
            while (rc < 0) {
                crm_time_add_seconds(origin, interval_s);
                rc = crm_time_compare(origin, now);
            }

            delay = crm_time_calculate_duration(origin, now);

            crm_time_log(LOG_TRACE, "origin", origin,
                         crm_time_log_date | crm_time_log_timeofday |
                         crm_time_log_with_timezone);
            crm_time_log(LOG_TRACE, "now", now,
                         crm_time_log_date | crm_time_log_timeofday |
                         crm_time_log_with_timezone);
            crm_time_log(LOG_TRACE, "delay", delay, crm_time_log_duration);

            delay_s = crm_time_get_seconds(delay);

            CRM_CHECK(delay_s >= 0, delay_s = 0);
            start_delay = delay_s * 1000;

            if (xml_obj) {
                crm_info("Calculated a start delay of %llds for %s", delay_s, ID(xml_obj));
            }

            if (meta) {
                g_hash_table_replace(meta, strdup(XML_OP_ATTR_START_DELAY),
                                     crm_itoa(start_delay));
            }

            crm_time_free(origin);
            crm_time_free(delay);
        } else if (!origin && xml_obj) {
            crm_config_err("Operation %s contained an invalid " XML_OP_ATTR_ORIGIN ": %s",
                           ID(xml_obj), value);
        }
    }

    return start_delay;
}

static int
unpack_timeout(const char *value, action_t *action, xmlNode *xml_obj,
               unsigned long long interval, GHashTable *config_hash)
{
    int timeout = 0;

    if (value == NULL && xml_obj == NULL && action &&
        safe_str_eq(action->task, RSC_STATUS) && interval == 0) {

        xmlNode *min_interval_mon = find_min_interval_mon(action->rsc, FALSE);

        if (min_interval_mon) {
            value = crm_element_value(min_interval_mon, XML_ATTR_TIMEOUT);
            pe_rsc_trace(action->rsc,
                         "\t%s uses the timeout value '%s' from the minimum interval monitor",
                         action->uuid, value);
        }
    }

    if (value == NULL && config_hash) {
        value = pe_pref(config_hash, "default-action-timeout");
    }

    timeout = crm_get_msec(value);
    if (timeout < 0) {
        timeout = 0;
    }

    return timeout;
}

static void
unpack_versioned_meta(xmlNode *versioned_meta, xmlNode *xml_obj, unsigned long long interval, crm_time_t *now)
{
    xmlNode *attrs = NULL;
    xmlNode *attr = NULL;

    for (attrs = __xml_first_child(versioned_meta); attrs != NULL; attrs = __xml_next_element(attrs)) {
        for (attr = __xml_first_child(attrs); attr != NULL; attr = __xml_next_element(attr)) {
            const char *name = crm_element_value(attr, XML_NVPAIR_ATTR_NAME);
            const char *value = crm_element_value(attr, XML_NVPAIR_ATTR_VALUE);

            if (safe_str_eq(name, XML_OP_ATTR_START_DELAY)) {
                int start_delay = unpack_start_delay(value, NULL);

                crm_xml_add_int(attr, XML_NVPAIR_ATTR_VALUE, start_delay);
            } else if (safe_str_eq(name, XML_OP_ATTR_ORIGIN)) {
                int start_delay = unpack_interval_origin(value, NULL, xml_obj, interval, now);

                crm_xml_add(attr, XML_NVPAIR_ATTR_NAME, XML_OP_ATTR_START_DELAY);
                crm_xml_add_int(attr, XML_NVPAIR_ATTR_VALUE, start_delay);
            } else if (safe_str_eq(name, XML_ATTR_TIMEOUT)) {
                int timeout = unpack_timeout(value, NULL, NULL, 0, NULL);

                crm_xml_add_int(attr, XML_NVPAIR_ATTR_VALUE, timeout);
            }
        }
    }
}

void
unpack_operation(action_t * action, xmlNode * xml_obj, resource_t * container,
                 pe_working_set_t * data_set)
{
    unsigned long long interval = 0;
    int timeout = 0;
    char *value_ms = NULL;
    const char *value = NULL;
    const char *field = NULL;

    CRM_CHECK(action->rsc != NULL, return);

    unpack_instance_attributes(data_set->input, data_set->op_defaults, XML_TAG_META_SETS, NULL,
                               action->meta, NULL, FALSE, data_set->now);

    if (xml_obj) {
        xmlAttrPtr xIter = NULL;

        for (xIter = xml_obj->properties; xIter; xIter = xIter->next) {
            const char *prop_name = (const char *)xIter->name;
            const char *prop_value = crm_element_value(xml_obj, prop_name);

            g_hash_table_replace(action->meta, strdup(prop_name), strdup(prop_value));
        }
    }

    unpack_instance_attributes(data_set->input, xml_obj, XML_TAG_META_SETS,
                               NULL, action->meta, NULL, FALSE, data_set->now);

    unpack_instance_attributes(data_set->input, xml_obj, XML_TAG_ATTR_SETS,
                               NULL, action->meta, NULL, FALSE, data_set->now);

    pe_unpack_versioned_attributes(data_set->input, xml_obj, XML_TAG_ATTR_SETS, NULL,
                                   action->versioned_parameters, data_set->now);

    pe_unpack_versioned_attributes(data_set->input, xml_obj, XML_TAG_META_SETS, NULL,
                                   action->versioned_meta, data_set->now);

    g_hash_table_remove(action->meta, "id");

    field = XML_LRM_ATTR_INTERVAL;
    value = g_hash_table_lookup(action->meta, field);
    if (value != NULL) {
        interval = crm_get_interval(value);
        if (interval > 0) {
            value_ms = crm_itoa(interval);
            g_hash_table_replace(action->meta, strdup(field), value_ms);

        } else {
            g_hash_table_remove(action->meta, field);
        }
    }

    /* Begin compatibility code ("requires" set on start action not resource) */
    value = g_hash_table_lookup(action->meta, "requires");

    if (safe_str_neq(action->task, RSC_START)
        && safe_str_neq(action->task, RSC_PROMOTE)) {
        action->needs = rsc_req_nothing;
        value = "nothing (not start/promote)";

    } else if (safe_str_eq(value, "nothing")) {
        action->needs = rsc_req_nothing;

    } else if (safe_str_eq(value, "quorum")) {
        action->needs = rsc_req_quorum;

    } else if (safe_str_eq(value, "unfencing")) {
        action->needs = rsc_req_stonith;
        set_bit(action->rsc->flags, pe_rsc_needs_unfencing);
        if (is_not_set(data_set->flags, pe_flag_stonith_enabled)) {
            crm_notice("%s requires unfencing but fencing is disabled", action->rsc->id);
        }

    } else if (is_set(data_set->flags, pe_flag_stonith_enabled)
               && safe_str_eq(value, "fencing")) {
        action->needs = rsc_req_stonith;
        if (is_not_set(data_set->flags, pe_flag_stonith_enabled)) {
            crm_notice("%s requires fencing but fencing is disabled", action->rsc->id);
        }
        /* End compatibility code */

    } else if (is_set(action->rsc->flags, pe_rsc_needs_fencing)) {
        action->needs = rsc_req_stonith;
        value = "fencing (resource)";

    } else if (is_set(action->rsc->flags, pe_rsc_needs_quorum)) {
        action->needs = rsc_req_quorum;
        value = "quorum (resource)";

    } else {
        action->needs = rsc_req_nothing;
        value = "nothing (resource)";
    }

    pe_rsc_trace(action->rsc, "\tAction %s requires: %s", action->task, value);

    value = unpack_operation_on_fail(action);

    if (value == NULL) {

    } else if (safe_str_eq(value, "block")) {
        action->on_fail = action_fail_block;
        g_hash_table_insert(action->meta, strdup(XML_OP_ATTR_ON_FAIL), strdup("block"));

    } else if (safe_str_eq(value, "fence")) {
        action->on_fail = action_fail_fence;
        value = "node fencing";

        if (is_set(data_set->flags, pe_flag_stonith_enabled) == FALSE) {
            crm_config_err("Specifying on_fail=fence and" " stonith-enabled=false makes no sense");
            action->on_fail = action_fail_stop;
            action->fail_role = RSC_ROLE_STOPPED;
            value = "stop resource";
        }

    } else if (safe_str_eq(value, "standby")) {
        action->on_fail = action_fail_standby;
        value = "node standby";

    } else if (safe_str_eq(value, "ignore")
               || safe_str_eq(value, "nothing")) {
        action->on_fail = action_fail_ignore;
        value = "ignore";

    } else if (safe_str_eq(value, "migrate")) {
        action->on_fail = action_fail_migrate;
        value = "force migration";

    } else if (safe_str_eq(value, "stop")) {
        action->on_fail = action_fail_stop;
        action->fail_role = RSC_ROLE_STOPPED;
        value = "stop resource";

    } else if (safe_str_eq(value, "restart")) {
        action->on_fail = action_fail_recover;
        value = "restart (and possibly migrate)";

    } else if (safe_str_eq(value, "restart-container")) {
        if (container) {
            action->on_fail = action_fail_restart_container;
            value = "restart container (and possibly migrate)";

        } else {
            value = NULL;
        }

    } else {
        pe_err("Resource %s: Unknown failure type (%s)", action->rsc->id, value);
        value = NULL;
    }

    /* defaults */
    if (value == NULL && container) {
        action->on_fail = action_fail_restart_container;
        value = "restart container (and possibly migrate) (default)";

    /* for baremetal remote nodes, ensure that any failure that results in
     * dropping an active connection to a remote node results in fencing of
     * the remote node.
     *
     * There are only two action failures that don't result in fencing.
     * 1. probes - probe failures are expected.
     * 2. start - a start failure indicates that an active connection does not already
     * exist. The user can set op on-fail=fence if they really want to fence start
     * failures. */
    } else if (((value == NULL) || !is_set(action->rsc->flags, pe_rsc_managed)) &&
                (is_rsc_baremetal_remote_node(action->rsc, data_set) &&
               !(safe_str_eq(action->task, CRMD_ACTION_STATUS) && interval == 0) &&
                (safe_str_neq(action->task, CRMD_ACTION_START)))) {

        if (!is_set(action->rsc->flags, pe_rsc_managed)) {
            action->on_fail = action_fail_stop;
            action->fail_role = RSC_ROLE_STOPPED;
            value = "stop unmanaged baremetal remote node (enforcing default)";

        } else {
            if (is_set(data_set->flags, pe_flag_stonith_enabled)) {
                value = "fence baremetal remote node (default)";
            } else {
                value = "recover baremetal remote node connection (default)";
            }

            if (action->rsc->remote_reconnect_interval) {
                action->fail_role = RSC_ROLE_STOPPED;
            }
            action->on_fail = action_fail_reset_remote;
        }

    } else if (value == NULL && safe_str_eq(action->task, CRMD_ACTION_STOP)) {
        if (is_set(data_set->flags, pe_flag_stonith_enabled)) {
            action->on_fail = action_fail_fence;
            value = "resource fence (default)";

        } else {
            action->on_fail = action_fail_block;
            value = "resource block (default)";
        }

    } else if (value == NULL) {
        action->on_fail = action_fail_recover;
        value = "restart (and possibly migrate) (default)";
    }

    pe_rsc_trace(action->rsc, "\t%s failure handling: %s", action->task, value);

    value = NULL;
    if (xml_obj != NULL) {
        value = g_hash_table_lookup(action->meta, "role_after_failure");
    }
    if (value != NULL && action->fail_role == RSC_ROLE_UNKNOWN) {
        action->fail_role = text2role(value);
    }
    /* defaults */
    if (action->fail_role == RSC_ROLE_UNKNOWN) {
        if (safe_str_eq(action->task, CRMD_ACTION_PROMOTE)) {
            action->fail_role = RSC_ROLE_SLAVE;
        } else {
            action->fail_role = RSC_ROLE_STARTED;
        }
    }
    pe_rsc_trace(action->rsc, "\t%s failure results in: %s", action->task,
                 role2text(action->fail_role));

    field = XML_OP_ATTR_START_DELAY;
    value = g_hash_table_lookup(action->meta, XML_OP_ATTR_START_DELAY);
    if (value) {
        unpack_start_delay(value, action->meta);
    } else {
        value = g_hash_table_lookup(action->meta, XML_OP_ATTR_ORIGIN);
        unpack_interval_origin(value, action->meta, xml_obj, interval, data_set->now);
    }

    field = XML_ATTR_TIMEOUT;
    value = g_hash_table_lookup(action->meta, field);
    timeout = unpack_timeout(value, action, xml_obj, interval, data_set->config_hash);
    g_hash_table_replace(action->meta, strdup(XML_ATTR_TIMEOUT), crm_itoa(timeout));

    unpack_versioned_meta(action->versioned_meta, xml_obj, interval, data_set->now);
}

static xmlNode *
find_rsc_op_entry_helper(resource_t * rsc, const char *key, gboolean include_disabled)
{
    unsigned long long number = 0;
    gboolean do_retry = TRUE;
    char *local_key = NULL;
    const char *name = NULL;
    const char *value = NULL;
    const char *interval = NULL;
    char *match_key = NULL;
    xmlNode *op = NULL;
    xmlNode *operation = NULL;

  retry:
    for (operation = __xml_first_child(rsc->ops_xml); operation != NULL;
         operation = __xml_next_element(operation)) {
        if (crm_str_eq((const char *)operation->name, "op", TRUE)) {
            name = crm_element_value(operation, "name");
            interval = crm_element_value(operation, XML_LRM_ATTR_INTERVAL);
            value = crm_element_value(operation, "enabled");
            if (!include_disabled && value && crm_is_true(value) == FALSE) {
                continue;
            }

            number = crm_get_interval(interval);
            match_key = generate_op_key(rsc->id, name, number);
            if (safe_str_eq(key, match_key)) {
                op = operation;
            }
            free(match_key);

            if (rsc->clone_name) {
                match_key = generate_op_key(rsc->clone_name, name, number);
                if (safe_str_eq(key, match_key)) {
                    op = operation;
                }
                free(match_key);
            }

            if (op != NULL) {
                free(local_key);
                return op;
            }
        }
    }

    free(local_key);
    if (do_retry == FALSE) {
        return NULL;
    }

    do_retry = FALSE;
    if (strstr(key, CRMD_ACTION_MIGRATE) || strstr(key, CRMD_ACTION_MIGRATED)) {
        local_key = generate_op_key(rsc->id, "migrate", 0);
        key = local_key;
        goto retry;

    } else if (strstr(key, "_notify_")) {
        local_key = generate_op_key(rsc->id, "notify", 0);
        key = local_key;
        goto retry;
    }

    return NULL;
}

xmlNode *
find_rsc_op_entry(resource_t * rsc, const char *key)
{
    return find_rsc_op_entry_helper(rsc, key, FALSE);
}

void
print_node(const char *pre_text, node_t * node, gboolean details)
{
    if (node == NULL) {
        crm_trace("%s%s: <NULL>", pre_text == NULL ? "" : pre_text, pre_text == NULL ? "" : ": ");
        return;
    }

    CRM_ASSERT(node->details);
    crm_trace("%s%s%sNode %s: (weight=%d, fixed=%s)",
              pre_text == NULL ? "" : pre_text,
              pre_text == NULL ? "" : ": ",
              node->details->online ? "" : "Unavailable/Unclean ",
              node->details->uname, node->weight, node->fixed ? "True" : "False");

    if (details) {
        char *pe_mutable = strdup("\t\t");
        GListPtr gIter = node->details->running_rsc;

        crm_trace("\t\t===Node Attributes");
        g_hash_table_foreach(node->details->attrs, print_str_str, pe_mutable);
        free(pe_mutable);

        crm_trace("\t\t=== Resources");

        for (; gIter != NULL; gIter = gIter->next) {
            resource_t *rsc = (resource_t *) gIter->data;

            print_resource(LOG_DEBUG_4, "\t\t", rsc, FALSE);
        }
    }
}

/*
 * Used by the HashTable for-loop
 */
void
print_str_str(gpointer key, gpointer value, gpointer user_data)
{
    crm_trace("%s%s %s ==> %s",
              user_data == NULL ? "" : (char *)user_data,
              user_data == NULL ? "" : ": ", (char *)key, (char *)value);
}

void
print_resource(int log_level, const char *pre_text, resource_t * rsc, gboolean details)
{
    long options = pe_print_log | pe_print_pending;

    if (rsc == NULL) {
        do_crm_log(log_level - 1, "%s%s: <NULL>",
                   pre_text == NULL ? "" : pre_text, pre_text == NULL ? "" : ": ");
        return;
    }
    if (details) {
        options |= pe_print_details;
    }
    rsc->fns->print(rsc, pre_text, options, &log_level);
}

void
pe_free_action(action_t * action)
{
    if (action == NULL) {
        return;
    }
    g_list_free_full(action->actions_before, free);     /* action_wrapper_t* */
    g_list_free_full(action->actions_after, free);      /* action_wrapper_t* */
    if (action->extra) {
        g_hash_table_destroy(action->extra);
    }
    if (action->meta) {
        g_hash_table_destroy(action->meta);
    }
    if (action->versioned_parameters) {
        free_xml(action->versioned_parameters);
    }
    if (action->versioned_meta) {
        free_xml(action->versioned_meta);
    }
    free(action->cancel_task);
    free(action->task);
    free(action->uuid);
    free(action->node);
    free(action);
}

GListPtr
find_recurring_actions(GListPtr input, node_t * not_on_node)
{
    const char *value = NULL;
    GListPtr result = NULL;
    GListPtr gIter = input;

    CRM_CHECK(input != NULL, return NULL);

    for (; gIter != NULL; gIter = gIter->next) {
        action_t *action = (action_t *) gIter->data;

        value = g_hash_table_lookup(action->meta, XML_LRM_ATTR_INTERVAL);
        if (value == NULL) {
            /* skip */
        } else if (safe_str_eq(value, "0")) {
            /* skip */
        } else if (safe_str_eq(CRMD_ACTION_CANCEL, action->task)) {
            /* skip */
        } else if (not_on_node == NULL) {
            crm_trace("(null) Found: %s", action->uuid);
            result = g_list_prepend(result, action);

        } else if (action->node == NULL) {
            /* skip */
        } else if (action->node->details != not_on_node->details) {
            crm_trace("Found: %s", action->uuid);
            result = g_list_prepend(result, action);
        }
    }

    return result;
}

enum action_tasks
get_complex_task(resource_t * rsc, const char *name, gboolean allow_non_atomic)
{
    enum action_tasks task = text2task(name);

    if (rsc == NULL) {
        return task;

    } else if (allow_non_atomic == FALSE || rsc->variant == pe_native) {
        switch (task) {
            case stopped_rsc:
            case started_rsc:
            case action_demoted:
            case action_promoted:
                crm_trace("Folding %s back into its atomic counterpart for %s", name, rsc->id);
                return task - 1;
                break;
            default:
                break;
        }
    }
    return task;
}

action_t *
find_first_action(GListPtr input, const char *uuid, const char *task, node_t * on_node)
{
    GListPtr gIter = NULL;

    CRM_CHECK(uuid || task, return NULL);

    for (gIter = input; gIter != NULL; gIter = gIter->next) {
        action_t *action = (action_t *) gIter->data;

        if (uuid != NULL && safe_str_neq(uuid, action->uuid)) {
            continue;

        } else if (task != NULL && safe_str_neq(task, action->task)) {
            continue;

        } else if (on_node == NULL) {
            return action;

        } else if (action->node == NULL) {
            continue;

        } else if (on_node->details == action->node->details) {
            return action;
        }
    }

    return NULL;
}

GListPtr
find_actions(GListPtr input, const char *key, const node_t *on_node)
{
    GListPtr gIter = input;
    GListPtr result = NULL;

    CRM_CHECK(key != NULL, return NULL);

    for (; gIter != NULL; gIter = gIter->next) {
        action_t *action = (action_t *) gIter->data;

        if (safe_str_neq(key, action->uuid)) {
            crm_trace("%s does not match action %s", key, action->uuid);
            continue;

        } else if (on_node == NULL) {
            crm_trace("Action %s matches (ignoring node)", key);
            result = g_list_prepend(result, action);

        } else if (action->node == NULL) {
            crm_trace("Action %s matches (unallocated, assigning to %s)",
                      key, on_node->details->uname);

            action->node = node_copy(on_node);
            result = g_list_prepend(result, action);

        } else if (on_node->details == action->node->details) {
            crm_trace("Action %s on %s matches", key, on_node->details->uname);
            result = g_list_prepend(result, action);

        } else {
            crm_trace("Action %s on node %s does not match requested node %s",
                      key, action->node->details->uname,
                      on_node->details->uname);
        }
    }

    return result;
}

GListPtr
find_actions_exact(GListPtr input, const char *key, node_t * on_node)
{
    GListPtr gIter = input;
    GListPtr result = NULL;

    CRM_CHECK(key != NULL, return NULL);

    for (; gIter != NULL; gIter = gIter->next) {
        action_t *action = (action_t *) gIter->data;

        crm_trace("Matching %s against %s", key, action->uuid);
        if (safe_str_neq(key, action->uuid)) {
            crm_trace("Key mismatch: %s vs. %s", key, action->uuid);
            continue;

        } else if (on_node == NULL || action->node == NULL) {
            crm_trace("on_node=%p, action->node=%p", on_node, action->node);
            continue;

        } else if (safe_str_eq(on_node->details->id, action->node->details->id)) {
            result = g_list_prepend(result, action);
        }
        crm_trace("Node mismatch: %s vs. %s", on_node->details->id, action->node->details->id);
    }

    return result;
}

static void
resource_node_score(resource_t * rsc, node_t * node, int score, const char *tag)
{
    node_t *match = NULL;

    if (rsc->children) {
        GListPtr gIter = rsc->children;

        for (; gIter != NULL; gIter = gIter->next) {
            resource_t *child_rsc = (resource_t *) gIter->data;

            resource_node_score(child_rsc, node, score, tag);
        }
    }

    pe_rsc_trace(rsc, "Setting %s for %s on %s: %d", tag, rsc->id, node->details->uname, score);
    match = pe_hash_table_lookup(rsc->allowed_nodes, node->details->id);
    if (match == NULL) {
        match = node_copy(node);
        g_hash_table_insert(rsc->allowed_nodes, (gpointer) match->details->id, match);
    }
    match->weight = merge_weights(match->weight, score);
}

void
resource_location(resource_t * rsc, node_t * node, int score, const char *tag,
                  pe_working_set_t * data_set)
{
    if (node != NULL) {
        resource_node_score(rsc, node, score, tag);

    } else if (data_set != NULL) {
        GListPtr gIter = data_set->nodes;

        for (; gIter != NULL; gIter = gIter->next) {
            node_t *node_iter = (node_t *) gIter->data;

            resource_node_score(rsc, node_iter, score, tag);
        }

    } else {
        GHashTableIter iter;
        node_t *node_iter = NULL;

        g_hash_table_iter_init(&iter, rsc->allowed_nodes);
        while (g_hash_table_iter_next(&iter, NULL, (void **)&node_iter)) {
            resource_node_score(rsc, node_iter, score, tag);
        }
    }

    if (node == NULL && score == -INFINITY) {
        if (rsc->allocated_to) {
            crm_info("Deallocating %s from %s", rsc->id, rsc->allocated_to->details->uname);
            free(rsc->allocated_to);
            rsc->allocated_to = NULL;
        }
    }
}

#define sort_return(an_int, why) do {					\
	free(a_uuid);						\
	free(b_uuid);						\
	crm_trace("%s (%d) %c %s (%d) : %s",				\
		  a_xml_id, a_call_id, an_int>0?'>':an_int<0?'<':'=',	\
		  b_xml_id, b_call_id, why);				\
	return an_int;							\
    } while(0)

gint
sort_op_by_callid(gconstpointer a, gconstpointer b)
{
    int a_call_id = -1;
    int b_call_id = -1;

    char *a_uuid = NULL;
    char *b_uuid = NULL;

    const xmlNode *xml_a = a;
    const xmlNode *xml_b = b;

    const char *a_xml_id = crm_element_value_const(xml_a, XML_ATTR_ID);
    const char *b_xml_id = crm_element_value_const(xml_b, XML_ATTR_ID);

    if (safe_str_eq(a_xml_id, b_xml_id)) {
        /* We have duplicate lrm_rsc_op entries in the status
         *    section which is unliklely to be a good thing
         *    - we can handle it easily enough, but we need to get
         *    to the bottom of why it's happening.
         */
        pe_err("Duplicate lrm_rsc_op entries named %s", a_xml_id);
        sort_return(0, "duplicate");
    }

    crm_element_value_const_int(xml_a, XML_LRM_ATTR_CALLID, &a_call_id);
    crm_element_value_const_int(xml_b, XML_LRM_ATTR_CALLID, &b_call_id);

    if (a_call_id == -1 && b_call_id == -1) {
        /* both are pending ops so it doesn't matter since
         *   stops are never pending
         */
        sort_return(0, "pending");

    } else if (a_call_id >= 0 && a_call_id < b_call_id) {
        sort_return(-1, "call id");

    } else if (b_call_id >= 0 && a_call_id > b_call_id) {
        sort_return(1, "call id");

    } else if (b_call_id >= 0 && a_call_id == b_call_id) {
        /*
         * The op and last_failed_op are the same
         * Order on last-rc-change
         */
        int last_a = -1;
        int last_b = -1;

        crm_element_value_const_int(xml_a, XML_RSC_OP_LAST_CHANGE, &last_a);
        crm_element_value_const_int(xml_b, XML_RSC_OP_LAST_CHANGE, &last_b);

        crm_trace("rc-change: %d vs %d", last_a, last_b);
        if (last_a >= 0 && last_a < last_b) {
            sort_return(-1, "rc-change");

        } else if (last_b >= 0 && last_a > last_b) {
            sort_return(1, "rc-change");
        }
        sort_return(0, "rc-change");

    } else {
        /* One of the inputs is a pending operation
         * Attempt to use XML_ATTR_TRANSITION_MAGIC to determine its age relative to the other
         */

        int a_id = -1;
        int b_id = -1;
        int dummy = -1;

        const char *a_magic = crm_element_value_const(xml_a, XML_ATTR_TRANSITION_MAGIC);
        const char *b_magic = crm_element_value_const(xml_b, XML_ATTR_TRANSITION_MAGIC);

        CRM_CHECK(a_magic != NULL && b_magic != NULL, sort_return(0, "No magic"));
        if(!decode_transition_magic(a_magic, &a_uuid, &a_id, &dummy, &dummy, &dummy, &dummy)) {
            sort_return(0, "bad magic a");
        }
        if(!decode_transition_magic(b_magic, &b_uuid, &b_id, &dummy, &dummy, &dummy, &dummy)) {
            sort_return(0, "bad magic b");
        }
        /* try to determine the relative age of the operation...
         * some pending operations (ie. a start) may have been superseded
         *   by a subsequent stop
         *
         * [a|b]_id == -1 means it's a shutdown operation and _always_ comes last
         */
        if (safe_str_neq(a_uuid, b_uuid) || a_id == b_id) {
            /*
             * some of the logic in here may be redundant...
             *
             * if the UUID from the TE doesn't match then one better
             *   be a pending operation.
             * pending operations don't survive between elections and joins
             *   because we query the LRM directly
             */

            if (b_call_id == -1) {
                sort_return(-1, "transition + call");

            } else if (a_call_id == -1) {
                sort_return(1, "transition + call");
            }

        } else if ((a_id >= 0 && a_id < b_id) || b_id == -1) {
            sort_return(-1, "transition");

        } else if ((b_id >= 0 && a_id > b_id) || a_id == -1) {
            sort_return(1, "transition");
        }
    }

    /* we should never end up here */
    CRM_CHECK(FALSE, sort_return(0, "default"));

}

time_t
get_effective_time(pe_working_set_t * data_set)
{
    if(data_set) {
        if (data_set->now == NULL) {
            crm_trace("Recording a new 'now'");
            data_set->now = crm_time_new(NULL);
        }
        return crm_time_get_seconds_since_epoch(data_set->now);
    }

    crm_trace("Defaulting to 'now'");
    return time(NULL);
}

gboolean
get_target_role(resource_t * rsc, enum rsc_role_e * role)
{
    enum rsc_role_e local_role = RSC_ROLE_UNKNOWN;
    const char *value = g_hash_table_lookup(rsc->meta, XML_RSC_ATTR_TARGET_ROLE);

    CRM_CHECK(role != NULL, return FALSE);

    if (value == NULL || safe_str_eq("started", value)
        || safe_str_eq("default", value)) {
        return FALSE;
    }

    local_role = text2role(value);
    if (local_role == RSC_ROLE_UNKNOWN) {
        crm_config_err("%s: Unknown value for %s: %s", rsc->id, XML_RSC_ATTR_TARGET_ROLE, value);
        return FALSE;

    } else if (local_role > RSC_ROLE_STARTED) {
        if (uber_parent(rsc)->variant == pe_master) {
            if (local_role > RSC_ROLE_SLAVE) {
                /* This is what we'd do anyway, just leave the default to avoid messing up the placement algorithm */
                return FALSE;
            }

        } else {
            crm_config_err("%s is not part of a master/slave resource, a %s of '%s' makes no sense",
                           rsc->id, XML_RSC_ATTR_TARGET_ROLE, value);
            return FALSE;
        }
    }

    *role = local_role;
    return TRUE;
}

gboolean
order_actions(action_t * lh_action, action_t * rh_action, enum pe_ordering order)
{
    GListPtr gIter = NULL;
    action_wrapper_t *wrapper = NULL;
    GListPtr list = NULL;

    if (order == pe_order_none) {
        return FALSE;
    }

    if (lh_action == NULL || rh_action == NULL) {
        return FALSE;
    }

    crm_trace("Ordering Action %s before %s", lh_action->uuid, rh_action->uuid);

    /* Ensure we never create a dependency on ourselves... it's happened */
    CRM_ASSERT(lh_action != rh_action);

    /* Filter dups, otherwise update_action_states() has too much work to do */
    gIter = lh_action->actions_after;
    for (; gIter != NULL; gIter = gIter->next) {
        action_wrapper_t *after = (action_wrapper_t *) gIter->data;

        if (after->action == rh_action && (after->type & order)) {
            return FALSE;
        }
    }

    wrapper = calloc(1, sizeof(action_wrapper_t));
    wrapper->action = rh_action;
    wrapper->type = order;

    list = lh_action->actions_after;
    list = g_list_prepend(list, wrapper);
    lh_action->actions_after = list;

    wrapper = NULL;

/* 	order |= pe_order_implies_then; */
/* 	order ^= pe_order_implies_then; */

    wrapper = calloc(1, sizeof(action_wrapper_t));
    wrapper->action = lh_action;
    wrapper->type = order;
    list = rh_action->actions_before;
    list = g_list_prepend(list, wrapper);
    rh_action->actions_before = list;
    return TRUE;
}

action_t *
get_pseudo_op(const char *name, pe_working_set_t * data_set)
{
    action_t *op = NULL;

    if(data_set->singletons) {
        op = g_hash_table_lookup(data_set->singletons, name);
    }
    if (op == NULL) {
        op = custom_action(NULL, strdup(name), name, NULL, TRUE, TRUE, data_set);
        set_bit(op->flags, pe_action_pseudo);
        set_bit(op->flags, pe_action_runnable);
    }

    return op;
}

void
destroy_ticket(gpointer data)
{
    ticket_t *ticket = data;

    if (ticket->state) {
        g_hash_table_destroy(ticket->state);
    }
    free(ticket->id);
    free(ticket);
}

ticket_t *
ticket_new(const char *ticket_id, pe_working_set_t * data_set)
{
    ticket_t *ticket = NULL;

    if (ticket_id == NULL || strlen(ticket_id) == 0) {
        return NULL;
    }

    if (data_set->tickets == NULL) {
        data_set->tickets =
            g_hash_table_new_full(crm_str_hash, g_str_equal, g_hash_destroy_str, destroy_ticket);
    }

    ticket = g_hash_table_lookup(data_set->tickets, ticket_id);
    if (ticket == NULL) {

        ticket = calloc(1, sizeof(ticket_t));
        if (ticket == NULL) {
            crm_err("Cannot allocate ticket '%s'", ticket_id);
            return NULL;
        }

        crm_trace("Creaing ticket entry for %s", ticket_id);

        ticket->id = strdup(ticket_id);
        ticket->granted = FALSE;
        ticket->last_granted = -1;
        ticket->standby = FALSE;
        ticket->state = g_hash_table_new_full(crm_str_hash, g_str_equal,
                                              g_hash_destroy_str, g_hash_destroy_str);

        g_hash_table_insert(data_set->tickets, strdup(ticket->id), ticket);
    }

    return ticket;
}

static void
filter_parameters(xmlNode * param_set, const char *param_string, bool need_present)
{
    int len = 0;
    char *name = NULL;
    char *match = NULL;

    if (param_set == NULL) {
        return;
    }

    if (param_set) {
        xmlAttrPtr xIter = param_set->properties;

        while (xIter) {
            const char *prop_name = (const char *)xIter->name;

            xIter = xIter->next;
            name = NULL;
            len = strlen(prop_name) + 3;

            name = malloc(len);
            if(name) {
                sprintf(name, " %s ", prop_name);
                name[len - 1] = 0;
                match = strstr(param_string, name);
            }

            if (need_present && match == NULL) {
                crm_trace("%s not found in %s", prop_name, param_string);
                xml_remove_prop(param_set, prop_name);

            } else if (need_present == FALSE && match) {
                crm_trace("%s found in %s", prop_name, param_string);
                xml_remove_prop(param_set, prop_name);
            }
            free(name);
        }
    }
}

bool fix_remote_addr(resource_t * rsc)
{
    const char *name;
    const char *value;
    const char *attr_list[] = {
        XML_ATTR_TYPE,
        XML_AGENT_ATTR_CLASS,
        XML_AGENT_ATTR_PROVIDER
    };
    const char *value_list[] = {
        "remote",
        "ocf",
        "pacemaker"
    };

    if(rsc == NULL) {
        return FALSE;
    }

    name = "addr";
    value = g_hash_table_lookup(rsc->parameters, name);
    if (safe_str_eq(value, "#uname") == FALSE) {
        return FALSE;
    }

    for (int lpc = 0; lpc < DIMOF(attr_list); lpc++) {
        name = attr_list[lpc];
        value = crm_element_value(rsc->xml, attr_list[lpc]);
        if (safe_str_eq(value, value_list[lpc]) == FALSE) {
            return FALSE;
        }
    }

    return TRUE;
}

static void
append_versioned_params(xmlNode *versioned_params, const char *ra_version, xmlNode *params)
{
    GHashTable *hash = pe_unpack_versioned_parameters(versioned_params, ra_version);
    char *key = NULL;
    char *value = NULL;
    GHashTableIter iter;

    g_hash_table_iter_init(&iter, hash);
    while (g_hash_table_iter_next(&iter, (gpointer *) &key, (gpointer *) &value)) {
        crm_xml_add(params, key, value);
    }
    g_hash_table_destroy(hash);
}

op_digest_cache_t *
rsc_action_digest_cmp(resource_t * rsc, xmlNode * xml_op, node_t * node,
                      pe_working_set_t * data_set)
{
    op_digest_cache_t *data = NULL;

    GHashTable *local_rsc_params = NULL;
    xmlNode *local_versioned_params = NULL;

    action_t *action = NULL;
    char *key = NULL;

    int interval = 0;
    const char *op_id = ID(xml_op);
    const char *interval_s = crm_element_value(xml_op, XML_LRM_ATTR_INTERVAL);
    const char *task = crm_element_value(xml_op, XML_LRM_ATTR_TASK);
    const char *digest_all;
    const char *digest_restart;
    const char *secure_list;
    const char *restart_list;
    const char *op_version;
    const char *ra_version;

    CRM_ASSERT(node != NULL);

    data = g_hash_table_lookup(node->details->digest_cache, op_id);
    if (data) {
        return data;
    }

    data = calloc(1, sizeof(op_digest_cache_t));
    CRM_ASSERT(data != NULL);

    digest_all = crm_element_value(xml_op, XML_LRM_ATTR_OP_DIGEST);
    digest_restart = crm_element_value(xml_op, XML_LRM_ATTR_RESTART_DIGEST);

    secure_list = crm_element_value(xml_op, XML_LRM_ATTR_OP_SECURE);
    restart_list = crm_element_value(xml_op, XML_LRM_ATTR_OP_RESTART);

    op_version = crm_element_value(xml_op, XML_ATTR_CRM_VERSION);
    ra_version = crm_element_value(xml_op, XML_ATTR_RA_VERSION);

    /* key is freed in custom_action */
    interval = crm_parse_int(interval_s, "0");
    key = generate_op_key(rsc->id, task, interval);
    action = custom_action(rsc, key, task, node, TRUE, FALSE, data_set);
    key = NULL;

    local_rsc_params = g_hash_table_new_full(crm_str_hash, g_str_equal,
                                             g_hash_destroy_str, g_hash_destroy_str);
    get_rsc_attributes(local_rsc_params, rsc, node, data_set);
    local_versioned_params = create_xml_node(NULL, XML_TAG_RSC_VER_ATTRS);
    pe_get_versioned_attributes(local_versioned_params, rsc, node, data_set);
    data->params_all = create_xml_node(NULL, XML_TAG_PARAMS);

    if(fix_remote_addr(rsc) && node) {
        // REMOTE_CONTAINER_HACK: Allow remote nodes that start containers with pacemaker remote inside
        crm_xml_add(data->params_all, "addr", node->details->uname);
        crm_trace("Fixing addr for %s on %s", rsc->id, node->details->uname);
    }

    g_hash_table_foreach(local_rsc_params, hash2field, data->params_all);
    g_hash_table_foreach(action->extra, hash2field, data->params_all);
    g_hash_table_foreach(rsc->parameters, hash2field, data->params_all);
    g_hash_table_foreach(action->meta, hash2metafield, data->params_all);
    append_versioned_params(local_versioned_params, ra_version, data->params_all);
    append_versioned_params(rsc->versioned_parameters, ra_version, data->params_all);
    append_versioned_params(action->versioned_parameters, ra_version, data->params_all);
    filter_action_parameters(data->params_all, op_version);

    data->digest_all_calc = calculate_operation_digest(data->params_all, op_version);

    if (secure_list && is_set(data_set->flags, pe_flag_sanitized)) {
        data->params_secure = copy_xml(data->params_all);

        if (secure_list) {
            filter_parameters(data->params_secure, secure_list, FALSE);
        }
        data->digest_secure_calc = calculate_operation_digest(data->params_secure, op_version);
    }

    if (digest_restart) {
        data->params_restart = copy_xml(data->params_all);

        if (restart_list) {
            filter_parameters(data->params_restart, restart_list, TRUE);
        }
        data->digest_restart_calc = calculate_operation_digest(data->params_restart, op_version);
    }

    data->rc = RSC_DIGEST_MATCH;
    if (digest_restart && strcmp(data->digest_restart_calc, digest_restart) != 0) {
        data->rc = RSC_DIGEST_RESTART;

    } else if (digest_all == NULL) {
        /* it is unknown what the previous op digest was */
        data->rc = RSC_DIGEST_UNKNOWN;

    } else if (strcmp(digest_all, data->digest_all_calc) != 0) {
        data->rc = RSC_DIGEST_ALL;
    }

    g_hash_table_insert(node->details->digest_cache, strdup(op_id), data);
    g_hash_table_destroy(local_rsc_params);
    free_xml(local_versioned_params);
    pe_free_action(action);

    return data;
}

const char *rsc_printable_id(resource_t *rsc)
{
    if (is_not_set(rsc->flags, pe_rsc_unique)) {
        return ID(rsc->xml);
    }
    return rsc->id;
}

void
clear_bit_recursive(resource_t * rsc, unsigned long long flag)
{
    GListPtr gIter = rsc->children;

    clear_bit(rsc->flags, flag);
    for (; gIter != NULL; gIter = gIter->next) {
        resource_t *child_rsc = (resource_t *) gIter->data;

        clear_bit_recursive(child_rsc, flag);
    }
}

void
set_bit_recursive(resource_t * rsc, unsigned long long flag)
{
    GListPtr gIter = rsc->children;

    set_bit(rsc->flags, flag);
    for (; gIter != NULL; gIter = gIter->next) {
        resource_t *child_rsc = (resource_t *) gIter->data;

        set_bit_recursive(child_rsc, flag);
    }
}

action_t *
pe_fence_op(node_t * node, const char *op, bool optional, pe_working_set_t * data_set)
{
    char *key = NULL;
    action_t *stonith_op = NULL;

    if(op == NULL) {
        op = data_set->stonith_action;
    }

    key = crm_strdup_printf("%s-%s-%s", CRM_OP_FENCE, node->details->uname, op);

    if(data_set->singletons) {
        stonith_op = g_hash_table_lookup(data_set->singletons, key);
    }

    if(stonith_op == NULL) {
        stonith_op = custom_action(NULL, key, CRM_OP_FENCE, node, optional, TRUE, data_set);

        add_hash_param(stonith_op->meta, XML_LRM_ATTR_TARGET, node->details->uname);
        add_hash_param(stonith_op->meta, XML_LRM_ATTR_TARGET_UUID, node->details->id);
        add_hash_param(stonith_op->meta, "stonith_action", op);
    } else {
        free(key);
    }

    if(optional == FALSE) {
        crm_trace("%s is no longer optional", stonith_op->uuid);
        pe_clear_action_bit(stonith_op, pe_action_optional);
    }

    return stonith_op;
}

void
trigger_unfencing(
    resource_t * rsc, node_t *node, const char *reason, action_t *dependency, pe_working_set_t * data_set) 
{
    if(is_not_set(data_set->flags, pe_flag_enable_unfencing)) {
        /* No resources require it */
        return;

    } else if (rsc != NULL && is_not_set(rsc->flags, pe_rsc_fence_device)) {
        /* Wasn't a stonith device */
        return;

    } else if(node
              && node->details->online
              && node->details->unclean == FALSE
              && node->details->shutdown == FALSE) {
        action_t *unfence = pe_fence_op(node, "on", FALSE, data_set);

        crm_notice("Unfencing %s: %s", node->details->uname, reason);
        if(dependency) {
            order_actions(unfence, dependency, pe_order_optional);
        }

    } else if(rsc) {
        GHashTableIter iter;

        g_hash_table_iter_init(&iter, rsc->allowed_nodes);
        while (g_hash_table_iter_next(&iter, NULL, (void **)&node)) {
            if(node->details->online && node->details->unclean == FALSE && node->details->shutdown == FALSE) {
                trigger_unfencing(rsc, node, reason, dependency, data_set);
            }
        }
    }
}

gboolean
add_tag_ref(GHashTable * tags, const char * tag_name,  const char * obj_ref)
{
    tag_t *tag = NULL;
    GListPtr gIter = NULL;
    gboolean is_existing = FALSE;

    CRM_CHECK(tags && tag_name && obj_ref, return FALSE);

    tag = g_hash_table_lookup(tags, tag_name);
    if (tag == NULL) {
        tag = calloc(1, sizeof(tag_t));
        if (tag == NULL) {
            return FALSE;
        }
        tag->id = strdup(tag_name);
        tag->refs = NULL;
        g_hash_table_insert(tags, strdup(tag_name), tag);
    }

    for (gIter = tag->refs; gIter != NULL; gIter = gIter->next) {
        const char *existing_ref = (const char *) gIter->data;

        if (crm_str_eq(existing_ref, obj_ref, TRUE)){
            is_existing = TRUE;
            break;
        }
    }

    if (is_existing == FALSE) {
        tag->refs = g_list_append(tag->refs, strdup(obj_ref));
        crm_trace("Added: tag=%s ref=%s", tag->id, obj_ref);
    }

    return TRUE;
}
