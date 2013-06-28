/*
 *  Shim IPC Process over Ethernet (using VLANs)
 *
 *    Francesco Salvestrini <f.salvestrini@nextworks.it>
 *    Sander Vrijders       <sander.vrijders@intec.ugent.be>
 *    Miquel Tarzan         <miquel.tarzan@i2cat.net>
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

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/if_ether.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/if_packet.h>

#define RINA_PREFIX "shim-eth"

#include "logs.h"
#include "common.h"
#include "shim.h"

static struct shim_t * shim;

/* Holds the configuration of one shim IPC process */
struct shim_eth_info_t {
        uint16_t        vlan_id;
        char *          interface_name;
        struct name_t * name;
};

enum port_id_state_t {
        PORT_STATE_NULL = 1,
        PORT_STATE_RECIPIENT_ALLOCATE_PENDING,
        PORT_STATE_INITIATOR_ALLOCATE_PENDING,
        PORT_STATE_ALLOCATED
};

/* Hold the information related to one flow*/
struct shim_eth_flow_t {
        uint64_t             src_mac;
        uint64_t             dst_mac;
        port_id_t            port_id;
        enum port_id_state_t port_id_state;

        /* FIXME: Will be a kfifo holding the SDUs or a sk_buff_head */
        /* QUEUE(sdu_queue, sdu_t *); */
};

/*
 * Contains all the information associated to an instance of a
 * shim Ethernet IPC Process
 */
struct shim_eth_instance_t {
        struct rb_node node;

        /* IPC process id and DIF name */
        ipc_process_id_t ipc_process_id;

        /* The configuration of the shim IPC Process */
        struct shim_eth_info_t * info;

        /* FIXME: Pointer to the device driver data_structure */
	/* Unsure if really needed */
        /* device_t * device_driver; */

        /* FIXME: Stores the state of flows indexed by port_id */
        /* HASH_TABLE(flow_to_port_id, port_id_t, shim_eth_flow_t); */
        /* rbtree or hash table? */
};


static int shim_flow_allocate_request(void *                     opaque,
                                      const struct name_t *      source,
                                      const struct name_t *      dest,
                                      const struct flow_spec_t * flow_spec,
                                      port_id_t                * port_id)
{
        LOG_FBEGN;
        LOG_FEXIT;

        return 0;
}

static int shim_flow_allocate_response(void *              opaque,
                                       port_id_t           port_id,
                                       response_reason_t * response)
{
        LOG_FBEGN;
        LOG_FEXIT;

        return 0;
}

static int shim_flow_deallocate(void *    opaque,
                                port_id_t port_id)
{
        LOG_FBEGN;
        LOG_FEXIT;

        return 0;
}

static int shim_application_register(void *                opaque,
                                     const struct name_t * name)
{
        LOG_FBEGN;
        LOG_FEXIT;

        return 0;
}

static int shim_application_unregister(void *                opaque,
                                       const struct name_t * name)
{
        LOG_FBEGN;
        LOG_FEXIT;

        return 0;
}

static int shim_sdu_write(void *               opaque,
                          port_id_t            port_id,
                          const struct sdu_t * sdu)
{
        LOG_FBEGN;
        LOG_FEXIT;

        return 0;
}

static int shim_sdu_read(void *         opaque,
                         port_id_t      id,
                         struct sdu_t * sdu)
{
        LOG_FBEGN;
        LOG_FEXIT;

        return 0;
}

/* Filter the devices here. Accept packets from VLANs that are configured */
static int shim_rcv(struct sk_buff *     skb,
                    struct net_device *  dev,
                    struct packet_type * pt,
                    struct net_device *  orig_dev)
{
        if (skb->pkt_type == PACKET_OTHERHOST ||
            skb->pkt_type == PACKET_LOOPBACK) {
                kfree_skb(skb);
                return 0;
        }

        skb = skb_share_check(skb, GFP_ATOMIC);
        if (!skb)
                return 0;

        /* Get the SDU out of the sk_buff */
	


        kfree_skb(skb);
        return 0;
};

