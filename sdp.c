/*! \file    sdp.c
 * \author   Lorenzo Miniero <lorenzo@meetecho.com>
 * \copyright GNU Affero General Public License v3
 * \brief    SDP processing
 * \details  Implementation (based on the Sofia-SDP stack) of the SDP
 * parser/merger/generator in the gateway. Each SDP coming from peers is
 * stripped/anonymized before it is passed to the plugins: all
 * DTLS/ICE/transport related information is removed, only leaving the
 * relevant information in place. SDP coming from plugins is stripped/anonymized
 * as well, and merged with the proper DTLS/ICE/transport information before
 * it is sent to the peers.
 * 
 * \ingroup protocols
 * \ref protocols
 */
 
#include "janus.h"
#include "ice.h"
#include "dtls.h"
#include "sdp.h"


static su_home_t *home = NULL;


/* SDP Initialization */
int janus_sdp_init() {
	home = su_home_new(sizeof(su_home_t));
	if(su_home_init(home) < 0) {
		JANUS_PRINT("Ops, error setting up sofia-sdp?\n");
		return -1;
	}
	return 0;
}

void janus_sdp_deinit() {
	su_home_deinit(home);
}


/* SDP parser */
void janus_sdp_free(janus_sdp *sdp) {
	if(!sdp)
		return;
	sdp_parser_t *parser = (sdp_parser_t *)sdp->parser;
	if(parser)
		sdp_parser_free(parser);
	sdp->parser = NULL;
	sdp->sdp = NULL;
	free(sdp);
	sdp = NULL;
}

/* Pre-parse SDP: is this SDP valid? how many audio/video lines? */
janus_sdp *janus_sdp_preparse(const char *jsep_sdp, int *audio, int *video) {
	sdp_parser_t *parser = sdp_parse(home, jsep_sdp, strlen(jsep_sdp), 0);
	sdp_session_t *parsed_sdp = sdp_session(parser);
	if(!parsed_sdp) {
		JANUS_DEBUG("  Error parsing SDP? %s\n", sdp_parsing_error(parser));
		sdp_parser_free(parser);
		/* Invalid SDP */
		return NULL;
	}
	sdp_media_t *m = parsed_sdp->sdp_media;
	while(m) {
		if(m->m_type == sdp_media_audio) {
			*audio = *audio + 1;
		} else if(m->m_type == sdp_media_video) {
			*video = *video + 1;
		}
		m = m->m_next;
	}
	janus_sdp *sdp = (janus_sdp *)calloc(1, sizeof(janus_sdp));
	if(sdp == NULL) {
		JANUS_DEBUG("Memory error!\n");
		return NULL;
	}

	sdp->parser = parser;
	sdp->sdp = parsed_sdp;
	return sdp;
}

