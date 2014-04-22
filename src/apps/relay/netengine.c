/*
 * Copyright (C) 2011, 2012, 2013 Citrix Systems
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "mainrelay.h"

//////////// Barrier for the threads //////////////

#if !defined(TURN_NO_THREAD_BARRIERS)
static unsigned int barrier_count = 0;
static pthread_barrier_t barrier;
#endif

//////////////////////////////////////////////

#define get_real_general_relay_servers_number() (turn_params.general_relay_servers_number > 1 ? turn_params.general_relay_servers_number : 1)
#define get_real_udp_relay_servers_number() (turn_params.udp_relay_servers_number > 1 ? turn_params.udp_relay_servers_number : 1)

static struct relay_server **general_relay_servers = NULL;
static struct relay_server **udp_relay_servers = NULL;

//////////////////////////////////////////////

static void run_events(struct event_base *eb, ioa_engine_handle e);
static void setup_relay_server(struct relay_server *rs, ioa_engine_handle e, int to_set_rfc5780);

/////////////// BARRIERS ///////////////////

#if !defined(PTHREAD_BARRIER_SERIAL_THREAD)
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)
#endif

static void barrier_wait_func(const char* func, int line)
{
#if !defined(TURN_NO_THREAD_BARRIERS)
	int br = 0;
	do {
		br = pthread_barrier_wait(&barrier);
		if ((br < 0)&&(br != PTHREAD_BARRIER_SERIAL_THREAD)) {
			int err = errno;
			perror("barrier wait");
			printf("%s:%s:%d: %d\n", __FUNCTION__, func,line,err);
		}
	} while (((br < 0)&&(br != PTHREAD_BARRIER_SERIAL_THREAD)) && (errno == EINTR));
#else
	UNUSED_ARG(func);
	UNUSED_ARG(line);
	sleep(5);
#endif
}

#define barrier_wait() barrier_wait_func(__FUNCTION__,__LINE__)

/////////////// AUX SERVERS ////////////////

static void add_aux_server_list(const char *saddr, turn_server_addrs_list_t *list)
{
	if(saddr && list) {
		ioa_addr addr;
		if(make_ioa_addr_from_full_string((const u08bits*)saddr, 0, &addr)!=0) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Wrong full address format: %s\n",saddr);
		} else {
			list->addrs = (ioa_addr*)realloc(list->addrs,sizeof(ioa_addr)*(list->size+1));
			addr_cpy(&(list->addrs[(list->size)++]),&addr);
			{
				u08bits s[1025];
				addr_to_string(&addr, s);
				TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "Aux server: %s\n",s);
			}
		}
	}
}

void add_aux_server(const char *saddr)
{
	add_aux_server_list(saddr,&turn_params.aux_servers_list);
}

/////////////// ALTERNATE SERVERS ////////////////

static void add_alt_server(const char *saddr, int default_port, turn_server_addrs_list_t *list)
{
	if(saddr && list) {
		ioa_addr addr;

		turn_mutex_lock(&(list->m));

		if(make_ioa_addr_from_full_string((const u08bits*)saddr, default_port, &addr)!=0) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Wrong IP address format: %s\n",saddr);
		} else {
			list->addrs = (ioa_addr*)realloc(list->addrs,sizeof(ioa_addr)*(list->size+1));
			addr_cpy(&(list->addrs[(list->size)++]),&addr);
			{
				u08bits s[1025];
				addr_to_string(&addr, s);
				TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "Alternate server added: %s\n",s);
			}
		}

		turn_mutex_unlock(&(list->m));
	}
}

static void del_alt_server(const char *saddr, int default_port, turn_server_addrs_list_t *list)
{
	if(saddr && list && list->size && list->addrs) {

		ioa_addr addr;

		turn_mutex_lock(&(list->m));

		if(make_ioa_addr_from_full_string((const u08bits*)saddr, default_port, &addr)!=0) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Wrong IP address format: %s\n",saddr);
		} else {

			size_t i;
			int found = 0;
			for(i=0;i<list->size;++i) {
				if(addr_eq(&(list->addrs[i]),&addr)) {
					found = 1;
					break;
				}
			}

			if(found) {

				size_t j;
				ioa_addr *new_addrs = (ioa_addr*)malloc(sizeof(ioa_addr)*(list->size-1));
				for(j=0;j<i;++j) {
					addr_cpy(&(new_addrs[j]),&(list->addrs[j]));
				}
				for(j=i;j<list->size-1;++j) {
					addr_cpy(&(new_addrs[j]),&(list->addrs[j+1]));
				}

				free(list->addrs);
				list->addrs = new_addrs;
				list->size -= 1;

				{
					u08bits s[1025];
					addr_to_string(&addr, s);
					TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "Alternate server removed: %s\n",s);
				}

				del_alt_server(saddr, default_port, list);
			}
		}

		turn_mutex_unlock(&(list->m));
	}
}

void add_alternate_server(const char *saddr)
{
	add_alt_server(saddr,DEFAULT_STUN_PORT,&turn_params.alternate_servers_list);
}

void del_alternate_server(const char *saddr)
{
	del_alt_server(saddr,DEFAULT_STUN_PORT,&turn_params.alternate_servers_list);
}

void add_tls_alternate_server(const char *saddr)
{
	add_alt_server(saddr,DEFAULT_STUN_TLS_PORT,&turn_params.tls_alternate_servers_list);
}

void del_tls_alternate_server(const char *saddr)
{
	del_alt_server(saddr,DEFAULT_STUN_TLS_PORT,&turn_params.tls_alternate_servers_list);
}

//////////////////////////////////////////////////

void add_listener_addr(const char* addr) {
	ioa_addr baddr;
	if(make_ioa_addr((const u08bits*)addr,0,&baddr)<0) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,"Cannot add a listener address: %s\n",addr);
	} else {
		size_t i = 0;
		for(i=0;i<turn_params.listener.addrs_number;++i) {
			if(addr_eq(turn_params.listener.encaddrs[turn_params.listener.addrs_number-1],&baddr)) {
				return;
			}
		}
		++turn_params.listener.addrs_number;
		++turn_params.listener.services_number;
		turn_params.listener.addrs = (char**)realloc(turn_params.listener.addrs, sizeof(char*)*turn_params.listener.addrs_number);
		turn_params.listener.addrs[turn_params.listener.addrs_number-1]=strdup(addr);
		turn_params.listener.encaddrs = (ioa_addr**)realloc(turn_params.listener.encaddrs, sizeof(ioa_addr*)*turn_params.listener.addrs_number);
		turn_params.listener.encaddrs[turn_params.listener.addrs_number-1]=(ioa_addr*)turn_malloc(sizeof(ioa_addr));
		addr_cpy(turn_params.listener.encaddrs[turn_params.listener.addrs_number-1],&baddr);
		TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "Listener address to use: %s\n",addr);
	}
}

int add_relay_addr(const char* addr) {
	ioa_addr baddr;
	if(make_ioa_addr((const u08bits*)addr,0,&baddr)<0) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,"Cannot add a relay address: %s\n",addr);
		return -1;
	} else {

		char sbaddr[129];
		addr_to_string_no_port(&baddr,(u08bits*)sbaddr);

		size_t i = 0;
		for(i=0;i<turn_params.relays_number;++i) {
			if(!strcmp(turn_params.relay_addrs[turn_params.relays_number-1],sbaddr)) {
				return 0;
			}
		}

		++turn_params.relays_number;
		turn_params.relay_addrs = (char**)realloc(turn_params.relay_addrs, sizeof(char*)*turn_params.relays_number);
		turn_params.relay_addrs[turn_params.relays_number-1]=strdup(sbaddr);

		TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "Relay address to use: %s\n",sbaddr);
		return 1;
	}
}

static void allocate_relay_addrs_ports(void) {
	int i;
	TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "Wait for relay ports initialization...\n");
	for(i=0;i<(int)turn_params.relays_number;i++) {
		ioa_addr baddr;
		if(make_ioa_addr((const u08bits*)turn_params.relay_addrs[i],0,&baddr)>=0) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "  relay %s initialization...\n",turn_params.relay_addrs[i]);
			turnipports_add_ip(STUN_ATTRIBUTE_TRANSPORT_UDP_VALUE, &baddr);
			turnipports_add_ip(STUN_ATTRIBUTE_TRANSPORT_TCP_VALUE, &baddr);
			TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "  relay %s initialization done\n",turn_params.relay_addrs[i]);
		}
	}
	TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "Relay ports initialization done\n");
}

//////////////////////////////////////////////////

// communications between listener and relays ==>>

static int handle_relay_message(relay_server_handle rs, struct message_to_relay *sm);

void send_auth_message_to_auth_server(struct auth_message *am)
{
	struct evbuffer *output = bufferevent_get_output(turn_params.authserver.out_buf);
	if(evbuffer_add(output,am,sizeof(struct auth_message))<0) {
		fprintf(stderr,"%s: Weird buffer error\n",__FUNCTION__);
	}
}

static void auth_server_receive_message(struct bufferevent *bev, void *ptr)
{
  UNUSED_ARG(ptr);
  
  struct auth_message am;
  int n = 0;
  struct evbuffer *input = bufferevent_get_input(bev);
  
  while ((n = evbuffer_remove(input, &am, sizeof(struct auth_message))) > 0) {
    if (n != sizeof(struct auth_message)) {
      fprintf(stderr,"%s: Weird buffer error: size=%d\n",__FUNCTION__,n);
      continue;
    }
    
    if(am.ct == TURN_CREDENTIALS_SHORT_TERM) {
      st_password_t pwd;
      if(get_user_pwd(am.username,pwd)<0) {
    	  am.success = 0;
      } else {
    	  ns_bcopy(pwd,am.pwd,sizeof(st_password_t));
    	  am.success = 1;
      }
    } else {
      hmackey_t key;
      if(get_user_key(am.username,am.realm,key,am.in_buffer.nbh)<0) {
    	  am.success = 0;
      } else {
    	  ns_bcopy(key,am.key,sizeof(hmackey_t));
    	  am.success = 1;
      }
    }
    
    size_t dest = am.id;
    
    struct evbuffer *output = NULL;
    
    if(dest>=TURNSERVER_ID_BOUNDARY_BETWEEN_TCP_AND_UDP) {
      dest -= TURNSERVER_ID_BOUNDARY_BETWEEN_TCP_AND_UDP;
      if(dest >= get_real_udp_relay_servers_number()) {
    	  TURN_LOG_FUNC(
		      TURN_LOG_LEVEL_ERROR,
		      "%s: Too large UDP relay number: %d\n",
		      __FUNCTION__,(int)dest);
      } else {
    	  output = bufferevent_get_output(udp_relay_servers[dest]->auth_out_buf);
      }
    } else {
      if(dest >= get_real_general_relay_servers_number()) {
    	  TURN_LOG_FUNC(
		      TURN_LOG_LEVEL_ERROR,
		      "%s: Too large general relay number: %d\n",
		      __FUNCTION__,(int)dest);
      } else {
    	  output = bufferevent_get_output(general_relay_servers[dest]->auth_out_buf);
      }
    }
    
    if(output)
      evbuffer_add(output,&am,sizeof(struct auth_message));
    else {
      ioa_network_buffer_delete(NULL, am.in_buffer.nbh);
      am.in_buffer.nbh = NULL;
    }
  }
}

static int send_socket_to_general_relay(ioa_engine_handle e, struct message_to_relay *sm)
{
	struct relay_server *rdest = sm->relay_server;

	if(!rdest) {
		size_t dest = (hash_int32(addr_get_port(&(sm->m.sm.nd.src_addr)))) % get_real_general_relay_servers_number();
		rdest = general_relay_servers[dest];
	}

	struct message_to_relay *smptr = sm;

	smptr->t = RMT_SOCKET;

	{
		struct evbuffer *output = NULL;
		int success = 0;

		output = bufferevent_get_output(rdest->out_buf);

		if(output) {

			if(evbuffer_add(output,smptr,sizeof(struct message_to_relay))<0) {
				TURN_LOG_FUNC(
					TURN_LOG_LEVEL_ERROR,
					"%s: Cannot add message to relay output buffer\n",
					__FUNCTION__);
			} else {

				success = 1;
				smptr->m.sm.nd.nbh=NULL;
			}

		}

		if(!success) {
			ioa_network_buffer_delete(e, smptr->m.sm.nd.nbh);
			smptr->m.sm.nd.nbh=NULL;

			IOA_CLOSE_SOCKET(smptr->m.sm.s);

			return -1;
		}
	}

	return 0;
}

static int send_socket_to_relay(turnserver_id id, u64bits cid, stun_tid *tid, ioa_socket_handle s, 
				int message_integrity, MESSAGE_TO_RELAY_TYPE rmt, ioa_net_data *nd)
{
	int ret = 0;

	struct message_to_relay sm;
	ns_bzero(&sm,sizeof(struct message_to_relay));
	sm.t = rmt;

	struct relay_server *rs = NULL;
	if(id>=TURNSERVER_ID_BOUNDARY_BETWEEN_TCP_AND_UDP) {
		size_t dest = id-TURNSERVER_ID_BOUNDARY_BETWEEN_TCP_AND_UDP;
		if(dest >= get_real_udp_relay_servers_number()) {
			TURN_LOG_FUNC(
					TURN_LOG_LEVEL_ERROR,
					"%s: Too large UDP relay number: %d, rmt=%d\n",
					__FUNCTION__,(int)dest,(int)rmt);
			ret = -1;
			goto err;
		}
		rs = udp_relay_servers[dest];
	} else {
		size_t dest = id;
		if(dest >= get_real_general_relay_servers_number()) {
			TURN_LOG_FUNC(
					TURN_LOG_LEVEL_ERROR,
					"%s: Too large general relay number: %d, rmt=%d\n",
					__FUNCTION__,(int)dest,(int)rmt);
			ret = -1;
			goto err;
		}
		rs = general_relay_servers[dest];
	}

	switch (rmt) {
	case(RMT_CB_SOCKET): {

		sm.m.cb_sm.id = id;
		sm.m.cb_sm.connection_id = (tcp_connection_id)cid;
		stun_tid_cpy(&(sm.m.cb_sm.tid),tid);
		sm.m.cb_sm.s = s;
		sm.m.cb_sm.message_integrity = message_integrity;

		break;
	}
	case (RMT_MOBILE_SOCKET): {

		if(nd && nd->nbh) {
			sm.m.sm.s = s;
			addr_cpy(&(sm.m.sm.nd.src_addr),&(nd->src_addr));
			sm.m.sm.nd.recv_tos = nd->recv_tos;
			sm.m.sm.nd.recv_ttl = nd->recv_ttl;
			sm.m.sm.nd.nbh = nd->nbh;
			nd->nbh = NULL;
		} else {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "%s: Empty buffer with mobile socket\n",__FUNCTION__);
			ret = -1;
		}

		break;
	}
	default: {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "%s: UNKNOWN RMT message: %d\n",__FUNCTION__,(int)rmt);
		ret = -1;
	}
	}

	if(ret == 0) {

		struct evbuffer *output = bufferevent_get_output(rs->out_buf);
		if(output) {
			evbuffer_add(output,&sm,sizeof(struct message_to_relay));
		} else {
			TURN_LOG_FUNC(
					TURN_LOG_LEVEL_ERROR,
					"%s: Empty output buffer\n",
					__FUNCTION__);
			ret = -1;
		}
	}

 err:
	if(ret < 0) {
	  IOA_CLOSE_SOCKET(s);
	  if(nd) {
	    ioa_network_buffer_delete(NULL, nd->nbh);
	    nd->nbh = NULL;
	  }
	  if(rmt == RMT_MOBILE_SOCKET) {
	    ioa_network_buffer_delete(NULL, sm.m.sm.nd.nbh);
	    sm.m.sm.nd.nbh = NULL;
	  }
	}

	return ret;
}

static int handle_relay_message(relay_server_handle rs, struct message_to_relay *sm)
{
	if(rs && sm) {

		switch (sm->t) {

		case RMT_SOCKET: {

			if (sm->m.sm.s->defer_nbh) {
				if (!sm->m.sm.nd.nbh) {
					sm->m.sm.nd.nbh = sm->m.sm.s->defer_nbh;
					sm->m.sm.s->defer_nbh = NULL;
				} else {
					ioa_network_buffer_delete(rs->ioa_eng, sm->m.sm.s->defer_nbh);
					sm->m.sm.s->defer_nbh = NULL;
				}
			}

			ioa_socket_handle s = sm->m.sm.s;

			if (!s) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,
					"%s: socket EMPTY\n",__FUNCTION__);
			} else if (s->read_event || s->bev) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,
					"%s: socket wrongly preset: 0x%lx : 0x%lx\n",
					__FUNCTION__, (long) s->read_event, (long) s->bev);
				IOA_CLOSE_SOCKET(s);
			} else {
				s->e = rs->ioa_eng;
				open_client_connection_session(&(rs->server), &(sm->m.sm));
			}

			ioa_network_buffer_delete(rs->ioa_eng, sm->m.sm.nd.nbh);
			sm->m.sm.nd.nbh = NULL;
		}
			break;
		case RMT_CB_SOCKET:

			turnserver_accept_tcp_client_data_connection(&(rs->server), sm->m.cb_sm.connection_id,
				&(sm->m.cb_sm.tid), sm->m.cb_sm.s, sm->m.cb_sm.message_integrity);

			break;
		case RMT_MOBILE_SOCKET: {

			ioa_socket_handle s = sm->m.sm.s;

			if (!s) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,
							"%s: mobile socket EMPTY\n",__FUNCTION__);
			} else if (s->read_event || s->bev) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,
									"%s: mobile socket wrongly preset: 0x%lx : 0x%lx\n",
									__FUNCTION__, (long) s->read_event, (long) s->bev);
				IOA_CLOSE_SOCKET(s);
			} else {
				s->e = rs->ioa_eng;
				open_client_connection_session(&(rs->server), &(sm->m.sm));
			}

			ioa_network_buffer_delete(rs->ioa_eng, sm->m.sm.nd.nbh);
			sm->m.sm.nd.nbh = NULL;
			break;
		}
		default: {
			perror("Weird buffer type\n");
		}
		}
	}

	return 0;
}

static void handle_relay_auth_message(struct relay_server *rs, struct auth_message *am)
{
	am->resume_func(am->success, am->key, am->pwd,
				&(rs->server), am->ctxkey, &(am->in_buffer));
	if (am->in_buffer.nbh) {
		ioa_network_buffer_delete(rs->ioa_eng, am->in_buffer.nbh);
		am->in_buffer.nbh = NULL;
	}
}

static void relay_receive_message(struct bufferevent *bev, void *ptr)
{
	struct message_to_relay sm;
	int n = 0;
	struct evbuffer *input = bufferevent_get_input(bev);
	struct relay_server *rs = (struct relay_server *)ptr;

	while ((n = evbuffer_remove(input, &sm, sizeof(struct message_to_relay))) > 0) {

		if (n != sizeof(struct message_to_relay)) {
			perror("Weird buffer error\n");
			continue;
		}

		handle_relay_message(rs, &sm);
	}
}

static void relay_receive_auth_message(struct bufferevent *bev, void *ptr)
{
	struct auth_message am;
	int n = 0;
	struct evbuffer *input = bufferevent_get_input(bev);
	struct relay_server *rs = (struct relay_server *)ptr;

	while ((n = evbuffer_remove(input, &am, sizeof(struct auth_message))) > 0) {

		if (n != sizeof(struct auth_message)) {
			perror("Weird auth_buffer error\n");
			continue;
		}

		handle_relay_auth_message(rs, &am);
	}
}

static int send_message_from_listener_to_client(ioa_engine_handle e, ioa_network_buffer_handle nbh, ioa_addr *origin, ioa_addr *destination)
{

	struct message_to_listener mm;
	mm.t = LMT_TO_CLIENT;
	addr_cpy(&(mm.m.tc.origin),origin);
	addr_cpy(&(mm.m.tc.destination),destination);
	mm.m.tc.nbh = ioa_network_buffer_allocate(e);
	ioa_network_buffer_header_init(mm.m.tc.nbh);
	ns_bcopy(ioa_network_buffer_data(nbh),ioa_network_buffer_data(mm.m.tc.nbh),ioa_network_buffer_get_size(nbh));
	ioa_network_buffer_set_size(mm.m.tc.nbh,ioa_network_buffer_get_size(nbh));

	struct evbuffer *output = bufferevent_get_output(turn_params.listener.out_buf);

	evbuffer_add(output,&mm,sizeof(struct message_to_listener));

	return 0;
}

static void listener_receive_message(struct bufferevent *bev, void *ptr)
{
	UNUSED_ARG(ptr);

	struct message_to_listener mm;
	int n = 0;
	struct evbuffer *input = bufferevent_get_input(bev);

	while ((n = evbuffer_remove(input, &mm, sizeof(struct message_to_listener))) > 0) {
		if (n != sizeof(struct message_to_listener)) {
			perror("Weird buffer error\n");
			continue;
		}

		if (mm.t != LMT_TO_CLIENT) {
			perror("Weird buffer type\n");
			continue;
		}

		size_t relay_thread_index = 0;

		if(turn_params.net_engine_version == NEV_UDP_SOCKET_PER_THREAD) {
			size_t ri;
			for(ri=0;ri<get_real_general_relay_servers_number();ri++) {
				if(general_relay_servers[ri]->thr == pthread_self()) {
					relay_thread_index=ri;
					break;
				}
			}
		}

		size_t i;
		int found = 0;
		for(i=0;i<turn_params.listener.addrs_number;i++) {
			if(addr_eq_no_port(turn_params.listener.encaddrs[i],&mm.m.tc.origin)) {
				int o_port = addr_get_port(&mm.m.tc.origin);
				if(turn_params.listener.addrs_number == turn_params.listener.services_number) {
					if(o_port == turn_params.listener_port) {
						if(turn_params.listener.udp_services && turn_params.listener.udp_services[i] && turn_params.listener.udp_services[i][relay_thread_index]) {
							found = 1;
							udp_send_message(turn_params.listener.udp_services[i][relay_thread_index], mm.m.tc.nbh, &mm.m.tc.destination);
						}
					} else {
						TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,"%s: Wrong origin port(1): %d\n",__FUNCTION__,o_port);
					}
				} else if((turn_params.listener.addrs_number * 2) == turn_params.listener.services_number) {
					if(o_port == turn_params.listener_port) {
						if(turn_params.listener.udp_services && turn_params.listener.udp_services[i*2] && turn_params.listener.udp_services[i*2][relay_thread_index]) {
							found = 1;
							udp_send_message(turn_params.listener.udp_services[i*2][relay_thread_index], mm.m.tc.nbh, &mm.m.tc.destination);
						}
					} else if(o_port == get_alt_listener_port()) {
						if(turn_params.listener.udp_services && turn_params.listener.udp_services[i*2+1] && turn_params.listener.udp_services[i*2+1][relay_thread_index]) {
							found = 1;
							udp_send_message(turn_params.listener.udp_services[i*2+1][relay_thread_index], mm.m.tc.nbh, &mm.m.tc.destination);
						}
					} else {
						TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,"%s: Wrong origin port(2): %d\n",__FUNCTION__,o_port);
					}
				} else {
					TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,"%s: Wrong listener setup\n",__FUNCTION__);
				}
				break;
			}
		}

		if(!found) {
			u08bits saddr[129];
			addr_to_string(&mm.m.tc.origin, saddr);
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,"%s: Cannot find local source %s\n",__FUNCTION__,saddr);
		}

		ioa_network_buffer_delete(turn_params.listener.ioa_eng, mm.m.tc.nbh);
		 mm.m.tc.nbh = NULL;
	}
}

// <<== communications between listener and relays

static ioa_engine_handle create_new_listener_engine(void)
{
	struct event_base *eb = turn_event_base_new();
	TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO,"IO method (udp listener/relay thread): %s\n",event_base_get_method(eb));
	super_memory_t* sm = new_super_memory_region();
	ioa_engine_handle e = create_ioa_engine(sm, eb, turn_params.listener.tp, turn_params.relay_ifname, turn_params.relays_number, turn_params.relay_addrs,
			turn_params.default_relays, turn_params.verbose, turn_params.max_bps
#if !defined(TURN_NO_HIREDIS)
			,turn_params.redis_statsdb
#endif
	);
	set_ssl_ctx(e, turn_params.tls_ctx_ssl23, turn_params.tls_ctx_v1_0,
#if defined(SSL_TXT_TLSV1_1)
					turn_params.tls_ctx_v1_1,
#if defined(SSL_TXT_TLSV1_2)
					turn_params.tls_ctx_v1_2,
#endif
#endif
					turn_params.dtls_ctx);
	ioa_engine_set_rtcp_map(e, turn_params.listener.rtcpmap);
	return e;
}

static void *run_udp_listener_thread(void *arg)
{
  static int always_true = 1;

  ignore_sigpipe();

  barrier_wait();

  dtls_listener_relay_server_type *server = (dtls_listener_relay_server_type *)arg;

  while(always_true && server) {
    run_events(NULL, get_engine(server));
  }

  return arg;
}

static void setup_listener(void)
{
	super_memory_t* sm = new_super_memory_region();

	turn_params.listener.tp = turnipports_create(sm, turn_params.min_port, turn_params.max_port);

	turn_params.listener.event_base = turn_event_base_new();

	TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO,"IO method (main listener thread): %s\n",event_base_get_method(turn_params.listener.event_base));

	turn_params.listener.ioa_eng = create_ioa_engine(sm, turn_params.listener.event_base, turn_params.listener.tp,
			turn_params.relay_ifname, turn_params.relays_number, turn_params.relay_addrs,
			turn_params.default_relays, turn_params.verbose, turn_params.max_bps
#if !defined(TURN_NO_HIREDIS)
			,turn_params.redis_statsdb
#endif
			);

	if(!turn_params.listener.ioa_eng)
		exit(-1);

	set_ssl_ctx(turn_params.listener.ioa_eng, turn_params.tls_ctx_ssl23, turn_params.tls_ctx_v1_0,
#if defined(SSL_TXT_TLSV1_1)
					turn_params.tls_ctx_v1_1,
#if defined(SSL_TXT_TLSV1_2)
					turn_params.tls_ctx_v1_2,
#endif
#endif
					turn_params.dtls_ctx);

	turn_params.listener.rtcpmap = rtcp_map_create(turn_params.listener.ioa_eng);

	ioa_engine_set_rtcp_map(turn_params.listener.ioa_eng, turn_params.listener.rtcpmap);

	{
		struct bufferevent *pair[2];
		int opts = BEV_OPT_DEFER_CALLBACKS | BEV_OPT_UNLOCK_CALLBACKS;

		opts |= BEV_OPT_THREADSAFE;

		bufferevent_pair_new(turn_params.listener.event_base, opts, pair);
		turn_params.listener.in_buf = pair[0];
		turn_params.listener.out_buf = pair[1];
		bufferevent_setcb(turn_params.listener.in_buf, listener_receive_message, NULL, NULL, &turn_params.listener);
		bufferevent_enable(turn_params.listener.in_buf, EV_READ);
	}

	if(turn_params.listener.addrs_number<2) {
		turn_params.rfc5780 = 0;
		TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "WARNING: I cannot support STUN CHANGE_REQUEST functionality because only one IP address is provided\n");
	} else {
		turn_params.listener.services_number = turn_params.listener.services_number * 2;
	}

	turn_params.listener.udp_services = (dtls_listener_relay_server_type***)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(dtls_listener_relay_server_type**)*turn_params.listener.services_number);
	turn_params.listener.dtls_services = (dtls_listener_relay_server_type***)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(dtls_listener_relay_server_type**)*turn_params.listener.services_number);

	turn_params.listener.aux_udp_services = (dtls_listener_relay_server_type***)allocate_super_memory_engine(turn_params.listener.ioa_eng, (sizeof(dtls_listener_relay_server_type**)*turn_params.aux_servers_list.size)+sizeof(void*));
}

static void setup_barriers(void)
{
	/* Adjust barriers: */