static struct shim_instance_t * shim_create(void *           opaque,
                                            ipc_process_id_t ipc_process_id)
{
        struct shim_instance_t * instance;
        struct shim_eth_instance_t * shim_instance;
        struct rb_node **p;
        struct rb_node *parent;
        struct shim_eth_instance_t * s;
        struct rb_root * shim_eth_root;
        LOG_FBEGN;

        shim_eth_root = (struct rb_root *) opaque;
        p = &shim_eth_root->rb_node;
        parent = NULL;

        instance = kmalloc(sizeof(*instance), GFP_KERNEL);
        if (!instance) {
                LOG_ERR("Cannot allocate memory for shim_instance");
                LOG_FEXIT;
                return 0;
        }

        shim_instance = kmalloc(sizeof(*shim_instance), GFP_KERNEL);
        if (!shim_instance) {
                LOG_ERR("Cannot allocate memory for shim_eth_instance");
                LOG_FEXIT;
                return instance;
        }

        shim_instance->ipc_process_id = ipc_process_id;

        while (*p) {
                parent = *p;
                s = rb_entry(parent, struct shim_eth_instance_t, node);
                if (unlikely(ipc_process_id == s->ipc_process_id)) {
                        LOG_ERR("Shim instance with id %x already exists",
                                ipc_process_id);
                        kfree(shim_instance);
                        kfree(instance);
                        LOG_FEXIT;
                        return 0;
                }
                else if (ipc_process_id < s->ipc_process_id)
                        p = &(*p)->rb_left;
                else
                        p = &(*p)->rb_right;
        }
        rb_link_node(&shim_instance->node,parent,p);
        rb_insert_color(&shim_instance->node,shim_eth_root);

        instance->opaque                 = shim_instance;
        instance->flow_allocate_request  = shim_flow_allocate_request;
        instance->flow_allocate_response = shim_flow_allocate_response;
        instance->flow_deallocate        = shim_flow_deallocate;
        instance->application_register   = shim_application_register;
        instance->application_unregister = shim_application_unregister;
        instance->sdu_write              = shim_sdu_write;
        instance->sdu_read               = shim_sdu_read;

        LOG_FEXIT;
        return instance;
}

static int name_cpy(struct name_t * dst,
                    const struct name_t *src)
{
        struct name_t * temp;
        LOG_FBEGN;
        temp = kmalloc(sizeof(*temp), GFP_KERNEL);
        if (!temp) {
                LOG_ERR("Cannot allocate memory for name");
                LOG_FEXIT;
                return -1;
        }

        strcpy(temp->process_name, src->process_name);
        strcpy(temp->process_instance, src->process_instance);
        strcpy(temp->entity_name, src->entity_name);
        strcpy(temp->entity_instance, src->entity_instance);
        dst = temp;
        return 0;
}

struct shim_instance_t * shim_configure
(void *                     opaque,
 struct shim_instance_t *   inst,
 const struct shim_conf_t * configuration)
{
        struct shim_eth_instance_t * instance;
        struct shim_eth_info_t * shim_info;
        struct list_head * pos;
        struct shim_conf_t * c;
        struct shim_config_entry_t * tmp;
        struct shim_config_value_t * val;
        bool_t reconfigure;
	uint16_t old_vlan_id;
	string_t * old_interface_name;

        /* Check if instance is not null, check if opaque is not null */
        if (!inst) {
                LOG_WARN("Configure called on empty shim instance");
                LOG_FEXIT;
                return inst;
        }

        instance = (struct shim_eth_instance_t *) inst->opaque;
        if (!instance) {
                LOG_WARN("Configure called on empty eth vlan shim instance");
                LOG_FEXIT;
                return inst;
        }

        /* If reconfigure = 1, break down all communication and setup again */
        reconfigure = 0;

        /* Get configuration struct pertaining to this shim instance */
        shim_info = instance->info;
	old_vlan_id = 0;
	old_interface_name = NULL;
        if (!shim_info) {
                shim_info = kmalloc(sizeof(*shim_info), GFP_KERNEL);
                reconfigure = 1;
        } else {
		old_vlan_id = shim_info->vlan_id;
		old_interface_name = shim_info->interface_name;
	}
        if (!shim_info) {
                LOG_ERR("Cannot allocate memory for shim_info");
                LOG_FEXIT;
                return inst;
        }

        /* Retrieve configuration of IPC process from params */
        list_for_each(pos, &(configuration->list)) {
                c = list_entry(pos, struct shim_conf_t, list);
                tmp = c->entry;
                val = tmp->value;
                if (!strcmp(tmp->name, "difname")
                    && val->type == SHIM_CONFIG_STRING) {
                        if (!name_cpy(shim_info->name,
                                      (struct name_t *) val->data)) {
                                LOG_FEXIT;
                                return inst;
                        }
                } else if (!strcmp(tmp->name, "vlanid")
                           && val->type == SHIM_CONFIG_UINT) {
                        shim_info->vlan_id = * (uint16_t *) val->data;
                        if (!reconfigure && 
				shim_info->vlan_id != old_vlan_id) {
				reconfigure = 1;
			}
                } else if (!strcmp(tmp->name,"interfacename")
                           && val->type == SHIM_CONFIG_STRING) {
			/* FIXME: Should probably be strcpy */
                        shim_info->interface_name = (string_t *) val->data;
			if (!reconfigure && !strcmp(shim_info->interface_name, 
							old_interface_name)) {
				reconfigure = 1;
			}
                } else {
                        LOG_WARN("Unknown config param for eth shim");
                }
        }
        instance->info = shim_info;

        if (reconfigure) {
		struct packet_type * shim_eth_vlan_packet_type;
		
		shim_eth_vlan_packet_type = 
			kmalloc(sizeof(*shim_eth_vlan_packet_type), GFP_KERNEL);
		if (!shim_eth_vlan_packet_type) {
			LOG_ERR("Cannot allocate memory for packet_type");
			LOG_FEXIT;
			return inst;
		}
		shim_eth_vlan_packet_type->type = cpu_to_be16(ETH_P_RINA);
		shim_eth_vlan_packet_type->func = shim_rcv;
		
                /* Remove previous handler if there's one */ 
		if (old_interface_name && old_vlan_id != 0) {
			char string_old_vlan_id[4];
			char * complete_interface;
			struct net_device *dev;
			
			/* First construct the complete interface name */
			complete_interface = 
				kmalloc(sizeof(*complete_interface), GFP_KERNEL);
			if (!complete_interface) {
				LOG_ERR("Cannot allocate memory for string");
				LOG_FEXIT;
				return inst;
			}

			sprintf(string_old_vlan_id,"%d",old_vlan_id);
			strcat(complete_interface, ".");
			strcat(complete_interface, string_old_vlan_id);
			
			/* Remove the handler */
			read_lock(&dev_base_lock);
			dev = __dev_get_by_name(&init_net, complete_interface);
			if (!dev) {
				LOG_ERR("Invalid device specified to configure");
				LOG_FEXIT;
				return inst;	
			}
			shim_eth_vlan_packet_type->dev = dev;
			dev_remove_pack(shim_eth_vlan_packet_type);
			read_unlock(&dev_base_lock);
			kfree(complete_interface);
		}
                /* FIXME: Add handler to correct interface and vlan id */
                /* Check if correctness VLAN id and interface name */
		

		dev_add_pack(shim_eth_vlan_packet_type);
		kfree(shim_eth_vlan_packet_type);
        }
        LOG_DBG("Configured shim ETH IPC Process");

        return inst;
}