/* Parse SDP */
int janus_sdp_parse(janus_ice_handle *handle, janus_sdp *sdp) {
	if(!handle || !sdp)
		return -1;
	//~ sdp_parser_t *parser = (sdp_parser_t *)sdp->parser;
	//~ if(!parser)
		//~ return -1;
	sdp_session_t *remote_sdp = (sdp_session_t *)sdp->sdp;
	if(!remote_sdp)
		return -1;
	janus_ice_stream *stream = NULL;
	janus_ice_component *component = NULL;
	gchar *ruser = NULL, *rpass = NULL, *rhashing = NULL, *rfingerprint = NULL;
	int audio = 0, video = 0;
	gint rstream = 0;
	/* Ok, let's start */
	sdp_attribute_t *a = remote_sdp->sdp_attributes;
	while(a) {
		if(a->a_name) {
			if(!strcasecmp(a->a_name, "fingerprint")) {
				JANUS_PRINT("[%"SCNu64"] Fingerprint (global) : %s\n", handle->handle_id, a->a_value);
				if(strcasestr(a->a_value, "sha-256 ") == a->a_value) {
					rhashing = g_strdup("sha-256");
					rfingerprint = g_strdup(a->a_value + strlen("sha-256 "));
				} else if(strcasestr(a->a_value, "sha-1 ") == a->a_value) {
					JANUS_PRINT("[%"SCNu64"]  Hashing algorithm not the one we expected (sha-1 instead of sha-256), but that's ok\n", handle->handle_id);
					rhashing = g_strdup("sha-1");
					rfingerprint = g_strdup(a->a_value + strlen("sha-1 "));
				} else {
					/* FIXME We should handle this somehow anyway... OpenSSL supports them all */
					JANUS_PRINT("[%"SCNu64"]  Hashing algorithm not the one we expected (sha-256/sha-1), *NOT* cool\n", handle->handle_id);
				}
			} else if(!strcasecmp(a->a_name, "ice-ufrag")) {
				JANUS_PRINT("[%"SCNu64"] ICE ufrag (global):   %s\n", handle->handle_id, a->a_value);
				ruser = g_strdup(a->a_value);
			} else if(!strcasecmp(a->a_name, "ice-pwd")) {
				JANUS_PRINT("[%"SCNu64"] ICE pwd (global):     %s\n", handle->handle_id, a->a_value);
				rpass = g_strdup(a->a_value);
			}
		}
		a = a->a_next;
	}
	sdp_media_t *m = remote_sdp->sdp_media;
	while(m) {
		/* What media type is this? */
		if(m->m_type == sdp_media_audio) {
			audio++;
			if(audio > 1) {
				m = m->m_next;
				continue;
			}
			JANUS_PRINT("[%"SCNu64"] Parsing audio candidates (stream=%d)...\n", handle->handle_id, handle->audio_id);
			rstream = handle->audio_id;
			stream = g_hash_table_lookup(handle->streams, GUINT_TO_POINTER(handle->audio_id));
		} else if(m->m_type == sdp_media_video) {
			video++;
			if(video > 1) {
				m = m->m_next;
				continue;
			}
			JANUS_PRINT("[%"SCNu64"] Parsing video candidates (stream=%d)...\n", handle->handle_id, handle->video_id);
			rstream = handle->video_id;
			stream = g_hash_table_lookup(handle->streams, GUINT_TO_POINTER(handle->video_id));
		} else {
			JANUS_PRINT("[%"SCNu64"] Skipping unsupported media line...\n", handle->handle_id);
			m = m->m_next;
			continue;
		}
		/* Look for ICE credentials and fingerprint first: check media attributes */
		a = m->m_attributes;
		while(a) {
			if(a->a_name) {
				if(!strcasecmp(a->a_name, "fingerprint")) {
					JANUS_PRINT("[%"SCNu64"] Fingerprint (local) : %s\n", handle->handle_id, a->a_value);
					if(strcasestr(a->a_value, "sha-256 ") == a->a_value) {
						if(rhashing)
							g_free(rhashing);	/* FIXME We're overwriting the global one, if any */
						rhashing = g_strdup("sha-256");
						if(rfingerprint)
							g_free(rfingerprint);	/* FIXME We're overwriting the global one, if any */
						rfingerprint = g_strdup(a->a_value + strlen("sha-256 "));
					} else if(strcasestr(a->a_value, "sha-1 ") == a->a_value) {
						JANUS_PRINT("[%"SCNu64"]  Hashing algorithm not the one we expected (sha-1 instead of sha-256), but that's ok\n", handle->handle_id);
						if(rhashing)
							g_free(rhashing);	/* FIXME We're overwriting the global one, if any */
						rhashing = g_strdup("sha-1");
						if(rfingerprint)
							g_free(rfingerprint);	/* FIXME We're overwriting the global one, if any */
						rfingerprint = g_strdup(a->a_value + strlen("sha-1 "));
					} else {
						/* FIXME We should handle this somehow anyway... OpenSSL supports them all */
						JANUS_PRINT("[%"SCNu64"]  Hashing algorithm not the one we expected (sha-256), *NOT* cool\n", handle->handle_id);
					}
				} else if(!strcasecmp(a->a_name, "setup")) {
					JANUS_PRINT("[%"SCNu64"] DTLS setup (local):  %s\n", handle->handle_id, a->a_value);
					if(!strcasecmp(a->a_value, "actpass") || !strcasecmp(a->a_value, "passive"))
						stream->dtls_role = JANUS_DTLS_ROLE_CLIENT;
					else if(!strcasecmp(a->a_value, "active"))
						stream->dtls_role = JANUS_DTLS_ROLE_SERVER;
					/* TODO Handle holdconn... */
				} else if(!strcasecmp(a->a_name, "ice-ufrag")) {
					JANUS_PRINT("[%"SCNu64"] ICE ufrag (local):   %s\n", handle->handle_id, a->a_value);
					if(ruser)
						g_free(ruser);	/* FIXME We're overwriting the global one, if any */
					ruser = g_strdup(a->a_value);
				} else if(!strcasecmp(a->a_name, "ice-pwd")) {
					JANUS_PRINT("[%"SCNu64"] ICE pwd (local):     %s\n", handle->handle_id, a->a_value);
					if(rpass)
						g_free(rpass);	/* FIXME We're overwriting the global one, if any */
					rpass = g_strdup(a->a_value);
				}
			}
			a = a->a_next;
		}
		if(!ruser || !rpass || !rfingerprint || !rhashing) {
			/* Missing mandatory information, failure... */
			if(ruser)
				g_free(ruser);
			ruser = NULL;
			if(rpass)
				g_free(rpass);
			rpass = NULL;
			if(rhashing)
				g_free(rhashing);
			rhashing = NULL;
			if(rfingerprint)
				g_free(rfingerprint);
			rfingerprint = NULL;
			return -2;
		}
		handle->remote_hashing = g_strdup(rhashing);
		handle->remote_fingerprint = g_strdup(rfingerprint);
		/* Now look for candidates and codec info */
		a = m->m_attributes;
		while(a) {
			if(a->a_name) {
				if(!strcasecmp(a->a_name, "candidate")) {
					char rfoundation[32], rtransport[4], rip[24], rtype[6], rrelip[24];
					guint32 rcomponent, rpriority, rport, rrelport;
					int res = 0;
					if((res = sscanf(a->a_value, "%31s %30u %3s %30u %23s %30u typ %5s %*s %23s %*s %30u",
						rfoundation, &rcomponent, rtransport, &rpriority,
							rip, &rport, rtype, rrelip, &rrelport)) >= 7) {
						/* Add remote candidate */
						JANUS_PRINT("[%"SCNu64"] Adding remote candidate for component %d to stream %d\n", handle->handle_id, rcomponent, rstream);
						component = g_hash_table_lookup(stream->components, GUINT_TO_POINTER(rcomponent));
						if(component == NULL) {
							JANUS_DEBUG("[%"SCNu64"] No such component %d in stream %d?\n", handle->handle_id, rcomponent, rstream);
						} else {
							component->component_id = rcomponent;
							component->stream_id = rstream;
							NiceCandidate *c = NULL;
							if(!strcasecmp(rtype, "host")) {
								JANUS_PRINT("[%"SCNu64"]  Adding host candidate... %s:%d\n", handle->handle_id, rip, rport);
								/* We only support UDP... */
								if(strcasecmp(rtransport, "udp")) {
									JANUS_DEBUG("[%"SCNu64"]    Unsupported transport %s!\n", handle->handle_id, rtransport);
								} else {
									c = nice_candidate_new(NICE_CANDIDATE_TYPE_HOST);
								}
							} else if(!strcasecmp(rtype, "srflx")) {
								JANUS_PRINT("[%"SCNu64"]  Adding srflx candidate... %s:%d --> %s:%d \n", handle->handle_id, rrelip, rrelport, rip, rport);
								/* We only support UDP... */
								if(strcasecmp(rtransport, "udp")) {
									JANUS_DEBUG("[%"SCNu64"]    Unsupported transport %s!\n", handle->handle_id, rtransport);
								}else {
									c = nice_candidate_new(NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE);
								}
							} else if(!strcasecmp(rtype, "prflx")) {
								JANUS_PRINT("[%"SCNu64"]  Adding prflx candidate... %s:%d --> %s:%d\n", handle->handle_id, rrelip, rrelport, rip, rport);
								/* We only support UDP... */
								if(strcasecmp(rtransport, "udp")) {
									JANUS_DEBUG("[%"SCNu64"]    Unsupported transport %s!\n", handle->handle_id, rtransport);
								} else {
									c = nice_candidate_new(NICE_CANDIDATE_TYPE_PEER_REFLEXIVE);
								}
							} else if(!strcasecmp(rtype, "relay")) {
								JANUS_PRINT("[%"SCNu64"]  Adding relay candidate... %s:%d --> %s:%d\n", handle->handle_id, rrelip, rrelport, rip, rport);
								/* We only support UDP/TCP/TLS... */
								if(strcasecmp(rtransport, "udp") && strcasecmp(rtransport, "tcp") && strcasecmp(rtransport, "tls")) {
									JANUS_DEBUG("[%"SCNu64"]    Unsupported transport %s!\n", handle->handle_id, rtransport);
								} else {
									c = nice_candidate_new(NICE_CANDIDATE_TYPE_RELAYED);
								}
							} else {
								/* FIXME What now? */
								JANUS_DEBUG("[%"SCNu64"]  Unknown candidate type %s!\n", handle->handle_id, rtype);
							}
							if(c != NULL) {
								c->component_id = rcomponent;
								c->stream_id = rstream;
								c->transport = NICE_CANDIDATE_TRANSPORT_UDP;
								strncpy(c->foundation, rfoundation, NICE_CANDIDATE_MAX_FOUNDATION);
								c->priority = rpriority;
								nice_address_set_from_string(&c->addr, rip);
								nice_address_set_port(&c->addr, rport);
								c->username = g_strdup(ruser);
								c->password = g_strdup(rpass);
								if(c->type == NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE || c->type == NICE_CANDIDATE_TYPE_PEER_REFLEXIVE) {
									nice_address_set_from_string(&c->base_addr, rrelip);
									nice_address_set_port(&c->base_addr, rrelport);
								} else if(c->type == NICE_CANDIDATE_TYPE_RELAYED) {
									/* FIXME Do we really need the base address for TURN? */
									nice_address_set_from_string(&c->base_addr, rrelip);
									nice_address_set_port(&c->base_addr, rrelport);
								}
								component->candidates = g_slist_append(component->candidates, c);
								JANUS_PRINT("[%"SCNu64"]    Candidate added to the list! (%u elements for %d/%d)\n", handle->handle_id,
									g_slist_length(component->candidates), stream->stream_id, component->component_id);
							}
						}
					} else {
						JANUS_DEBUG("[%"SCNu64"] Failed to parse candidate... (%d)\n", handle->handle_id, res);
					}
				}
			}
			a = a->a_next;
		}
		m = m->m_next;
	}
	if(ruser)
		g_free(ruser);
	ruser = NULL;
	if(rpass)
		g_free(rpass);
	rpass = NULL;
	if(rhashing)
		g_free(rhashing);
	rhashing = NULL;
	if(rfingerprint)
		g_free(rfingerprint);
	rfingerprint = NULL;
	return 0;	/* FIXME Handle errors better */
}