#if !defined(TURN_NO_THREAD_BARRIERS)

	if((turn_params.net_engine_version == NEV_UDP_SOCKET_PER_ENDPOINT) && turn_params.general_relay_servers_number>1) {

		/* UDP: */
		if(!turn_params.no_udp) {

			barrier_count += turn_params.listener.addrs_number;

			if(turn_params.rfc5780) {
				barrier_count += turn_params.listener.addrs_number;
			}
		}

		if(!turn_params.no_dtls && (turn_params.no_udp || (turn_params.listener_port != turn_params.tls_listener_port))) {

			barrier_count += turn_params.listener.addrs_number;

			if(turn_params.rfc5780) {
				barrier_count += turn_params.listener.addrs_number;
			}
		}

		if(!turn_params.no_udp || !turn_params.no_dtls) {
			barrier_count += (unsigned int)turn_params.aux_servers_list.size;
		}
	}
#endif

#if !defined(TURN_NO_THREAD_BARRIERS)
	{
		if(pthread_barrier_init(&barrier,NULL,barrier_count)<0)
			perror("barrier init");
	}

#endif
}

static void setup_socket_per_endpoint_udp_listener_servers(void)
{
	size_t i = 0;

	/* Adjust udp relay number */

	if(turn_params.general_relay_servers_number>1) {

		if (!turn_params.no_udp) {

			turn_params.udp_relay_servers_number += turn_params.listener.addrs_number;

			if (turn_params.rfc5780) {
				turn_params.udp_relay_servers_number += turn_params.listener.addrs_number;
			}
		}

		if (!turn_params.no_dtls && (turn_params.no_udp || (turn_params.listener_port != turn_params.tls_listener_port))) {

			turn_params.udp_relay_servers_number += turn_params.listener.addrs_number;

			if (turn_params.rfc5780) {
				turn_params.udp_relay_servers_number += turn_params.listener.addrs_number;
			}
		}

		if (!turn_params.no_udp || !turn_params.no_dtls) {
			turn_params.udp_relay_servers_number += (unsigned int) turn_params.aux_servers_list.size;
		}
	}

	{
		if (!turn_params.no_udp || !turn_params.no_dtls) {
			udp_relay_servers = (struct relay_server**) allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(struct relay_server *)*get_real_udp_relay_servers_number());

			for (i = 0; i < get_real_udp_relay_servers_number(); i++) {

				ioa_engine_handle e = turn_params.listener.ioa_eng;
				int is_5780 = turn_params.rfc5780;

				if(turn_params.general_relay_servers_number<=1) {
					while(!(general_relay_servers[0]->ioa_eng))
						sched_yield();
					udp_relay_servers[i] = general_relay_servers[0];
					continue;
				} else if(turn_params.general_relay_servers_number>1) {
					e = create_new_listener_engine();
					is_5780 = is_5780 && (i >= (size_t) (turn_params.aux_servers_list.size));
				}

				super_memory_t *sm = new_super_memory_region();
				struct relay_server* udp_rs = (struct relay_server*) allocate_super_memory_region(sm, sizeof(struct relay_server));
				udp_rs->id = (turnserver_id) i + TURNSERVER_ID_BOUNDARY_BETWEEN_TCP_AND_UDP;
				udp_rs->sm = sm;
				setup_relay_server(udp_rs, e, is_5780);
				udp_relay_servers[i] = udp_rs;
			}
		}
	}

	int udp_relay_server_index = 0;

	/* Create listeners */

	/* Aux UDP servers */
	for(i=0; i<turn_params.aux_servers_list.size; i++) {

		int index = i;

		if(!turn_params.no_udp || !turn_params.no_dtls) {

			ioa_addr addr;
			char saddr[129];
			addr_cpy(&addr,&turn_params.aux_servers_list.addrs[i]);
			int port = (int)addr_get_port(&addr);
			addr_to_string_no_port(&addr,(u08bits*)saddr);

			turn_params.listener.aux_udp_services[index] = (dtls_listener_relay_server_type**)allocate_super_memory_engine(udp_relay_servers[udp_relay_server_index]->ioa_eng, sizeof(dtls_listener_relay_server_type*));
			turn_params.listener.aux_udp_services[index][0] = create_dtls_listener_server(turn_params.listener_ifname, saddr, port, turn_params.verbose, udp_relay_servers[udp_relay_server_index]->ioa_eng, &(udp_relay_servers[udp_relay_server_index]->server), 1, NULL);

			if(turn_params.general_relay_servers_number>1) {
				++udp_relay_server_index;
				pthread_t thr;
				if(pthread_create(&thr, NULL, run_udp_listener_thread, turn_params.listener.aux_udp_services[index][0])<0) {
					perror("Cannot create aux listener thread\n");
					exit(-1);
				}
				pthread_detach(thr);
			}
		}
	}

	/* Main servers */
	for(i=0; i<turn_params.listener.addrs_number; i++) {

		int index = turn_params.rfc5780 ? i*2 : i;

		/* UDP: */
		if(!turn_params.no_udp) {

			turn_params.listener.udp_services[index] = (dtls_listener_relay_server_type**)allocate_super_memory_engine(udp_relay_servers[udp_relay_server_index]->ioa_eng, sizeof(dtls_listener_relay_server_type*));
			turn_params.listener.udp_services[index][0] = create_dtls_listener_server(turn_params.listener_ifname, turn_params.listener.addrs[i], turn_params.listener_port, turn_params.verbose, udp_relay_servers[udp_relay_server_index]->ioa_eng, &(udp_relay_servers[udp_relay_server_index]->server), 1, NULL);

			if(turn_params.general_relay_servers_number>1) {
				++udp_relay_server_index;
				pthread_t thr;
				if(pthread_create(&thr, NULL, run_udp_listener_thread, turn_params.listener.udp_services[index][0])<0) {
					perror("Cannot create listener thread\n");
					exit(-1);
				}
				pthread_detach(thr);
			}

			if(turn_params.rfc5780) {

				turn_params.listener.udp_services[index+1] = (dtls_listener_relay_server_type**)allocate_super_memory_engine(udp_relay_servers[udp_relay_server_index]->ioa_eng, sizeof(dtls_listener_relay_server_type*));
				turn_params.listener.udp_services[index+1][0] = create_dtls_listener_server(turn_params.listener_ifname, turn_params.listener.addrs[i], get_alt_listener_port(), turn_params.verbose, udp_relay_servers[udp_relay_server_index]->ioa_eng, &(udp_relay_servers[udp_relay_server_index]->server), 1, NULL);

				if(turn_params.general_relay_servers_number>1) {
					++udp_relay_server_index;
					pthread_t thr;
					if(pthread_create(&thr, NULL, run_udp_listener_thread, turn_params.listener.udp_services[index+1][0])<0) {
						perror("Cannot create listener thread\n");
						exit(-1);
					}
					pthread_detach(thr);
				}
			}
		} else {
			turn_params.listener.udp_services[index] = NULL;
			if(turn_params.rfc5780)
				turn_params.listener.udp_services[index+1] = NULL;
		}
		if(!turn_params.no_dtls && (turn_params.no_udp || (turn_params.listener_port != turn_params.tls_listener_port))) {

			turn_params.listener.dtls_services[index] = (dtls_listener_relay_server_type**)allocate_super_memory_engine(udp_relay_servers[udp_relay_server_index]->ioa_eng, sizeof(dtls_listener_relay_server_type*));
			turn_params.listener.dtls_services[index][0] = create_dtls_listener_server(turn_params.listener_ifname, turn_params.listener.addrs[i], turn_params.tls_listener_port, turn_params.verbose, udp_relay_servers[udp_relay_server_index]->ioa_eng, &(udp_relay_servers[udp_relay_server_index]->server), 1, NULL);

			if(turn_params.general_relay_servers_number>1) {
				++udp_relay_server_index;
				pthread_t thr;
				if(pthread_create(&thr, NULL, run_udp_listener_thread, turn_params.listener.dtls_services[index][0])<0) {
					perror("Cannot create listener thread\n");
					exit(-1);
				}
				pthread_detach(thr);
			}

			if(turn_params.rfc5780) {

				turn_params.listener.dtls_services[index+1] = (dtls_listener_relay_server_type**)allocate_super_memory_engine(udp_relay_servers[udp_relay_server_index]->ioa_eng, sizeof(dtls_listener_relay_server_type*));
				turn_params.listener.dtls_services[index+1][0] = create_dtls_listener_server(turn_params.listener_ifname, turn_params.listener.addrs[i], get_alt_tls_listener_port(), turn_params.verbose, udp_relay_servers[udp_relay_server_index]->ioa_eng, &(udp_relay_servers[udp_relay_server_index]->server), 1, NULL);

				if(turn_params.general_relay_servers_number>1) {
					++udp_relay_server_index;
					pthread_t thr;
					if(pthread_create(&thr, NULL, run_udp_listener_thread, turn_params.listener.dtls_services[index+1][0])<0) {
						perror("Cannot create listener thread\n");
						exit(-1);
					}
					pthread_detach(thr);
				}
			}
		} else {
			turn_params.listener.dtls_services[index] = NULL;
			if(turn_params.rfc5780)
				turn_params.listener.dtls_services[index+1] = NULL;
		}
	}
}