static int shim_destroy(void *                   opaque,
                        struct shim_instance_t * inst)
{
        struct shim_eth_instance_t * instance;

        struct rb_root * shim_eth_root;
        LOG_FBEGN;

        shim_eth_root = (struct rb_root *) opaque;

        if (inst) {
                /*
                 * FIXME: Need to ask instance to clean up as well
                 * Don't know yet in full what to delete
                 */
                instance = (struct shim_eth_instance_t *) inst->opaque;
                if (instance) {
                        rb_erase(&instance->node, shim_eth_root);
                        kfree(instance);
                }
                kfree(inst);
        }
        LOG_FEXIT;
        return 0;
}

static int __init mod_init(void)
{
        /* Holds all shim instances */
        struct rb_root * shim_eth_root;
        LOG_FBEGN;
        LOG_INFO("Shim-eth-vlan module v%d.%d loaded",0,1);

        shim_eth_root = kmalloc(sizeof(*shim_eth_root), GFP_KERNEL);
        if (!shim_eth_root) {
                LOG_ERR("Cannot allocate %zu bytes of memory",
                        sizeof(*shim_eth_root));
                LOG_FEXIT;
                return -1;
        }

        *shim_eth_root = RB_ROOT;

        shim = kmalloc(sizeof(*shim), GFP_KERNEL);
        if (!shim) {
                LOG_ERR("Cannot allocate %zu bytes of memory", sizeof(*shim));
                LOG_FEXIT;
                return -1;
        }

        shim->label     = "shim-eth-vlan";
        shim->create    = shim_create;
        shim->destroy   = shim_destroy;
        shim->configure = shim_configure;

        shim->opaque = shim_eth_root;

        if (shim_register(shim)) {
                LOG_ERR("Initialization of module shim-eth-vlan failed");
                kfree(shim);

                LOG_FEXIT;
                return -1;
        }

        LOG_FEXIT;

        return 0;
}

static void __exit mod_exit(void)
{
        struct rb_node * s;
        struct rb_node * e;
        struct shim_eth_instance_t * i;
        struct rb_root * shim_eth_root;
        LOG_FBEGN;

        shim_eth_root = (struct rb_root *) shim->opaque;

        /* Destroy all shim instances */
        s = rb_first(shim_eth_root);
        while(s) {
                /* Get next node and keep pointer to this one */
                e = s;
                rb_next(s);
                /*
                 * Get the shim_instance
                 * FIXME: Need to ask it to clean up as well
                 * Don't know yet in full what to delete
                 */
                i = rb_entry(e,struct shim_eth_instance_t, node);
                rb_erase(e, shim_eth_root);
                kfree(i);
        }

        kfree(shim->opaque);
        kfree(shim);

        LOG_FEXIT;
}

module_init(mod_init);
module_exit(mod_exit);

MODULE_DESCRIPTION("RINA Shim IPC over Ethernet");

MODULE_LICENSE("GPL");

MODULE_AUTHOR("Francesco Salvestrini <f.salvestrini@nextworks.it>");
MODULE_AUTHOR("Miquel Tarzan <miquel.tarzan@i2cat.net>");
MODULE_AUTHOR("Sander Vrijders <sander.vrijders@intec.ugent.be>");