char *janus_sdp_anonymize(const char *sdp) {
	if(sdp == NULL)
		return NULL;
	//~ su_home_t home[1] = { SU_HOME_INIT(home) };
	sdp_session_t *anon = NULL;
	sdp_parser_t *parser = sdp_parse(home, sdp, strlen(sdp), 0);
	if(!(anon = sdp_session(parser))) {
		JANUS_DEBUG("Error parsing/merging SDP: %s\n", sdp_parsing_error(parser));
		return NULL;
	}
		//~ /* o= */
	//~ if(anon->sdp_origin && anon->sdp_origin->o_username) {
		//~ free(anon->sdp_origin->o_username);
		//~ anon->sdp_origin->o_username = strdup("JANUS");
	//~ }
		/* c= */
	if(anon->sdp_connection && anon->sdp_connection->c_address) {
		//~ free(anon->sdp_connection->c_address);
		anon->sdp_connection->c_address = strdup("1.1.1.1");
	}
		/* a= */
	if(anon->sdp_attributes) {
		/* FIXME These are attributes we handle ourselves, the plugins don't need them */
		while(sdp_attribute_find(anon->sdp_attributes, "ice-ufrag"))
			sdp_attribute_remove(&anon->sdp_attributes, "ice-ufrag");
		while(sdp_attribute_find(anon->sdp_attributes, "ice-pwd"))
			sdp_attribute_remove(&anon->sdp_attributes, "ice-pwd");
		while(sdp_attribute_find(anon->sdp_attributes, "ice-options"))
			sdp_attribute_remove(&anon->sdp_attributes, "ice-options");
		while(sdp_attribute_find(anon->sdp_attributes, "fingerprint"))
			sdp_attribute_remove(&anon->sdp_attributes, "fingerprint");
		while(sdp_attribute_find(anon->sdp_attributes, "group"))
			sdp_attribute_remove(&anon->sdp_attributes, "group");
		while(sdp_attribute_find(anon->sdp_attributes, "msid-semantic"))
			sdp_attribute_remove(&anon->sdp_attributes, "msid-semantic");
	}
		/* m= */
	int a_sendrecv = 0, v_sendrecv = 0;
	if(anon->sdp_media) {
		int audio = 0, video = 0;
		sdp_media_t *m = anon->sdp_media;
		while(m) {
			if(m->m_type == sdp_media_audio) {
				audio++;
				m->m_port = audio == 1 ? 1 : 0;
			} else if(m->m_type == sdp_media_video) {
				video++;
				m->m_port = audio == 1 ? 1 : 0;
			} else {
				m->m_port = 0;
			}
				/* c= */
			if(m->m_connections) {
				sdp_connection_t *c = m->m_connections;
				while(c) {
					if(c->c_address) {
						//~ free(c->c_address);
						c->c_address = strdup("1.1.1.1");
					}
					c = c->c_next;
				}
			}
				/* a= */
			if(m->m_attributes) {
				/* FIXME These are attributes we handle ourselves, the plugins don't need them */
				while(sdp_attribute_find(m->m_attributes, "ice-ufrag"))
					sdp_attribute_remove(&m->m_attributes, "ice-ufrag");
				while(sdp_attribute_find(m->m_attributes, "ice-pwd"))
					sdp_attribute_remove(&m->m_attributes, "ice-pwd");
				while(sdp_attribute_find(m->m_attributes, "ice-options"))
					sdp_attribute_remove(&m->m_attributes, "ice-options");
				while(sdp_attribute_find(m->m_attributes, "crypto"))
					sdp_attribute_remove(&m->m_attributes, "crypto");
				while(sdp_attribute_find(m->m_attributes, "fingerprint"))
					sdp_attribute_remove(&m->m_attributes, "fingerprint");
				while(sdp_attribute_find(m->m_attributes, "setup"))
					sdp_attribute_remove(&m->m_attributes, "setup");
				while(sdp_attribute_find(m->m_attributes, "connection"))
					sdp_attribute_remove(&m->m_attributes, "connection");
				while(sdp_attribute_find(m->m_attributes, "group"))
					sdp_attribute_remove(&m->m_attributes, "group");
				while(sdp_attribute_find(m->m_attributes, "msid-semantic"))
					sdp_attribute_remove(&m->m_attributes, "msid-semantic");
				while(sdp_attribute_find(m->m_attributes, "rtcp"))
					sdp_attribute_remove(&m->m_attributes, "rtcp");
				while(sdp_attribute_find(m->m_attributes, "rtcp-mux"))
					sdp_attribute_remove(&m->m_attributes, "rtcp-mux");
				while(sdp_attribute_find(m->m_attributes, "candidate"))
					sdp_attribute_remove(&m->m_attributes, "candidate");
				while(sdp_attribute_find(m->m_attributes, "ssrc"))
					sdp_attribute_remove(&m->m_attributes, "ssrc");
				while(sdp_attribute_find(m->m_attributes, "extmap"))	/* TODO Actually implement RTP extensions */
					sdp_attribute_remove(&m->m_attributes, "extmap");
			}
			/* FIXME sendrecv hack: sofia-sdp doesn't print sendrecv, but we want it to */
			if(m->m_mode == sdp_sendrecv) {
				m->m_mode = sdp_inactive;
				if(m->m_type == sdp_media_audio)
					a_sendrecv = 1;
				else if(m->m_type == sdp_media_video)
					v_sendrecv = 1;
			}
			m = m->m_next;
		}
	}
	char buf[BUFSIZE];
	sdp_printer_t *printer = sdp_print(home, anon, buf, BUFSIZE, 0);
	if(sdp_message(printer)) {
		int retval = sdp_message_size(printer);
		sdp_printer_free(printer);
		/* FIXME Take care of the sendrecv hack */
		if(a_sendrecv || v_sendrecv) {
			char *replace = strstr(buf, "a=inactive");
			while(replace != NULL) {
				memcpy(replace, "a=sendrecv", strlen("a=sendrecv"));
				replace++;
				replace = strstr(replace, "a=inactive");
			}
		}
		JANUS_PRINT(" -------------------------------------------\n");
		JANUS_PRINT("  >> Anonymized (%zu --> %d bytes)\n", strlen(sdp), retval);
		JANUS_PRINT(" -------------------------------------------\n");
		JANUS_PRINT("%s\n", buf);
		return g_strdup(buf);
	} else {
		JANUS_DEBUG("Error anonymizing SDP: %s\n", sdp_printing_error(printer));
		return NULL;
	}
}