static void setup_socket_per_thread_udp_listener_servers(void)
{
	size_t i = 0;
	size_t relayindex = 0;

	/* Create listeners */

	for(relayindex=0;relayindex<get_real_general_relay_servers_number();relayindex++) {
		while(!(general_relay_servers[relayindex]->ioa_eng) || !(general_relay_servers[relayindex]->server.e))
			sched_yield();
	}

	/* Aux UDP servers */
	for(i=0; i<turn_params.aux_servers_list.size; i++) {

		int index = i;

		if(!turn_params.no_udp || !turn_params.no_dtls) {

			ioa_addr addr;
			char saddr[129];
			addr_cpy(&addr,&turn_params.aux_servers_list.addrs[i]);
			int port = (int)addr_get_port(&addr);
			addr_to_string_no_port(&addr,(u08bits*)saddr);

			turn_params.listener.aux_udp_services[index] = (dtls_listener_relay_server_type**)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(dtls_listener_relay_server_type*) * get_real_general_relay_servers_number());

			for(relayindex=0;relayindex<get_real_general_relay_servers_number();relayindex++) {
				turn_params.listener.aux_udp_services[index][relayindex] = create_dtls_listener_server(turn_params.listener_ifname, saddr, port, turn_params.verbose,
						general_relay_servers[relayindex]->ioa_eng, &(general_relay_servers[relayindex]->server), !relayindex, NULL);
			}
		}
	}

	/* Main servers */
	for(i=0; i<turn_params.listener.addrs_number; i++) {

		int index = turn_params.rfc5780 ? i*2 : i;

		/* UDP: */
		if(!turn_params.no_udp) {

			turn_params.listener.udp_services[index] = (dtls_listener_relay_server_type**)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(dtls_listener_relay_server_type*) * get_real_general_relay_servers_number());

			for(relayindex=0;relayindex<get_real_general_relay_servers_number();relayindex++) {
				turn_params.listener.udp_services[index][relayindex] = create_dtls_listener_server(turn_params.listener_ifname, turn_params.listener.addrs[i], turn_params.listener_port, turn_params.verbose,
						general_relay_servers[relayindex]->ioa_eng, &(general_relay_servers[relayindex]->server), !relayindex, NULL);
			}

			if(turn_params.rfc5780) {

				turn_params.listener.udp_services[index+1] = (dtls_listener_relay_server_type**)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(dtls_listener_relay_server_type*) * get_real_general_relay_servers_number());

				for(relayindex=0;relayindex<get_real_general_relay_servers_number();relayindex++) {
					turn_params.listener.udp_services[index+1][relayindex] = create_dtls_listener_server(turn_params.listener_ifname, turn_params.listener.addrs[i], get_alt_listener_port(), turn_params.verbose,
							general_relay_servers[relayindex]->ioa_eng, &(general_relay_servers[relayindex]->server), !relayindex, NULL);
				}
			}
		} else {
			turn_params.listener.udp_services[index] = NULL;
			if(turn_params.rfc5780)
				turn_params.listener.udp_services[index+1] = NULL;
		}
		if(!turn_params.no_dtls && (turn_params.no_udp || (turn_params.listener_port != turn_params.tls_listener_port))) {

			turn_params.listener.dtls_services[index] = (dtls_listener_relay_server_type**)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(dtls_listener_relay_server_type*) * get_real_general_relay_servers_number());

			for(relayindex=0;relayindex<get_real_general_relay_servers_number();relayindex++) {
				turn_params.listener.dtls_services[index][relayindex] = create_dtls_listener_server(turn_params.listener_ifname, turn_params.listener.addrs[i], turn_params.tls_listener_port, turn_params.verbose,
						general_relay_servers[relayindex]->ioa_eng, &(general_relay_servers[relayindex]->server), !relayindex, NULL);
			}

			if(turn_params.rfc5780) {

				turn_params.listener.dtls_services[index+1] = (dtls_listener_relay_server_type**)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(dtls_listener_relay_server_type*) * get_real_general_relay_servers_number());

				for(relayindex=0;relayindex<get_real_general_relay_servers_number();relayindex++) {
					turn_params.listener.dtls_services[index+1][relayindex] = create_dtls_listener_server(turn_params.listener_ifname, turn_params.listener.addrs[i], get_alt_tls_listener_port(), turn_params.verbose,
							general_relay_servers[relayindex]->ioa_eng, &(general_relay_servers[relayindex]->server), !relayindex, NULL);
				}
			}
		} else {
			turn_params.listener.dtls_services[index] = NULL;
			if(turn_params.rfc5780)
				turn_params.listener.dtls_services[index+1] = NULL;
		}
	}
}