char *janus_sdp_merge(janus_ice_handle *handle, const char *origsdp) {
	if(handle == NULL || origsdp == NULL)
		return NULL;
	//~ su_home_t home[1] = { SU_HOME_INIT(home) };
	sdp_session_t *anon = NULL;
	sdp_parser_t *parser = sdp_parse(home, origsdp, strlen(origsdp), 0);
	if(!(anon = sdp_session(parser))) {
		JANUS_DEBUG("[%"SCNu64"] Error parsing/merging SDP: %s\n", handle->handle_id, sdp_parsing_error(parser));
		return NULL;
	}
	/* Prepare SDP to merge */
	gchar buffer[200];
	memset(buffer, 0, 200);
	char *sdp = (char*)calloc(BUFSIZE, sizeof(char));
	if(sdp == NULL) {
		JANUS_DEBUG("Memory error!\n");
		return NULL;
	}
	sdp[0] = '\0';
	/* Version v= */
	g_strlcat(sdp,
		"v=0\r\n", BUFSIZE);
	/* Origin o= */
	if(anon->sdp_origin) {
		g_sprintf(buffer,
			"o=%s %"SCNu64" %"SCNu64" IN IP4 127.0.0.1\r\n",	/* FIXME Should we fix the address? */
				anon->sdp_origin->o_username ? anon->sdp_origin->o_username : "-",
				anon->sdp_origin->o_id, anon->sdp_origin->o_version);
		g_strlcat(sdp, buffer, BUFSIZE);
	} else {
		gint64 sessid = g_get_monotonic_time();
		gint64 version = sessid;	/* FIXME This needs to be increased when it changes, so time should be ok */
		g_sprintf(buffer,
			"o=%s %"SCNi64" %"SCNi64" IN IP4 127.0.0.1\r\n",	/* FIXME Should we fix the address? */
				"-", sessid, version);
		g_strlcat(sdp, buffer, BUFSIZE);
	}
	/* Session name s= */
	g_sprintf(buffer,
		"s=%s\r\n", anon->sdp_subject ? anon->sdp_subject : "Meetecho Janus");
	g_strlcat(sdp, buffer, BUFSIZE);
	/* Timing t= */
	g_sprintf(buffer,
		"t=%lu %lu\r\n", anon->sdp_time ? anon->sdp_time->t_start : 0, anon->sdp_time ? anon->sdp_time->t_stop : 0);
	g_strlcat(sdp, buffer, BUFSIZE);
	/* Any global bandwidth? */
	//~ if(anon->sdp_bandwidths) {
		//~ g_sprintf(buffer,
			//~ "b=%s:%"SCNu64"\r\n",
				//~ anon->sdp_bandwidths->b_modifier_name ? anon->sdp_bandwidths->b_modifier_name : "AS",
				//~ anon->sdp_bandwidths->b_value);
		//~ g_strlcat(sdp, buffer, BUFSIZE);
	//~ }
	/* msid-semantic: add new global attribute */
	g_strlcat(sdp,
		"a=msid-semantic: WMS janus\r\n",
		BUFSIZE);
	//~ /* Connection c= (global) */
	//~ if(anon->sdp_connection) {
		//~ g_sprintf(buffer,
			//~ "c=IN IP4 %s\r\n", janus_get_local_ip());
		//~ g_strlcat(sdp, buffer, BUFSIZE);
	//~ }
	/* DTLS fingerprint a= (global) */
	g_sprintf(buffer,
		"a=fingerprint:sha-256 %s\r\n", janus_dtls_get_local_fingerprint());
	g_strlcat(sdp, buffer, BUFSIZE);
	/* Copy other global attributes, if any */
	if(anon->sdp_attributes) {
		sdp_attribute_t *a = anon->sdp_attributes;
		while(a) {
			if(a->a_value == NULL) {
				g_sprintf(buffer,
					"a=%s\r\n", a->a_name);
				g_strlcat(sdp, buffer, BUFSIZE);
			} else {
				g_sprintf(buffer,
					"a=%s:%s\r\n", a->a_name, a->a_value);
				g_strlcat(sdp, buffer, BUFSIZE);
			}
			a = a->a_next;
		}
	}
	/* Media lines now */
	if(anon->sdp_media) {
		int audio = 0, video = 0;
		sdp_media_t *m = anon->sdp_media;
		janus_ice_stream *stream = NULL;
		while(m) {
			if(m->m_type == sdp_media_audio) {
				audio++;
				if(audio > 1 || !handle->audio_id) {
					JANUS_DEBUG("[%"SCNu64"] Skipping audio line (we have %d audio lines, and the id is %d)\n", handle->handle_id, audio, handle->audio_id);
					g_strlcat(sdp, "m=audio 0 RTP/SAVPF 0\r\n", BUFSIZE);
					m = m->m_next;
					continue;
				}
				/* Audio */
				stream = g_hash_table_lookup(handle->streams, GUINT_TO_POINTER(handle->audio_id));
				if(stream == NULL) {
					JANUS_DEBUG("[%"SCNu64"] Skipping audio line (invalid stream %d)\n", handle->handle_id, handle->audio_id);
					g_strlcat(sdp, "m=audio 0 RTP/SAVPF 0\r\n", BUFSIZE);
					m = m->m_next;
					continue;
				}
				g_strlcat(sdp, "m=audio ARTPP RTP/SAVPF", BUFSIZE);
			} else if(m->m_type == sdp_media_video) {
				video++;
				if(video > 1 || !handle->video_id) {
					JANUS_DEBUG("[%"SCNu64"] Skipping video line (we have %d video lines, and the id is %d)\n", handle->handle_id, video, handle->video_id);
					g_strlcat(sdp, "m=video 0 RTP/SAVPF 0\r\n", BUFSIZE);
					m = m->m_next;
					continue;
				}
				/* Video */
				stream = g_hash_table_lookup(handle->streams, GUINT_TO_POINTER(handle->video_id));
				if(stream == NULL) {
					JANUS_DEBUG("[%"SCNu64"] Skipping video line (invalid stream %d)\n", handle->handle_id, handle->audio_id);
					g_strlcat(sdp, "m=video 0 RTP/SAVPF 0\r\n", BUFSIZE);
					m = m->m_next;
					continue;
				}
				g_strlcat(sdp, "m=video VRTPP RTP/SAVPF", BUFSIZE);
			} else {
				JANUS_DEBUG("[%"SCNu64"] Skipping unsupported media line...\n", handle->handle_id);
				g_sprintf(buffer,
					"m=%s 0 %s 0\r\n",
					m->m_type_name, m->m_proto_name);
				g_strlcat(sdp, buffer, BUFSIZE);
				m = m->m_next;
				continue;
			}
			/* Add formats now */
			if(!m->m_rtpmaps) {
				JANUS_PRINT("[%"SCNu64"] No RTP maps?? trying formats...\n", handle->handle_id);
				if(!m->m_format) {
					JANUS_DEBUG("[%"SCNu64"] No formats either?? this sucks!\n", handle->handle_id);
					g_strlcat(sdp, " 0", BUFSIZE);	/* FIXME Won't work apparently */
				} else {
					sdp_list_t *fmt = m->m_format;
					while(fmt) {
						g_sprintf(buffer, " %s", fmt->l_text);
						g_strlcat(sdp, buffer, BUFSIZE);
						fmt = fmt->l_next;
					}
				}
			} else {
				sdp_rtpmap_t *r = m->m_rtpmaps;
				while(r) {
					g_sprintf(buffer, " %d", r->rm_pt);
					g_strlcat(sdp, buffer, BUFSIZE);
					r = r->rm_next;
				}
			}
			g_strlcat(sdp, "\r\n", BUFSIZE);
			/* Any bandwidth? */
			if(m->m_bandwidths) {
				g_sprintf(buffer,
					"b=%s:%lu\r\n",	/* FIXME Are we doing this correctly? */
						m->m_bandwidths->b_modifier_name ? m->m_bandwidths->b_modifier_name : "AS",
						m->m_bandwidths->b_value);
				g_strlcat(sdp, buffer, BUFSIZE);
			}
			/* Media connection c= */
			//~ if(m->m_connections) {
				g_sprintf(buffer,
					"c=IN IP4 %s\r\n", janus_get_local_ip());
				g_strlcat(sdp, buffer, BUFSIZE);
			//~ }
			/* What is the direction? */
			switch(m->m_mode) {
				case sdp_inactive:
					g_strlcat(sdp, "a=inactive\r\n", BUFSIZE);
					break;
				case sdp_sendonly:
					g_strlcat(sdp, "a=sendonly\r\n", BUFSIZE);
					break;
				case sdp_recvonly:
					g_strlcat(sdp, "a=recvonly\r\n", BUFSIZE);
					break;
				case sdp_sendrecv:
				default:
					g_strlcat(sdp, "a=sendrecv\r\n", BUFSIZE);
					break;
			}
			/* RTCP */
			g_sprintf(buffer, "a=rtcp:%s IN IP4 %s\r\n",
				m->m_type == sdp_media_audio ? "ARTCP" : "VRTCP", janus_get_local_ip());
			g_strlcat(sdp, buffer, BUFSIZE);
			/* RTP maps */
			if(m->m_rtpmaps) {
				sdp_rtpmap_t *rm = NULL;
				for(rm = m->m_rtpmaps; rm; rm = rm->rm_next) {
					g_sprintf(buffer, "a=rtpmap:%u %s/%lu%s%s\r\n",
						rm->rm_pt, rm->rm_encoding, rm->rm_rate,
						rm->rm_params ? "/" : "", 
						rm->rm_params ? rm->rm_params : "");
					g_strlcat(sdp, buffer, BUFSIZE);
				}
				for(rm = m->m_rtpmaps; rm; rm = rm->rm_next) {
					if(rm->rm_fmtp) {
						g_sprintf(buffer, "a=fmtp:%u %s\r\n", rm->rm_pt, rm->rm_fmtp);
						g_strlcat(sdp, buffer, BUFSIZE);
					}
				}
			}
			/* ICE ufrag and pwd, DTLS setup and connection a= */
			gchar *ufrag = NULL;
			gchar *password = NULL;
			nice_agent_get_local_credentials(handle->agent, stream->stream_id, &ufrag, &password);
			memset(buffer, 0, 100);
			g_sprintf(buffer,
				"a=ice-ufrag:%s\r\n"
				"a=ice-pwd:%s\r\n"
				"a=setup:%s\r\n"
				"a=connection:new\r\n",
					ufrag, password,
					janus_get_dtls_srtp_role(stream->dtls_role));
			g_strlcat(sdp, buffer, BUFSIZE);
			/* Copy existing media attributes, if any */
			if(m->m_attributes) {
				sdp_attribute_t *a = m->m_attributes;
				while(a) {
					if(a->a_value == NULL) {
						g_sprintf(buffer,
							"a=%s\r\n", a->a_name);
						g_strlcat(sdp, buffer, BUFSIZE);
					} else {
						g_sprintf(buffer,
							"a=%s:%s\r\n", a->a_name, a->a_value);
						g_strlcat(sdp, buffer, BUFSIZE);
					}
					a = a->a_next;
				}
			}
			/* Add last attributes, rtcp and ssrc (msid) */
			if(m->m_type == sdp_media_audio) {
				g_sprintf(buffer,
					//~ "a=rtcp:ARTCP IN IP4 %s\r\n"
					"a=ssrc:%i cname:janusaudio\r\n"
					"a=ssrc:%i msid:janus janusa0\r\n"
					"a=ssrc:%i mslabel:janus\r\n"
					"a=ssrc:%i label:janusa0\r\n",
						//~ janus_get_local_ip(),
						stream->ssrc, stream->ssrc, stream->ssrc, stream->ssrc);
				g_strlcat(sdp, buffer, BUFSIZE);
			} else if(m->m_type == sdp_media_video) {
				g_sprintf(buffer,
					//~ "a=rtcp:VRTCP IN IP4 %s\r\n"
					"a=ssrc:%i cname:janusvideo\r\n"
					"a=ssrc:%i msid:janus janusv0\r\n"
					"a=ssrc:%i mslabel:janus\r\n"
					"a=ssrc:%i label:janusv0\r\n",
						//~ janus_get_local_ip(),
						stream->ssrc, stream->ssrc, stream->ssrc, stream->ssrc);
				g_strlcat(sdp, buffer, BUFSIZE);
			}
			/* And now the candidates */
			janus_ice_setup_candidate(handle, sdp, stream->stream_id, 1);
			janus_ice_setup_candidate(handle, sdp, stream->stream_id, 2);
			/* Next */
			m = m->m_next;
		}
	}
	JANUS_PRINT(" -------------------------------------------\n");
	JANUS_PRINT("  >> Merged (%zu --> %zu bytes)\n", strlen(origsdp), strlen(sdp));
	JANUS_PRINT(" -------------------------------------------\n");
	JANUS_PRINT("%s\n", sdp);
	return sdp;
}