static void setup_socket_per_session_udp_listener_servers(void)
{
	size_t i = 0;

	/* Aux UDP servers */
	for(i=0; i<turn_params.aux_servers_list.size; i++) {

		int index = i;

		if(!turn_params.no_udp || !turn_params.no_dtls) {

			ioa_addr addr;
			char saddr[129];
			addr_cpy(&addr,&turn_params.aux_servers_list.addrs[i]);
			int port = (int)addr_get_port(&addr);
			addr_to_string_no_port(&addr,(u08bits*)saddr);

			turn_params.listener.aux_udp_services[index] = (dtls_listener_relay_server_type**)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(dtls_listener_relay_server_type*));

			turn_params.listener.aux_udp_services[index][0] = create_dtls_listener_server(turn_params.listener_ifname, saddr, port, turn_params.verbose,
						turn_params.listener.ioa_eng, NULL, 1, send_socket_to_general_relay);
		}
	}

	/* Main servers */
	for(i=0; i<turn_params.listener.addrs_number; i++) {

		int index = turn_params.rfc5780 ? i*2 : i;

		/* UDP: */
		if(!turn_params.no_udp) {

			turn_params.listener.udp_services[index] = (dtls_listener_relay_server_type**)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(dtls_listener_relay_server_type*));

			turn_params.listener.udp_services[index][0] = create_dtls_listener_server(turn_params.listener_ifname, turn_params.listener.addrs[i], turn_params.listener_port, turn_params.verbose,
						turn_params.listener.ioa_eng, NULL, 1, send_socket_to_general_relay);

			if(turn_params.rfc5780) {

				turn_params.listener.udp_services[index+1] = (dtls_listener_relay_server_type**)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(dtls_listener_relay_server_type*));

				turn_params.listener.udp_services[index+1][0] = create_dtls_listener_server(turn_params.listener_ifname, turn_params.listener.addrs[i], get_alt_listener_port(), turn_params.verbose,
							turn_params.listener.ioa_eng, NULL, 1, send_socket_to_general_relay);
			}
		} else {
			turn_params.listener.udp_services[index] = NULL;
			if(turn_params.rfc5780)
				turn_params.listener.udp_services[index+1] = NULL;
		}
		if(!turn_params.no_dtls && (turn_params.no_udp || (turn_params.listener_port != turn_params.tls_listener_port))) {

			turn_params.listener.dtls_services[index] = (dtls_listener_relay_server_type**)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(dtls_listener_relay_server_type*));

			turn_params.listener.dtls_services[index][0] = create_dtls_listener_server(turn_params.listener_ifname, turn_params.listener.addrs[i], turn_params.tls_listener_port, turn_params.verbose,
						turn_params.listener.ioa_eng, NULL, 1, send_socket_to_general_relay);

			if(turn_params.rfc5780) {

				turn_params.listener.dtls_services[index+1] = (dtls_listener_relay_server_type**)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(dtls_listener_relay_server_type*));

				turn_params.listener.dtls_services[index+1][0] = create_dtls_listener_server(turn_params.listener_ifname, turn_params.listener.addrs[i], get_alt_tls_listener_port(), turn_params.verbose,
							turn_params.listener.ioa_eng, NULL, 1, send_socket_to_general_relay);
			}
		} else {
			turn_params.listener.dtls_services[index] = NULL;
			if(turn_params.rfc5780)
				turn_params.listener.dtls_services[index+1] = NULL;
		}
	}
}

static void setup_tcp_listener_servers(ioa_engine_handle e, struct relay_server *relay_server)
{
	size_t i = 0;

	tls_listener_relay_server_type **tcp_services = (tls_listener_relay_server_type**)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(tls_listener_relay_server_type*)*turn_params.listener.services_number);
	tls_listener_relay_server_type **tls_services = (tls_listener_relay_server_type**)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(tls_listener_relay_server_type*)*turn_params.listener.services_number);

	tls_listener_relay_server_type **aux_tcp_services = (tls_listener_relay_server_type**)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(tls_listener_relay_server_type*)*turn_params.aux_servers_list.size+1);

	/* Create listeners */

	/* Aux TCP servers */
	if(!turn_params.no_tls || !turn_params.no_tcp) {

		for(i=0; i<turn_params.aux_servers_list.size; i++) {

			ioa_addr addr;
			char saddr[129];
			addr_cpy(&addr,&turn_params.aux_servers_list.addrs[i]);
			int port = (int)addr_get_port(&addr);
			addr_to_string_no_port(&addr,(u08bits*)saddr);

			aux_tcp_services[i] = create_tls_listener_server(turn_params.listener_ifname, saddr, port, turn_params.verbose, e, send_socket_to_general_relay, relay_server);
		}
	}

	/* Main servers */
	for(i=0; i<turn_params.listener.addrs_number; i++) {

		int index = turn_params.rfc5780 ? i*2 : i;

		/* TCP: */
		if(!turn_params.no_tcp) {
			tcp_services[index] = create_tls_listener_server(turn_params.listener_ifname, turn_params.listener.addrs[i], turn_params.listener_port, turn_params.verbose, e, send_socket_to_general_relay, relay_server);
			if(turn_params.rfc5780)
				tcp_services[index+1] = create_tls_listener_server(turn_params.listener_ifname, turn_params.listener.addrs[i], get_alt_listener_port(), turn_params.verbose, e, send_socket_to_general_relay, relay_server);
		} else {
			tcp_services[index] = NULL;
			if(turn_params.rfc5780)
				tcp_services[index+1] = NULL;
		}
		if(!turn_params.no_tls && (turn_params.no_tcp || (turn_params.listener_port != turn_params.tls_listener_port))) {
			tls_services[index] = create_tls_listener_server(turn_params.listener_ifname, turn_params.listener.addrs[i], turn_params.tls_listener_port, turn_params.verbose, e, send_socket_to_general_relay, relay_server);
			if(turn_params.rfc5780)
				tls_services[index+1] = create_tls_listener_server(turn_params.listener_ifname, turn_params.listener.addrs[i], get_alt_tls_listener_port(), turn_params.verbose, e, send_socket_to_general_relay, relay_server);
		} else {
			tls_services[index] = NULL;
			if(turn_params.rfc5780)
				tls_services[index+1] = NULL;
		}
	}
}

static int get_alt_addr(ioa_addr *addr, ioa_addr *alt_addr)
{
	if(!addr || !turn_params.rfc5780 || (turn_params.listener.addrs_number<2))
	  ;
	else {
		size_t index = 0xffff;
		size_t i = 0;
		int alt_port = -1;
		int port = addr_get_port(addr);

		if(port == turn_params.listener_port)
			alt_port = get_alt_listener_port();
		else if(port == get_alt_listener_port())
			alt_port = turn_params.listener_port;
		else if(port == turn_params.tls_listener_port)
			alt_port = get_alt_tls_listener_port();
		else if(port == get_alt_tls_listener_port())
			alt_port = turn_params.tls_listener_port;
		else
			return -1;

		for(i=0;i<turn_params.listener.addrs_number;i++) {
			if(turn_params.listener.encaddrs && turn_params.listener.encaddrs[i]) {
				if(addr->ss.sa_family == turn_params.listener.encaddrs[i]->ss.sa_family) {
					index=i;
					break;
				}
			}
		}
		if(index!=0xffff) {
			for(i=0;i<turn_params.listener.addrs_number;i++) {
				size_t ind = (index+i+1) % turn_params.listener.addrs_number;
				if(turn_params.listener.encaddrs && turn_params.listener.encaddrs[ind]) {
					ioa_addr *caddr = turn_params.listener.encaddrs[ind];
					if(caddr->ss.sa_family == addr->ss.sa_family) {
						addr_cpy(alt_addr,caddr);
						addr_set_port(alt_addr, alt_port);
						return 0;
					}
				}
			}
		}
	}

	return -1;
}

static void run_events(struct event_base *eb, ioa_engine_handle e)
{
	if(!eb && e)
		eb = e->event_base;

	if (!eb)
		return;

	struct timeval timeout;

	timeout.tv_sec = 5;
	timeout.tv_usec = 0;

	event_base_loopexit(eb, &timeout);

	event_base_dispatch(eb);


#if !defined(TURN_NO_HIREDIS)
	if(e)
		send_message_to_redis(e->rch, "publish", "__XXX__", "__YYY__");
#endif
}

void run_listener_server(struct listener_server *ls)
{
	unsigned int cycle = 0;
	while (!turn_params.stop_turn_server) {

		if (eve(turn_params.verbose)) {
			if ((cycle++ & 15) == 0) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "%s: cycle=%u\n", __FUNCTION__, cycle);
			}
		}

		run_events(ls->event_base, ls->ioa_eng);

		rollover_logfile();
	}
}

static void setup_relay_server(struct relay_server *rs, ioa_engine_handle e, int to_set_rfc5780)
{
	struct bufferevent *pair[2];
	int opts = BEV_OPT_DEFER_CALLBACKS | BEV_OPT_UNLOCK_CALLBACKS;

	if(e) {
		rs->event_base = e->event_base;
		rs->ioa_eng = e;
	} else {
		rs->event_base = turn_event_base_new();
		TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO,"IO method (general relay thread): %s\n",event_base_get_method(rs->event_base));
		rs->ioa_eng = create_ioa_engine(rs->sm, rs->event_base, turn_params.listener.tp, turn_params.relay_ifname,
			turn_params.relays_number, turn_params.relay_addrs, turn_params.default_relays, turn_params.verbose, turn_params.max_bps
#if !defined(TURN_NO_HIREDIS)
			,turn_params.redis_statsdb
#endif
		);
		set_ssl_ctx(rs->ioa_eng, turn_params.tls_ctx_ssl23, turn_params.tls_ctx_v1_0,
#if defined(SSL_TXT_TLSV1_1)
						turn_params.tls_ctx_v1_1,
#if defined(SSL_TXT_TLSV1_2)
						turn_params.tls_ctx_v1_2,
#endif
#endif
						turn_params.dtls_ctx);
		ioa_engine_set_rtcp_map(rs->ioa_eng, turn_params.listener.rtcpmap);
	}

	opts |= BEV_OPT_THREADSAFE;

	bufferevent_pair_new(rs->event_base, opts, pair);
	rs->in_buf = pair[0];
	rs->out_buf = pair[1];
	bufferevent_setcb(rs->in_buf, relay_receive_message, NULL, NULL, rs);
	bufferevent_enable(rs->in_buf, EV_READ);

	bufferevent_pair_new(rs->event_base, opts, pair);
	rs->auth_in_buf = pair[0];
	rs->auth_out_buf = pair[1];
	bufferevent_setcb(rs->auth_in_buf, relay_receive_auth_message, NULL, NULL, rs);
	bufferevent_enable(rs->auth_in_buf, EV_READ);

	init_turn_server(&(rs->server),
			 rs->id, turn_params.verbose,
			 rs->ioa_eng, turn_params.ct, 0,
			 turn_params.fingerprint, DONT_FRAGMENT_SUPPORTED,
			 start_user_check,
			 check_new_allocation_quota,
			 release_allocation_quota,
			 turn_params.external_ip,
			 &turn_params.no_tcp_relay,
			 &turn_params.no_udp_relay,
			 &turn_params.stale_nonce,
			 &turn_params.stun_only,
			 &turn_params.no_stun,
			 &turn_params.alternate_servers_list,
			 &turn_params.tls_alternate_servers_list,
			 &turn_params.aux_servers_list,
			 turn_params.udp_self_balance,
			 &turn_params.no_multicast_peers, &turn_params.no_loopback_peers,
			 &turn_params.ip_whitelist, &turn_params.ip_blacklist,
			 send_socket_to_relay,
			 &turn_params.secure_stun, turn_params.shatype, &turn_params.mobility,
			 turn_params.server_relay,
			 send_turn_session_info);
	
	if(to_set_rfc5780) {
		set_rfc5780(&(rs->server), get_alt_addr, send_message_from_listener_to_client);
	}

	if(turn_params.net_engine_version == NEV_UDP_SOCKET_PER_THREAD) {
		setup_tcp_listener_servers(rs->ioa_eng, rs);
	}
}

static void *run_general_relay_thread(void *arg)
{
  static int always_true = 1;
  struct relay_server *rs = (struct relay_server *)arg;
  
  int udp_reuses_the_same_relay_server = (turn_params.general_relay_servers_number<=1) || (turn_params.net_engine_version == NEV_UDP_SOCKET_PER_THREAD) || (turn_params.net_engine_version == NEV_UDP_SOCKET_PER_SESSION);

  int we_need_rfc5780 = udp_reuses_the_same_relay_server && turn_params.rfc5780;

  ignore_sigpipe();

  setup_relay_server(rs, NULL, we_need_rfc5780);

  barrier_wait();

  while(always_true) {
    run_events(rs->event_base, rs->ioa_eng);
  }
  
  return arg;
}

static void setup_general_relay_servers(void)
{
	size_t i = 0;

	general_relay_servers = (struct relay_server**)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(struct relay_server *)*get_real_general_relay_servers_number());

	for(i=0;i<get_real_general_relay_servers_number();i++) {

		if(turn_params.general_relay_servers_number == 0) {
			general_relay_servers[i] = (struct relay_server*)allocate_super_memory_engine(turn_params.listener.ioa_eng, sizeof(struct relay_server));
			general_relay_servers[i]->id = (turnserver_id)i;
			general_relay_servers[i]->sm = NULL;
			setup_relay_server(general_relay_servers[i], turn_params.listener.ioa_eng, ((turn_params.net_engine_version == NEV_UDP_SOCKET_PER_THREAD) || (turn_params.net_engine_version == NEV_UDP_SOCKET_PER_SESSION)) && turn_params.rfc5780);
			general_relay_servers[i]->thr = pthread_self();
		} else {
			super_memory_t *sm = new_super_memory_region();
			general_relay_servers[i] = (struct relay_server*)allocate_super_memory_region(sm,sizeof(struct relay_server));
			general_relay_servers[i]->id = (turnserver_id)i;
			general_relay_servers[i]->sm = sm;
			if(pthread_create(&(general_relay_servers[i]->thr), NULL, run_general_relay_thread, general_relay_servers[i])<0) {
				perror("Cannot create relay thread\n");
				exit(-1);
			}
			pthread_detach(general_relay_servers[i]->thr);
		}
	}
}

static int run_auth_server_flag = 1;

static void* run_auth_server_thread(void *arg)
{
	ignore_sigpipe();

	ns_bzero(&turn_params.authserver,sizeof(struct auth_server));

	turn_params.authserver.event_base = turn_event_base_new();
	TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO,"IO method (auth thread): %s\n",event_base_get_method(turn_params.authserver.event_base));

	struct bufferevent *pair[2];
	int opts = BEV_OPT_DEFER_CALLBACKS | BEV_OPT_UNLOCK_CALLBACKS;

	opts |= BEV_OPT_THREADSAFE;

	bufferevent_pair_new(turn_params.authserver.event_base, opts, pair);
	turn_params.authserver.in_buf = pair[0];
	turn_params.authserver.out_buf = pair[1];
	bufferevent_setcb(turn_params.authserver.in_buf, auth_server_receive_message, NULL, NULL, &turn_params.authserver);
	bufferevent_enable(turn_params.authserver.in_buf, EV_READ);

#if !defined(TURN_NO_HIREDIS)
	turn_params.authserver.rch = get_redis_async_connection(turn_params.authserver.event_base, turn_params.redis_statsdb, 1);
#endif

	struct auth_server *authserver = &turn_params.authserver;
	struct event_base *eb = authserver->event_base;

	barrier_wait();

	while(run_auth_server_flag) {
		reread_realms();
		run_events(eb,NULL);
		read_userdb_file(0);
		update_white_and_black_lists();
		auth_ping(
#if !defined(TURN_NO_HIREDIS)
		authserver->rch
#endif
		);
	}

	return arg;
}

static void setup_auth_server(void)
{
	if(pthread_create(&(turn_params.authserver.thr), NULL, run_auth_server_thread, NULL)<0) {
		perror("Cannot create auth thread\n");
		exit(-1);
	}
	pthread_detach(turn_params.authserver.thr);
}

static void* run_cli_server_thread(void *arg)
{
	ignore_sigpipe();

	setup_cli_thread();

	barrier_wait();

	while(cliserver.event_base) {
		run_events(cliserver.event_base,NULL);
	}

	return arg;
}

static void setup_cli_server(void)
{
	ns_bzero(&cliserver,sizeof(struct cli_server));
	cliserver.listen_fd = -1;
	cliserver.verbose = turn_params.verbose;

	if(pthread_create(&(cliserver.thr), NULL, run_cli_server_thread, &cliserver)<0) {
		perror("Cannot create cli thread\n");
		exit(-1);
	}

	pthread_detach(cliserver.thr);
}

void setup_server(void)
{
	evthread_use_pthreads();

#if !defined(TURN_NO_THREAD_BARRIERS)

	/* relay threads plus auth thread plus main listener thread */
	/* udp address listener thread(s) will start later */
	barrier_count = turn_params.general_relay_servers_number+2;

	if(use_cli) {
		barrier_count += 1;
	}

#endif

	setup_listener();
	allocate_relay_addrs_ports();
	setup_barriers();
	setup_general_relay_servers();

	if(turn_params.net_engine_version == NEV_UDP_SOCKET_PER_THREAD)
		setup_socket_per_thread_udp_listener_servers();
	else if(turn_params.net_engine_version == NEV_UDP_SOCKET_PER_ENDPOINT)
		setup_socket_per_endpoint_udp_listener_servers();
	else if(turn_params.net_engine_version == NEV_UDP_SOCKET_PER_SESSION)
		setup_socket_per_session_udp_listener_servers();

	if(turn_params.net_engine_version != NEV_UDP_SOCKET_PER_THREAD) {
		setup_tcp_listener_servers(turn_params.listener.ioa_eng, NULL);
	}

	setup_auth_server();
	if(use_cli)
		setup_cli_server();

	barrier_wait();
}

void init_listener(void)
{
	ns_bzero(&turn_params.listener,sizeof(struct listener_server));
}

///////////////////////////////